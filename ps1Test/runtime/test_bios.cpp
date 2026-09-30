#include <vector>
#include "runtime/bios/bios.h"
#include "runtime/psyq/psyq_state.h"
#include "runtime/cdrom/virtual_fs.h"
#include "runtime/cpu_context.h"
#include "runtime/memory.h"
#include <gtest/gtest.h>
#include <string>

using namespace ps1::bios;
using namespace ps1::cdrom;
using namespace ps1;

class BiosTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Set up execution context
    ctx.mem = &mem;
    // A yield point only dispatches when the guest has a usable stack, so
    // give the fixture one -- the top of main RAM, where PS1 stacks live.
    ctx.r29 = 0x801FFFF0;

    fs = std::make_unique<VirtualFs>();
    bios = std::make_unique<Bios>(ctx, *fs, mem);
  }

  // Write a string to emulated memory
  void writeString(uint32_t addr, const std::string &str) {
    for (size_t i = 0; i < str.length(); ++i) {
      mem.write8(addr + i, str[i]);
    }
    mem.write8(addr + str.length(), '\0');
  }

  // Read a string from emulated memory
  std::string readString(uint32_t addr) {
    std::string str;
    char c;
    while ((c = mem.read8(addr++)) != '\0') {
      str += c;
    }
    return str;
  }

  Memory mem;
  recomp_context ctx{};
  std::unique_ptr<VirtualFs> fs;
  std::unique_ptr<Bios> bios;
};

TEST_F(BiosTest, StrcmpReturnsCorrectly) {
  writeString(0x80000000, "apple");
  writeString(0x80000100, "apple");
  writeString(0x80000200, "banana");

  // Test equal
  ctx.r[T1] = 0x17; // strcmp A0
  ctx.r[A0] = 0x80000000;
  ctx.r[A1] = 0x80000100;
  bios->executeA0();
  EXPECT_EQ(ctx.r[V0], 0);

  // Test not equal
  ctx.r[T1] = 0x17;
  ctx.r[A0] = 0x80000000;
  ctx.r[A1] = 0x80000200;
  bios->executeA0();
  EXPECT_LT(static_cast<int32_t>(ctx.r[V0]), 0);
}

TEST_F(BiosTest, StrcpyCopiesString) {
  writeString(0x80000000, "hello world");

  ctx.r[T1] = 0x19;       // strcpy A0
  ctx.r[A0] = 0x80000100; // dest
  ctx.r[A1] = 0x80000000; // src
  bios->executeA0();

  EXPECT_EQ(ctx.r[V0], 0x80000100);
  EXPECT_EQ(readString(0x80000100), "hello world");
}

TEST_F(BiosTest, StrlenCountsCorrectly) {
  writeString(0x80000000, "test length");

  ctx.r[T1] = 0x1B; // strlen A0
  ctx.r[A0] = 0x80000000;
  bios->executeA0();

  EXPECT_EQ(ctx.r[V0], 11);
}

TEST_F(BiosTest, MemcpyCopiesBytes) {
  mem.write8(0x80000000, 0xAA);
  mem.write8(0x80000001, 0xBB);
  mem.write8(0x80000002, 0xCC);

  ctx.r[T1] = 0x2A;       // memcpy A0
  ctx.r[A0] = 0x80000100; // dst
  ctx.r[A1] = 0x80000000; // src
  ctx.r[A2] = 3;          // len
  bios->executeA0();

  EXPECT_EQ(ctx.r[V0], 0x80000100);
  EXPECT_EQ(mem.read8(0x80000100), 0xAA);
  EXPECT_EQ(mem.read8(0x80000101), 0xBB);
  EXPECT_EQ(mem.read8(0x80000102), 0xCC);
}

TEST_F(BiosTest, MemsetFillsBytes) {
  ctx.r[T1] = 0x2B;       // memset A0
  ctx.r[A0] = 0x80000000; // dst
  ctx.r[A1] = 0x55;       // val (only lower 8 bits used)
  ctx.r[A2] = 4;          // len
  bios->executeA0();

  EXPECT_EQ(ctx.r[V0], 0x80000000);
  EXPECT_EQ(mem.read8(0x80000000), 0x55);
  EXPECT_EQ(mem.read8(0x80000001), 0x55);
  EXPECT_EQ(mem.read8(0x80000002), 0x55);
  EXPECT_EQ(mem.read8(0x80000003), 0x55);
}

// Re-entrant drain
//
// A dispatched callback is recompiled game code, and the recompiler injects a
// drain at every backward branch -- so a callback with a loop in it calls back
// into drainPendingCallbacks and gets dispatched again. Measured in Crash
// Bandicoot as 561 million nested calls against 153 thousand real yield points,
// with the game thread never returning to its main loop.
//
// Here the dispatch hook stands in for that callback: it bumps the VSync
// counter (so the interrupt tick would re-queue) and re-enters the drain.
// Without the guard this recurses until the stack gives out.
extern void (*g_testRecompDispatchHook)(recomp_context *ctx, uint32_t addr);

namespace {
Bios *g_reentryBios = nullptr;
int g_reentryDispatches = 0;

void reentrantHook(recomp_context * /*ctx*/, uint32_t /*addr*/) {
  if (++g_reentryDispatches > 50)
    return; // runaway guard for the failing case
  ps1::psyq::psyq_state().vsyncCounter.fetch_add(1, std::memory_order_release);
  g_reentryBios->drainPendingCallbacks();
}
} // namespace

TEST_F(BiosTest, DrainPendingCallbacksRefusesToReenter) {
  auto &st = ps1::psyq::psyq_state();
  st.reset();
  st.intrCallback[4] = 0x80046000;
  st.vsyncCounter.store(1, std::memory_order_release);

  g_reentryBios = bios.get();
  g_reentryDispatches = 0;
  g_testRecompDispatchHook = &reentrantHook;

  bios->drainPendingCallbacks();

  g_testRecompDispatchHook = nullptr;
  g_reentryBios = nullptr;

  EXPECT_EQ(g_reentryDispatches, 1)
      << "nested drain dispatched the callback again";
  st.reset();
}

// Which callbacks the per-frame interrupt tick delivers.
//
// libsnd's `SsStart` in VSync tick mode hooks the sequencer onto IRQ line 0
// (VBlank) through `InterruptCallback(0, _SsTrapIntrVSync)`; a tick that only
// walks the timer lines never runs it and the music never plays.  The SPU
// library's transfer-completion poller arrives through `DMACallback(4, fn)`,
// not `InterruptCallback` -- it has to keep running once it lands in the DMA
// table, or the sound effects stop.
namespace {
std::vector<uint32_t> g_dispatched;
void recordingHook(recomp_context * /*ctx*/, uint32_t addr) {
  g_dispatched.push_back(addr);
}
} // namespace

TEST_F(BiosTest, DrainTicksVBlankLineCallbackOncePerFrame) {
  auto &st = ps1::psyq::psyq_state();
  st.reset();
  st.intrCallback[0] = 0x8004B828;
  st.vsyncCounter.store(1, std::memory_order_release);
  g_dispatched.clear();
  g_testRecompDispatchHook = &recordingHook;

  bios->drainPendingCallbacks();
  bios->drainPendingCallbacks(); // same frame: no second tick

  g_testRecompDispatchHook = nullptr;
  ASSERT_EQ(g_dispatched.size(), 1u);
  EXPECT_EQ(g_dispatched[0], 0x8004B828u);
  st.reset();
}

TEST_F(BiosTest, DrainPollsDmaCompletionCallbacks) {
  auto &st = ps1::psyq::psyq_state();
  st.reset();
  st.dmaCallback[4] = 0x800466A0;
  st.vsyncCounter.store(1, std::memory_order_release);
  g_dispatched.clear();
  g_testRecompDispatchHook = &recordingHook;

  bios->drainPendingCallbacks();

  g_testRecompDispatchHook = nullptr;
  ASSERT_EQ(g_dispatched.size(), 1u);
  EXPECT_EQ(g_dispatched[0], 0x800466A0u);
  st.reset();
}

// Callbacks run on libetc's interrupt stack, as PSY-Q's `startIntr` arranges,
// not on whatever $sp the interrupted code holds.  Crash's level decompressor
// points $sp into the compressed buffer and reads the table just below it; a
// callback frame pushed there corrupted the stream and the loop never ended.
namespace {
uint32_t g_callbackSp = 0;
void spRecordingHook(recomp_context *ctx, uint32_t /*addr*/) {
  g_callbackSp = ctx->r29;
}
} // namespace

TEST_F(BiosTest, CallbacksRunOnTheInterruptStack) {
  auto &st = ps1::psyq::psyq_state();
  st.reset();
  st.intrStackTop = 0x8005494C;
  st.intrCallback[0] = 0x8004B828;
  st.vsyncCounter.store(1, std::memory_order_release);
  g_callbackSp = 0;
  g_testRecompDispatchHook = &spRecordingHook;

  const uint32_t interruptedSp = ctx.r29;
  bios->drainPendingCallbacks();

  g_testRecompDispatchHook = nullptr;
  EXPECT_EQ(g_callbackSp, 0x8005494Cu);
  EXPECT_EQ(ctx.r29, interruptedSp) << "interrupted $sp not restored";
  st.reset();
}

// Hand-written PS1 assembly may save the register file to a context block and
// then use $sp as a general-purpose register -- Crash's model transform parks
// $sp in the scratchpad and packs GTE operands through it.  A yield point
// reached in that window must not dispatch, or the callback's prologue writes
// its frame over a data value.  The work stays queued for the next yield
// point that does have a stack.
TEST_F(BiosTest, DrainDefersWhileGuestHasNoStack) {
  auto &st = ps1::psyq::psyq_state();
  st.reset();
  st.intrCallback[4] = 0x80046000;
  st.vsyncCounter.store(1, std::memory_order_release);

  g_reentryBios = bios.get();
  g_reentryDispatches = 0;
  g_testRecompDispatchHook = &reentrantHook;

  const uint32_t savedSp = ctx.r29;
  ctx.r29 = 0x0905A8E4; // a GTE operand, not an address
  bios->drainPendingCallbacks();
  EXPECT_EQ(g_reentryDispatches, 0) << "dispatched without a guest stack";

  ctx.r29 = 0x1F8000E0; // scratchpad: live working storage, not a stack
  bios->drainPendingCallbacks();
  EXPECT_EQ(g_reentryDispatches, 0) << "dispatched onto the scratchpad";

  // Restoring the stack releases the deferred callback.
  ctx.r29 = savedSp;
  bios->drainPendingCallbacks();
  EXPECT_EQ(g_reentryDispatches, 1) << "deferred callback was never delivered";

  g_testRecompDispatchHook = nullptr;
  g_reentryBios = nullptr;
  ctx.r29 = savedSp;
  st.reset();
}
