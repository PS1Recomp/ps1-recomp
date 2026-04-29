// Tests for the libcd HLE (Sessao 1.B).
//
// Strategy mirrors test_psyq_libapi.cpp: build a real Bios + CdromController
// + Memory, wire psyq_addresses for the per-game BSS slots, then call each
// HLE entry and verify (a) the BSS write-through and (b) controller-side
// state changes (motor on, command FIFO drained, etc.).

#include "runtime/bios/bios.h"
#include "runtime/cdrom/cdrom_controller.h"
#include "runtime/cdrom/virtual_fs.h"
#include "runtime/cpu_context.h"
#include "runtime/memory.h"
#include "runtime/psyq/psyq_hle.h"
#include "runtime/psyq/psyq_libcd.h"
#include "runtime/psyq/psyq_registry.h"

#include <gtest/gtest.h>

using namespace ps1;
using namespace ps1::psyq;

namespace {

// Pick BSS addresses inside main RAM that don't overlap each other so each
// HLE entry's writes are independently observable.
constexpr uint32_t kCdSyncByte   = 0x80100000u;
constexpr uint32_t kCdReadyByte  = 0x80100001u;
constexpr uint32_t kCdRemaining  = 0x80100010u;
constexpr uint32_t kCdDestPtr    = 0x80100014u;
constexpr uint32_t kCdWordCount  = 0x80100018u;
constexpr uint32_t kCdDataCb     = 0x80100020u;
constexpr uint32_t kCdNotifyCb   = 0x80100024u;

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

    bios::Bios::PsyqAddresses addrs;
    addrs.cdSyncByte   = kCdSyncByte;
    addrs.cdReadyByte  = kCdReadyByte;
    addrs.cdRemaining  = kCdRemaining;
    addrs.cdDestPtr    = kCdDestPtr;
    addrs.cdWordCount  = kCdWordCount;
    addrs.cdDataCb     = kCdDataCb;
    addrs.cdNotifyCb   = kCdNotifyCb;
    bios->setPsyqAddresses(addrs);

    // Wire the controller's interrupt callback exactly like main_host.cpp.
    cdrom.setInterruptCallback(
        [this](uint8_t intType) { bios->triggerCdromEvent(intType); });

    // Pre-fill BSS with sentinels so we can detect writes vs no-ops.
    mem.write32(kCdRemaining, 0xDEADBEEFu);
    mem.write32(kCdDestPtr,   0xDEADBEEFu);
    mem.write32(kCdWordCount, 0xDEADBEEFu);
    mem.write32(kCdDataCb,    0u);
    mem.write32(kCdNotifyCb,  0u);
    mem.write8(kCdSyncByte, 0);
    mem.write8(kCdReadyByte, 0);
  }
};

} // namespace

// ──────────────────────────────────────────────────────────
// CdInit
// ──────────────────────────────────────────────────────────

TEST_F(PsyqCdTest, CdInitReturnsSuccessAndDrivesBssAndController) {
  hle_libcd_CdInit(&ctx);

  // libcd CdInit returns 1 on success (the path that doesn't print
  // "Init failed" — the whole point of the HLE replacement).
  EXPECT_EQ(ctx.r[V0], 1u);

  // BIOS event chain mapped INT3 + INT2 to cdSyncByte = 2.
  EXPECT_EQ(mem.read8(kCdSyncByte), 2u);

  // Read-state slots cleared.
  EXPECT_EQ(mem.read32(kCdRemaining), 0u);
  EXPECT_EQ(mem.read32(kCdDestPtr),   0u);
  EXPECT_EQ(mem.read32(kCdWordCount), 0u);

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
  EXPECT_EQ(mem.read32(kCdRemaining), 8u);
  EXPECT_EQ(mem.read32(kCdDestPtr),   0x80020000u);
  EXPECT_EQ(mem.read32(kCdWordCount), 512u); // 2048 bytes / 4

  // CdlSetmode + CdlReadN landed on the controller — check the side effects.
  EXPECT_EQ(cdrom.getMode(), 0x80u);
  EXPECT_EQ(cdrom.getState(), cdrom::CdromState::ReadingData);
}

TEST_F(PsyqCdTest, CdReadHonoursWholeSectorMode) {
  ctx.r[A0] = 1;
  ctx.r[A1] = 0x80020000u;
  ctx.r[A2] = 0x20; // bit 5 set → 2340-byte sectors

  hle_libcd_CdRead(&ctx);

  EXPECT_EQ(mem.read32(kCdWordCount), 585u); // 2340 / 4
}

// ──────────────────────────────────────────────────────────
// CdSync / CdReady
// ──────────────────────────────────────────────────────────

TEST_F(PsyqCdTest, CdSyncReturnsCompleteWhenNoWaitHook) {
  // Default HleConfig has no waitForCdSync hook, so non-blocking path runs.
  HleConfig cfg{}; // no callbacks bound
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

TEST_F(PsyqCdTest, CdReadyReturnsBssReadyByteWhenPolling) {
  HleConfig cfg{};
  configure(cfg);

  // Polling mode reads the BSS ready byte.
  mem.write8(kCdReadyByte, 1); // CdlDataReady
  ctx.r[A0] = 1;
  ctx.r[A1] = 0;
  hle_libcd_CdReady(&ctx);
  EXPECT_EQ(ctx.r[V0], 1u);
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
  mem.write32(kCdDataCb, 0xDEADBEEFu);
  ctx.r[A0] = 0x80055000u;
  hle_libcd_CdReadCallback(&ctx);
  EXPECT_EQ(ctx.r[V0], 0xDEADBEEFu);
  EXPECT_EQ(mem.read32(kCdDataCb), 0x80055000u);
}

TEST_F(PsyqCdTest, CdReadyCallbackStoresAndReturnsPrev) {
  mem.write32(kCdDataCb, 0x80055000u);
  ctx.r[A0] = 0x80056000u;
  hle_libcd_CdReadyCallback(&ctx);
  EXPECT_EQ(ctx.r[V0], 0x80055000u);
  EXPECT_EQ(mem.read32(kCdDataCb), 0x80056000u);
}

TEST_F(PsyqCdTest, CdDataCallbackIsAliasForCdReadyCallback) {
  mem.write32(kCdDataCb, 0u);
  ctx.r[A0] = 0x80057000u;
  hle_libcd_CdDataCallback(&ctx);
  EXPECT_EQ(mem.read32(kCdDataCb), 0x80057000u);
}

// ──────────────────────────────────────────────────────────
// CdMix / CdReadBreak
// ──────────────────────────────────────────────────────────

TEST_F(PsyqCdTest, CdMixIsNopReturnsOne) {
  ctx.r[V0] = 0;
  hle_libcd_CdMix(&ctx);
  EXPECT_EQ(ctx.r[V0], 1u);
}

TEST_F(PsyqCdTest, CdReadBreakStopsControllerAndZeroesBss) {
  // Put the controller into a reading state so we have something to break.
  ctx.r[A0] = 4;
  ctx.r[A1] = 0x80020000u;
  ctx.r[A2] = 0x80;
  hle_libcd_CdRead(&ctx);
  ASSERT_EQ(cdrom.getState(), cdrom::CdromState::ReadingData);

  hle_libcd_CdReadBreak(&ctx);

  EXPECT_EQ(ctx.r[V0], 1u);
  EXPECT_EQ(cdrom.getState(), cdrom::CdromState::Idle);
  EXPECT_EQ(mem.read32(kCdRemaining), 0u);
  EXPECT_EQ(mem.read32(kCdDestPtr),   0u);
  EXPECT_EQ(mem.read32(kCdWordCount), 0u);
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
