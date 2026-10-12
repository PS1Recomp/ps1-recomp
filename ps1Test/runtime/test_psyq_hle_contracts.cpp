// Contracts of the PsyQ HLEs Crash Bandicoot routes to that had no test of
// their own: what each one must return and which state it may touch.  The
// libetc interrupt module and libgpu's internal executors carry real
// behaviour; the rest are deliberate no-ops, and their contract -- return 0,
// leave memory and the GPU alone -- is pinned here too, so promoting one to a
// real implementation is a visible decision rather than a silent drift.

#include "runtime/cpu_context.h"
#include "runtime/memory.h"
#include "runtime/psyq/psyq_hle.h"
#include "runtime/psyq/psyq_libetc.h"
#include "runtime/psyq/psyq_libgpu.h"
#include "runtime/psyq/psyq_registry.h"
#include "runtime/psyq/psyq_state.h"
#include "runtime/timers/timers.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

using namespace ps1;
using namespace ps1::psyq;

namespace {

class PsyqHleContractTest : public ::testing::Test {
protected:
  Memory mem;
  recomp_context ctx;
  std::vector<uint32_t> gp0;

  void SetUp() override {
    ctx.reset();
    ctx.mem = &mem;
    gp0.clear();
    HleConfig cfg;
    cfg.writeGP0 = [this](uint32_t w) { gp0.push_back(w); };
    cfg.writeGP1 = [](uint32_t) {};
    configure(cfg);
    psyq_state().reset();
    psyq_registry_init_defaults();
    psyq_register_libgpu_extras();
    psyq_register_libetc_intr();
  }

  void TearDown() override { psyq_state().reset(); }

  void writeRect(uint32_t p, int16_t x, int16_t y, int16_t w, int16_t h) {
    mem.write16(p + 0, static_cast<uint16_t>(x));
    mem.write16(p + 2, static_cast<uint16_t>(y));
    mem.write16(p + 4, static_cast<uint16_t>(w));
    mem.write16(p + 6, static_cast<uint16_t>(h));
  }
};

} // namespace

// libetc interrupt module (psyz decomp/src/libetc/intr.c)

// InterruptCallback(n, fn) stores the handler for IRQ line n and returns the
// one it replaces; the drain then ticks it.  libsnd installs the music
// sequencer on line 0 this way.
TEST_F(PsyqHleContractTest, InterruptCallbackStoresTheHandlerAndReturnsThePrevious) {
  ctx.r[A0] = 0;
  ctx.r[A1] = 0x8004B828;
  psyq_dispatch("libetc_InterruptCallback", &ctx);
  EXPECT_EQ(ctx.r[V0], 0u);
  EXPECT_EQ(psyq_state().intrCallback[0], 0x8004B828u);

  ctx.r[A0] = 0;
  ctx.r[A1] = 0;
  psyq_dispatch("libetc_InterruptCallback", &ctx);
  EXPECT_EQ(ctx.r[V0], 0x8004B828u) << "previous handler";
  EXPECT_EQ(psyq_state().intrCallback[0], 0u);
}

TEST_F(PsyqHleContractTest, DmaCallbackUsesTheDmaTableNotTheIrqTable) {
  ctx.r[A0] = 4; // channel 4: SPU
  ctx.r[A1] = 0x800466A0;
  psyq_dispatch("libetc_DMACallback", &ctx);
  EXPECT_EQ(ctx.r[V0], 0u);
  EXPECT_EQ(psyq_state().dmaCallback[4], 0x800466A0u);
  EXPECT_EQ(psyq_state().intrCallback[4], 0u) << "IRQ line 4 is RCnt0, not SPU DMA";
}

// SetIntrMask(mask) writes I_MASK and returns the previous mask.
TEST_F(PsyqHleContractTest, SetIntrMaskRoundTripsTheMask) {
  InterruptController irq; // I_MASK lives in the interrupt controller
  mem.setInterruptController(&irq);
  ctx.r[A0] = 0x1234;
  psyq_dispatch("libetc_SetIntrMask", &ctx);
  ctx.r[A0] = 0x0055;
  psyq_dispatch("libetc_SetIntrMask", &ctx);
  EXPECT_EQ(ctx.r[V0], 0x1234u) << "previous mask";
  EXPECT_EQ(psyq_state().intrMask, 0x0055u);
  EXPECT_EQ(irq.readIMask(), 0x0055u) << "written through to I_MASK";
  mem.setInterruptController(nullptr);
}

// start/stop/restart are the vtable entries ResetCallback/StopCallback/
// RestartCallback call through: start enables and returns non-zero (PsyQ
// boilerplate aborts on zero), stop/restart return the previous state.
TEST_F(PsyqHleContractTest, StartStopRestartIntrToggleCallbacks) {
  psyq_dispatch("libetc_startIntr", &ctx);
  EXPECT_NE(ctx.r[V0], 0u);
  EXPECT_TRUE(psyq_state().callbacksEnabled);

  psyq_dispatch("libetc_stopIntr", &ctx);
  EXPECT_EQ(ctx.r[V0], 1u) << "was enabled";
  EXPECT_FALSE(psyq_state().callbacksEnabled);

  psyq_dispatch("libetc_restartIntr", &ctx);
  EXPECT_EQ(ctx.r[V0], 0u) << "was disabled";
  EXPECT_TRUE(psyq_state().callbacksEnabled);
}

// startIntr must not forget registered handlers: PadInit re-runs it, and
// wiping the table once stopped Crash's sound tick (level loads hung).
TEST_F(PsyqHleContractTest, StartIntrKeepsRegisteredHandlers) {
  psyq_state().intrCallback[0] = 0x8004B828;
  psyq_state().dmaCallback[4] = 0x800466A0;
  psyq_dispatch("libetc_startIntr", &ctx);
  EXPECT_EQ(psyq_state().intrCallback[0], 0x8004B828u);
  EXPECT_EQ(psyq_state().dmaCallback[4], 0x800466A0u);
}

// libgpu internal executors

// _clr(rect*, color): the executor behind ClearImage -- one GP0(02h) fill.
TEST_F(PsyqHleContractTest, ClrEmitsOneFillRect) {
  writeRect(0x80100000, 0, 12, 512, 216);
  ctx.r[A0] = 0x80100000;
  ctx.r[A1] = 0x80332211; // bit 31 (semi-transparent variant) is masked off
  psyq_dispatch("libgpu__clr", &ctx);
  ASSERT_EQ(gp0.size(), 3u);
  EXPECT_EQ(gp0[0], 0x02332211u);
  EXPECT_EQ(gp0[1], (12u << 16) | 0u);
  EXPECT_EQ(gp0[2], (216u << 16) | 512u);
  EXPECT_EQ(ctx.r[V0], 0u);
}

TEST_F(PsyqHleContractTest, ClrWithAnEmptyRectEmitsNothing) {
  writeRect(0x80100000, 0, 0, 512, 0);
  ctx.r[A0] = 0x80100000;
  ctx.r[A1] = 0;
  psyq_dispatch("libgpu__clr", &ctx);
  EXPECT_TRUE(gp0.empty());
  EXPECT_EQ(ctx.r[V0], 0u);
}

// _drs(rect*, dst*): the executor behind StoreImage -- same GP0(C0h) header,
// and returns 0.
TEST_F(PsyqHleContractTest, DrsIssuesTheStoreImageHeader) {
  writeRect(0x80100000, 64, 240, 16, 8);
  ctx.r[A0] = 0x80100000;
  ctx.r[A1] = 0x80100200;
  ctx.r[V0] = 0xDEAD;
  psyq_dispatch("libgpu__drs", &ctx);
  ASSERT_GE(gp0.size(), 3u);
  EXPECT_EQ(gp0[0], 0xC0000000u);
  EXPECT_EQ(gp0[1], (240u << 16) | 64u);
  EXPECT_EQ(gp0[2], (8u << 16) | 16u);
  EXPECT_EQ(ctx.r[V0], 0u);
}

// Deliberate no-ops

// checkRECT only validates in debug builds of libgpu; it must not draw or
// write.
TEST_F(PsyqHleContractTest, CheckRectHasNoSideEffects) {
  writeRect(0x80100000, 1, 2, 3, 4);
  ctx.r[A0] = 0x80100000;
  psyq_dispatch("libgpu_checkRECT", &ctx);
  EXPECT_TRUE(gp0.empty());
  EXPECT_EQ(mem.read16(0x80100004), 3u);
}

// Root counters are modelled by the runtime's timers and by
// `[timing] rcnt_ticks_per_vblank`; the libapi calls accept and discard their
// arguments.  _reset and DrawPrim have no observable effect in Crash's boot
// path.  All five return 0 and touch neither memory nor the GPU.
class PsyqNoopStubTest : public PsyqHleContractTest,
                         public ::testing::WithParamInterface<const char *> {};

TEST_P(PsyqNoopStubTest, ReturnsZeroAndTouchesNothing) {
  mem.write32(0x80100000, 0xCAFEBABE);
  ctx.r[A0] = 0xF2000002;
  ctx.r[A1] = 0x80100000;
  ctx.r[A2] = 0x1000;
  ctx.r[V0] = 0x1234;
  psyq_dispatch(GetParam(), &ctx);
  EXPECT_EQ(ctx.r[V0], 0u);
  EXPECT_TRUE(gp0.empty());
  EXPECT_EQ(mem.read32(0x80100000), 0xCAFEBABEu);
}

INSTANTIATE_TEST_SUITE_P(CrashNoops, PsyqNoopStubTest,
                         ::testing::Values("libapi_SetRCnt", "libapi_StartRCnt",
                                           "libapi_StopRCnt", "libgpu__reset",
                                           "libgpu_DrawPrim"));

// The rest of the libetc callback API (intr.c): ResetCallback starts the
// module (non-zero = initialised) without forgetting handlers; Stop/Restart
// return the previous state; CheckCallback is 0 outside a callback; GetIntrMask
// reads back what SetIntrMask wrote.
TEST_F(PsyqHleContractTest, CallbackControlRoundTrips) {
  psyq_state().intrCallback[0] = 0x8004B828;
  psyq_dispatch("libetc_ResetCallback", &ctx);
  EXPECT_NE(ctx.r[V0], 0u);
  EXPECT_EQ(psyq_state().intrCallback[0], 0x8004B828u);

  psyq_dispatch("libetc_StopCallback", &ctx);
  EXPECT_EQ(ctx.r[V0], 1u);
  psyq_dispatch("libetc_RestartCallback", &ctx);
  EXPECT_EQ(ctx.r[V0], 0u);

  psyq_dispatch("libetc_CheckCallback", &ctx);
  EXPECT_EQ(ctx.r[V0], 0u) << "never inside a callback from the game thread";

  ctx.r[A0] = 0x0021;
  psyq_dispatch("libetc_SetIntrMask", &ctx);
  psyq_dispatch("libetc_GetIntrMask", &ctx);
  EXPECT_EQ(ctx.r[V0], 0x0021u);
}

INSTANTIATE_TEST_SUITE_P(OtherNoops, PsyqNoopStubTest,
                         ::testing::Values("libgpu_TermPrim",
                                           "libcd_StCdInterrupt2"));
