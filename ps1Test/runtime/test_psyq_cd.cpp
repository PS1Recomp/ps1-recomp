// Tests for the libcd HLE (Sessao 1.B).
//
// Strategy: build a real Bios + CdromController + Memory; reset the PsyQ
// state singleton in SetUp; call each HLE entry and verify both the
// `psyq_state()` updates (sector counters, callbacks, sync atomics) and the
// controller-side state changes (motor on, command FIFO drained, etc.).
// Phase 2.4 retired the per-game BSS slot configuration — every libcd HLE
// now talks to `ps1::psyq::psyq_state()`.

#include "runtime/bios/bios.h"
#include "runtime/cdrom/cdrom_controller.h"
#include "runtime/cdrom/virtual_fs.h"
#include "runtime/cpu_context.h"
#include "runtime/memory.h"
#include "runtime/psyq/psyq_hle.h"
#include "runtime/psyq/psyq_libcd.h"
#include "runtime/psyq/psyq_registry.h"
#include "runtime/psyq/psyq_state.h"

#include <gtest/gtest.h>

using namespace ps1;
using namespace ps1::psyq;

namespace {

class PsyqCdTest : public ::testing::Test {
protected:
  Memory mem;
  recomp_context ctx;
  cdrom::VirtualFs fs;
  cdrom::CdromController cdrom;
  std::unique_ptr<bios::Bios> bios;

  void SetUp() override {
    ctx.reset();
    ctx.mem = &mem;
    bios = std::make_unique<bios::Bios>(ctx, fs, mem);
    bios->setCdromController(&cdrom);
    ctx.bios = bios.get();

    // Wire the controller's interrupt callback exactly like main_host.cpp:
    // cross-thread enqueue, drained game-thread-side via Bios::drainCdrom-
    // EventQueue (called from drainPendingCallbacks and hle_libcd_CdSync).
    // Phase 3.3 — direct triggerCdromEvent from this lambda would race on
    // event-system state when the SDL render thread fires it concurrently.
    cdrom.setInterruptCallback(
        [this](uint8_t intType) { bios->queueCdromEvent(intType); });

    psyq::psyq_state().reset();
  }

  void TearDown() override { psyq::psyq_state().reset(); }
};

} // namespace

// ──────────────────────────────────────────────────────────
// CdInit
// ──────────────────────────────────────────────────────────

TEST_F(PsyqCdTest, CdInitReturnsSuccessAndDrivesStateAndController) {
  // Pre-mutate so we can prove CdInit zeroes them.
  psyq::psyq_state().cdRemaining = 0xDEADBEEFu;
  psyq::psyq_state().cdDestPtr   = 0xDEADBEEFu;
  psyq::psyq_state().cdWordCount = 0xDEADBEEFu;

  hle_libcd_CdInit(&ctx);

  // libcd CdInit returns 1 on success (the path that doesn't print
  // "Init failed" — the whole point of the HLE replacement).
  EXPECT_EQ(ctx.r[V0], 1u);

  // BIOS event chain mapped INT3 + INT2 to psyq_state().cdSyncByte = 2.
  EXPECT_EQ(psyq::psyq_state().cdSyncByte.load(), 2u);

  // Read-state slots cleared in psyq_state().
  EXPECT_EQ(psyq::psyq_state().cdRemaining, 0u);
  EXPECT_EQ(psyq::psyq_state().cdDestPtr,   0u);
  EXPECT_EQ(psyq::psyq_state().cdWordCount, 0u);

  // Controller transitioned through CdlInit → state is Idle, motor on.
  EXPECT_EQ(cdrom.getState(), cdrom::CdromState::Idle);
  // Mode is reset to 0 by cmdInit.
  EXPECT_EQ(cdrom.getMode(), 0u);
}

TEST_F(PsyqCdTest, CdInitToleratesMissingBiosGracefully) {
  ctx.bios = nullptr;
  ctx.r[V0] = 0xCAFEu;
  hle_libcd_CdInit(&ctx);
  EXPECT_EQ(ctx.r[V0], 0u); // hard failure when bios is unwired
}

// ──────────────────────────────────────────────────────────
// CdRead
// ──────────────────────────────────────────────────────────

TEST_F(PsyqCdTest, CdReadWritesReadStateAndIssuesCommands) {
  ctx.r[A0] = 8;             // sectors
  ctx.r[A1] = 0x80020000u;   // dest buffer
  ctx.r[A2] = 0x80;          // mode bit 7 = double-speed, bit 5 unset → 2048 sectors

  hle_libcd_CdRead(&ctx);

  EXPECT_EQ(ctx.r[V0], 1u);
  EXPECT_EQ(psyq::psyq_state().cdRemaining, 8u);
  EXPECT_EQ(psyq::psyq_state().cdDestPtr,   0x80020000u);
  EXPECT_EQ(psyq::psyq_state().cdWordCount, 512u); // 2048 bytes / 4

  // CdlSetmode + CdlReadN landed on the controller — check the side effects.
  EXPECT_EQ(cdrom.getMode(), 0x80u);
  EXPECT_EQ(cdrom.getState(), cdrom::CdromState::ReadingData);
}

TEST_F(PsyqCdTest, CdReadHonoursWholeSectorMode) {
  ctx.r[A0] = 1;
  ctx.r[A1] = 0x80020000u;
  ctx.r[A2] = 0x20; // bit 5 set → 2340-byte sectors

  hle_libcd_CdRead(&ctx);

  EXPECT_EQ(psyq::psyq_state().cdWordCount, 585u); // 2340 / 4
}

TEST_F(PsyqCdTest, CdReadWithZeroSectorsPreservesRemaining) {
  // Crash Bandicoot calls libcd_CdRead twice back-to-back: first with
  // sectors=N to start the read, then with sectors=0.  If the second call
  // zeroes `cdRemaining`, the in-flight sector is discarded when INT1
  // arrives.  We preserve the prior count when sectors==0 (dest/wordcount
  // still get the fresh values from the call).
  ctx.r[A0] = 3;             // start: read 3 sectors
  ctx.r[A1] = 0x80020000u;
  ctx.r[A2] = 0x80;
  hle_libcd_CdRead(&ctx);
  EXPECT_EQ(psyq::psyq_state().cdRemaining, 3u);

  ctx.r[A0] = 0;             // follow-up call with sectors=0
  ctx.r[A1] = 0x80030000u;
  ctx.r[A2] = 0x80;
  hle_libcd_CdRead(&ctx);

  EXPECT_EQ(psyq::psyq_state().cdRemaining, 3u) << "remaining must be preserved";
  EXPECT_EQ(psyq::psyq_state().cdDestPtr,   0x80030000u)
      << "dest pointer follows the latest call";

  // CdReadBreak is the supported way to cancel a read.
  hle_libcd_CdReadBreak(&ctx);
  EXPECT_EQ(psyq::psyq_state().cdRemaining, 0u);
}

// ──────────────────────────────────────────────────────────
// CdSync / CdReady
// ──────────────────────────────────────────────────────────

TEST_F(PsyqCdTest, CdSyncPollReturnsCompleteUnconditionally) {
  // mode != 0 (poll): synchronous CD command dispatch guarantees the prior
  // command already finished, so we report Complete regardless of the atomic.
  HleConfig cfg{};
  configure(cfg);

  ctx.r[A0] = 1; // mode = poll
  ctx.r[A1] = 0; // no result struct
  hle_libcd_CdSync(&ctx);
  EXPECT_EQ(ctx.r[V0], 2u); // CdlComplete

  HleConfig empty{};
  configure(empty);
}

TEST_F(PsyqCdTest, CdSyncFillsResultStructWhenProvided) {
  HleConfig cfg{};
  configure(cfg);

  uint32_t resultPtr = 0x80100100u;
  // Pre-fill with garbage to verify CdSync overwrites all 8 bytes.
  for (int i = 0; i < 8; ++i) mem.write8(resultPtr + i, 0xAA);

  ctx.r[A0] = 1;
  ctx.r[A1] = resultPtr;
  hle_libcd_CdSync(&ctx);

  EXPECT_EQ(mem.read8(resultPtr + 0), 2u); // CdlComplete
  for (int i = 1; i < 8; ++i)
    EXPECT_EQ(mem.read8(resultPtr + i), 0u) << "byte " << i;
}

TEST_F(PsyqCdTest, CdReadyPollReturnsAtomicValue) {
  HleConfig cfg{};
  configure(cfg);

  // Polling mode reads psyq_state().cdReadyByte.
  psyq::psyq_state().cdReadyByte.store(1); // CdlDataReady
  ctx.r[A0] = 1;
  ctx.r[A1] = 0;
  hle_libcd_CdReady(&ctx);
  EXPECT_EQ(ctx.r[V0], 1u);
}

TEST_F(PsyqCdTest, CdReadyPollWithZeroAtomicReturnsDataReadySentinel) {
  HleConfig cfg{};
  configure(cfg);

  // When nothing has arrived, polling returns CdlDataReady so callers that
  // poll non-blocking and then hit CdGetSector don't deadlock (matches
  // pre-2.3 behaviour preserved by hle_libcd_CdReady).
  psyq::psyq_state().cdReadyByte.store(0);
  ctx.r[A0] = 1;
  ctx.r[A1] = 0;
  hle_libcd_CdReady(&ctx);
  EXPECT_EQ(ctx.r[V0], 1u); // CdlDataReady sentinel
}

TEST_F(PsyqCdTest, TriggerCdromEventInt2WritesAtomicToComplete) {
  // Direct integration check: bios::triggerCdromEvent(2) should set
  // psyq_state().cdSyncByte to 2 (no BSS write happens — Phase 2.3).
  psyq::psyq_state().cdSyncByte.store(0);
  bios->triggerCdromEvent(2);
  EXPECT_EQ(psyq::psyq_state().cdSyncByte.load(), 2u);
}

TEST_F(PsyqCdTest, TriggerCdromEventInt5WritesBothAtomicsToError) {
  psyq::psyq_state().cdSyncByte.store(0);
  psyq::psyq_state().cdReadyByte.store(0);
  bios->triggerCdromEvent(5);
  EXPECT_EQ(psyq::psyq_state().cdSyncByte.load(), 5u);
  EXPECT_EQ(psyq::psyq_state().cdReadyByte.load(), 5u);
}

// ──────────────────────────────────────────────────────────
// CdControl / CdControlF
// ──────────────────────────────────────────────────────────

TEST_F(PsyqCdTest, CdControlPushesParamsAndIssuesCommand) {
  // CdlSetloc(0x02) — 3 BCD bytes M:S:F.
  uint32_t paramPtr = 0x80100200u;
  mem.write8(paramPtr + 0, 0x00); // M
  mem.write8(paramPtr + 1, 0x05); // S = 5
  mem.write8(paramPtr + 2, 0x00); // F

  ctx.r[A0] = 0x02; // CdlSetloc
  ctx.r[A1] = paramPtr;
  ctx.r[A2] = 0;    // no result
  hle_libcd_CdControl(&ctx);

  EXPECT_EQ(ctx.r[V0], 1u);
  // Side-effect of CdlSetloc on cdrom_controller: it acknowledges with
  // INT3, leaving an interrupt pending.  No direct seekTarget_ accessor
  // exists, but we can check the controller saw something happen via the
  // interrupt being asserted then the BIOS HLE dropping it.
  // (After triggerCdromEvent fires synchronously, interruptFlag_ stays
  // set on the controller until the test acks; so we just confirm a
  // primary response was queued.)
  EXPECT_TRUE(cdrom.hasInterrupt());
}

TEST_F(PsyqCdTest, CdControlFiresCommandWithoutParamsWhenParamPtrIsNull) {
  ctx.r[A0] = 0x09; // CdlPause — 0 params
  ctx.r[A1] = 0;
  ctx.r[A2] = 0;
  hle_libcd_CdControl(&ctx);
  EXPECT_EQ(ctx.r[V0], 1u);
  // cmdPause sets state_ = Paused.
  EXPECT_EQ(cdrom.getState(), cdrom::CdromState::Paused);
}

TEST_F(PsyqCdTest, CdControlReturnsZeroWithoutController) {
  bios->setCdromController(nullptr);
  ctx.r[A0] = 0x01; // GetStat
  hle_libcd_CdControl(&ctx);
  EXPECT_EQ(ctx.r[V0], 0u);
}

TEST_F(PsyqCdTest, CdControlFIsFireAndForget) {
  // CdlSetmode(0x0E) with one param byte.
  uint32_t paramPtr = 0x80100210u;
  mem.write8(paramPtr, 0x80); // double-speed
  ctx.r[A0] = 0x0E;
  ctx.r[A1] = paramPtr;
  hle_libcd_CdControlF(&ctx);
  EXPECT_EQ(ctx.r[V0], 1u);
  EXPECT_EQ(cdrom.getMode(), 0x80u);
}

// ──────────────────────────────────────────────────────────
// CdGetSector
// ──────────────────────────────────────────────────────────

TEST_F(PsyqCdTest, CdGetSectorReturnsZero) {
  ctx.r[V0] = 0xCAFEu;
  hle_libcd_CdGetSector(&ctx);
  EXPECT_EQ(ctx.r[V0], 0u);
}

// ──────────────────────────────────────────────────────────
// Callbacks
// ──────────────────────────────────────────────────────────

TEST_F(PsyqCdTest, CdReadCallbackStoresAndReturnsPrev) {
  psyq::psyq_state().cdDataCb = 0xDEADBEEFu;
  ctx.r[A0] = 0x80055000u;
  hle_libcd_CdReadCallback(&ctx);
  EXPECT_EQ(ctx.r[V0], 0xDEADBEEFu);
  EXPECT_EQ(psyq::psyq_state().cdDataCb, 0x80055000u);
}

TEST_F(PsyqCdTest, CdReadyCallbackStoresAndReturnsPrev) {
  psyq::psyq_state().cdDataCb = 0x80055000u;
  ctx.r[A0] = 0x80056000u;
  hle_libcd_CdReadyCallback(&ctx);
  EXPECT_EQ(ctx.r[V0], 0x80055000u);
  EXPECT_EQ(psyq::psyq_state().cdDataCb, 0x80056000u);
}

TEST_F(PsyqCdTest, CdDataCallbackIsAliasForCdReadyCallback) {
  psyq::psyq_state().cdDataCb = 0u;
  ctx.r[A0] = 0x80057000u;
  hle_libcd_CdDataCallback(&ctx);
  EXPECT_EQ(psyq::psyq_state().cdDataCb, 0x80057000u);
}

// ──────────────────────────────────────────────────────────
// CdMix / CdReadBreak
// ──────────────────────────────────────────────────────────

TEST_F(PsyqCdTest, CdMixIsNopReturnsOne) {
  ctx.r[V0] = 0;
  hle_libcd_CdMix(&ctx);
  EXPECT_EQ(ctx.r[V0], 1u);
}

TEST_F(PsyqCdTest, CdReadBreakStopsControllerAndZeroesState) {
  // Put the controller into a reading state so we have something to break.
  ctx.r[A0] = 4;
  ctx.r[A1] = 0x80020000u;
  ctx.r[A2] = 0x80;
  hle_libcd_CdRead(&ctx);
  ASSERT_EQ(cdrom.getState(), cdrom::CdromState::ReadingData);

  hle_libcd_CdReadBreak(&ctx);

  EXPECT_EQ(ctx.r[V0], 1u);
  EXPECT_EQ(cdrom.getState(), cdrom::CdromState::Idle);
  EXPECT_EQ(psyq::psyq_state().cdRemaining, 0u);
  EXPECT_EQ(psyq::psyq_state().cdDestPtr,   0u);
  EXPECT_EQ(psyq::psyq_state().cdWordCount, 0u);
}

// ──────────────────────────────────────────────────────────
// Phase 2.4 — direct PsyqState integration
// ──────────────────────────────────────────────────────────

TEST_F(PsyqCdTest, TriggerVBlankStampsDrawSyncAllSlotsComplete) {
  // PsyQ DrawSync polls status == 2 for "drawing complete".  triggerVBlankEvent
  // must mirror that on every slot in psyq_state().drawSync.
  auto &draw = psyq::psyq_state().drawSync;
  draw.count = 4;
  for (auto &s : draw.status) s = 0;

  bios->triggerVBlankEvent();

  for (uint32_t i = 0; i < draw.count; ++i)
    EXPECT_EQ(draw.status[i], 2u) << "slot " << i;
  // Slots beyond count are untouched (default-constructed = 0).
  for (uint32_t i = draw.count; i < psyq::GpuDrawSync::kMaxSlots; ++i)
    EXPECT_EQ(draw.status[i], 0u) << "slot " << i;
}

TEST_F(PsyqCdTest, TriggerVBlankSkipsSwapCbWhenZero) {
  // Default psyq_state().gpuSwapCb is 0 — VBlank must not crash queuing it.
  ASSERT_EQ(psyq::psyq_state().gpuSwapCb, 0u);
  EXPECT_NO_FATAL_FAILURE(bios->triggerVBlankEvent());
}

// ──────────────────────────────────────────────────────────
// Phase 3.3 — cross-thread CDROM event queue
// ──────────────────────────────────────────────────────────

// queueCdromEvent must NOT touch psyq_state or the event system itself —
// those side effects only run when the game thread later drains the queue.
// This is what makes the call cross-thread-safe from the SDL render thread.
TEST_F(PsyqCdTest, QueueCdromEventDefersAllSideEffectsUntilDrain) {
  psyq::psyq_state().cdSyncByte.store(0);
  psyq::psyq_state().cdReadyByte.store(0);

  bios->queueCdromEvent(2); // INT2 Complete → would set cdSyncByte=2 if direct
  bios->queueCdromEvent(1); // INT1 DataReady → would set cdReadyByte=1

  // Nothing should have run yet.
  EXPECT_EQ(psyq::psyq_state().cdSyncByte.load(), 0u);
  EXPECT_EQ(psyq::psyq_state().cdReadyByte.load(), 0u);

  // Drain on the game thread (this thread).  Both events fire in FIFO order.
  std::size_t drained = bios->drainCdromEventQueue();
  EXPECT_EQ(drained, 2u);
  EXPECT_EQ(psyq::psyq_state().cdSyncByte.load(),  2u);
  EXPECT_EQ(psyq::psyq_state().cdReadyByte.load(), 1u);

  // Re-draining is a no-op — the queue is empty.
  EXPECT_EQ(bios->drainCdromEventQueue(), 0u);
}

// hle_libcd_CdSync must drain the IRQ queue before reading status, so a
// poll-mode caller (mode != 0) sees post-IRQ state.  Without the leading
// drain, the cdSyncByte atomic would lag the queued IRQ until the next
// drainPendingCallbacks call.
TEST_F(PsyqCdTest, CdSyncDrainsQueueBeforeReadingStatus) {
  HleConfig cfg{};
  configure(cfg);

  // Simulate a cross-thread IRQ that arrived just before the game thread
  // entered CdSync — the controller's tick() callback would have queued
  // an INT5 (DiskError) which maps to cdSyncByte = 5.
  psyq::psyq_state().cdSyncByte.store(0);
  bios->queueCdromEvent(5);
  EXPECT_EQ(psyq::psyq_state().cdSyncByte.load(), 0u); // not drained yet

  // Poll mode (mode != 0): hle_libcd_CdSync should drain the queue and
  // return CDL_COMPLETE per the existing contract; the side effect we
  // assert is that cdSyncByte was updated to 5 by the drained INT5.
  ctx.r[A0] = 1; // poll
  ctx.r[A1] = 0; // no result struct
  hle_libcd_CdSync(&ctx);

  EXPECT_EQ(psyq::psyq_state().cdSyncByte.load(), 5u)
      << "CdSync must drain the queued IRQ before checking status";
}

// drainPendingCallbacks owns the same queue drain — so any code path that
// reaches a yield point picks up cross-thread IRQs.  This is the "general
// drain" hook for sites that don't go through CdSync.
TEST_F(PsyqCdTest, DrainPendingCallbacksDrainsCdromEventQueue) {
  psyq::psyq_state().cdSyncByte.store(0);
  bios->queueCdromEvent(2);
  EXPECT_EQ(psyq::psyq_state().cdSyncByte.load(), 0u);

  bios->drainPendingCallbacks();

  EXPECT_EQ(psyq::psyq_state().cdSyncByte.load(), 2u);
}

// FIFO ordering is preserved — the queue is std::queue<uint8_t> under the
// hood and drain pops front-to-back.  Use the cdReadyByte atomic which
// records the LAST INT1/INT4 to land (overwrites): if INT4 was queued
// after INT1, the atomic should reflect 4.
TEST_F(PsyqCdTest, QueuePreservesFifoOrderOnDrain) {
  psyq::psyq_state().cdReadyByte.store(0);

  bios->queueCdromEvent(1); // first → readyByte=1
  bios->queueCdromEvent(4); // last  → readyByte=4 (overwrites)

  bios->drainCdromEventQueue();
  EXPECT_EQ(psyq::psyq_state().cdReadyByte.load(), 4u);
}

// ──────────────────────────────────────────────────────────
// Registry coverage
// ──────────────────────────────────────────────────────────

TEST_F(PsyqCdTest, RegisterLibcdExposesAll12FunctionsViaDispatch) {
  psyq_register_libcd();

  const char *names[] = {
      "libcd_CdInit",          "libcd_CdRead",         "libcd_CdSync",
      "libcd_CdReady",         "libcd_CdControl",      "libcd_CdControlF",
      "libcd_CdGetSector",     "libcd_CdReadCallback", "libcd_CdReadyCallback",
      "libcd_CdDataCallback",  "libcd_CdMix",          "libcd_CdReadBreak",
      "libcd_StSetMask",
  };

  HleConfig cfg{};
  configure(cfg);

  for (const char *n : names) {
    ctx.reset();
    ctx.mem = &mem;
    ctx.bios = bios.get();
    EXPECT_NO_FATAL_FAILURE(psyq_dispatch(n, &ctx)) << "dispatch failed: " << n;
  }
}
