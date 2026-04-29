#include "runtime/psyq/psyq_libcd.h"
#include "runtime/bios/bios.h"
#include "runtime/cdrom/cdrom_controller.h"
#include "runtime/memory.h"
#include "runtime/psyq/psyq_hle.h"
#include "runtime/psyq/psyq_registry.h"

#include <array>
#include <cstdint>

namespace ps1::psyq {

namespace {

// ── PsyQ libcd command bytes (subset relevant for boot path) ──
// Mirrors `enum CdlCommand` from PsyQ <libcd.h>.
constexpr uint8_t CDL_NOP       = 0x01;
constexpr uint8_t CDL_SETLOC    = 0x02;
constexpr uint8_t CDL_PLAY      = 0x03;
constexpr uint8_t CDL_READN     = 0x06;
constexpr uint8_t CDL_PAUSE     = 0x09;
constexpr uint8_t CDL_INIT      = 0x0A;
constexpr uint8_t CDL_SETFILTER = 0x0D;
constexpr uint8_t CDL_SETMODE   = 0x0E;
constexpr uint8_t CDL_GETTD     = 0x14;
constexpr uint8_t CDL_TEST      = 0x19;

// ── PsyQ libcd response codes ──
// PsyQ <libcd.h> enum CdlIntr: NoIntr=0, DataReady=1, Complete=2,
// Acknowledge=3, DataEnd=4, DiskError=5.
constexpr uint8_t CDL_NO_INTR    = 0;
constexpr uint8_t CDL_DATA_READY = 1;
constexpr uint8_t CDL_COMPLETE   = 2;
constexpr uint8_t CDL_DISK_ERROR = 5;

// ── PsyQ CDROM I/O ports (KSEG1 unmasked addresses) ──
constexpr uint32_t CDR_PORT0 = 0x1F801800; // status / index
constexpr uint32_t CDR_PORT1 = 0x1F801801; // command / response
constexpr uint32_t CDR_PORT2 = 0x1F801802; // parameter / data
constexpr uint32_t CDR_PORT3 = 0x1F801803; // IE / IF

// Number of parameter bytes the controller consumes for each command.  Anything
// missing here defaults to 0, which is correct for stat-only commands.  The
// hardware FIFO is cleared after each command, so over-pushing a few extra
// bytes on commands we haven't tabulated would be benign — but pushing fewer
// than required would leave the controller waiting for parameters it never
// sees (e.g. CdlSetloc without M:S:F).
constexpr int paramCountForCmd(uint8_t com) {
  switch (com) {
  case CDL_SETLOC:    return 3; // M / S / F
  case CDL_SETFILTER: return 2; // file / channel
  case CDL_SETMODE:   return 1; // mode bits
  case CDL_GETTD:     return 1; // track number
  case CDL_TEST:      return 1; // sub-function
  default:            return 0;
  }
}

cdrom::CdromController *cdromOf(recomp_context *ctx) {
  return ctx->bios ? ctx->bios->cdromController() : nullptr;
}

// Push parameter bytes from PS1 RAM into the controller's parameter FIFO.
// Skips silently when paramPtr is NULL (game passed nullptr for a command
// that takes no parameters).
void pushParams(recomp_context *ctx, cdrom::CdromController *cdrom,
                uint32_t paramPtr, int count) {
  if (!cdrom || count <= 0 || paramPtr == 0)
    return;
  for (int i = 0; i < count; ++i)
    cdrom->writeRegister(CDR_PORT2, ctx->mem->read8(paramPtr + i));
}

// Drain up to 4 bytes of response into `*result`.  Real libcd CdControl
// fills 0..8 bytes depending on the command; the boot-path callers we care
// about only inspect the status byte, so 4 is a safe upper bound that
// avoids over-reading into the next response.
void drainResponse(recomp_context *ctx, cdrom::CdromController *cdrom,
                   uint32_t resultPtr) {
  if (!cdrom || resultPtr == 0)
    return;
  for (int i = 0; i < 4; ++i)
    ctx->mem->write8(resultPtr + i, cdrom->readRegister(CDR_PORT1));
}

// Issue a CD command synchronously through the controller register interface.
// The controller's interruptCallback (wired to bios.triggerCdromEvent in
// main_host.cpp) fires INT3/INT2 inline, so by the time this returns the
// game-thread BSS state already reflects the response.
void issueCommand(cdrom::CdromController *cdrom, uint8_t com) {
  if (!cdrom) return;
  cdrom->writeRegister(CDR_PORT0, 0); // index = 0
  cdrom->writeRegister(CDR_PORT1, com);
}

} // namespace

// ── CdInit ────────────────────────────────────────────────────────────────
//
// HLE replacement for the native PsyQ CdInit.  We deliberately bypass the
// retry loop / B0:42 verification that "Init failed" comes from — the
// hardware controller is trivially reset and INT3+INT2 are delivered
// synchronously, so the state machine ends in cdSyncByte=2 and CdInit
// always succeeds.
//
void hle_libcd_CdInit(recomp_context *ctx) {
  auto *bios = ctx->bios;
  if (!bios) {
    ctx->r[V0] = 0;
    return;
  }

  if (auto *cdrom = bios->cdromController()) {
    // Send CdlInit through the register interface so cdrom_controller
    // transitions Idle / motorOn / mode=0 like a real reset.
    cdrom->writeRegister(CDR_PORT0, 0);
    cdrom->writeRegister(CDR_PORT1, CDL_INIT);
    // The controller queues INT3+INT2 itself; we deliver them ourselves
    // (next two lines), so cancel the queued copy to avoid a double event.
    cdrom->cancelPendingInterrupt();
  }

  // Drive the BIOS event chain that mirrors what the PsyQ IRQ handler
  // would do: INT3 = Acknowledge, INT2 = Complete.  Both map to
  // cdSyncByte = 2 in the per-game BSS (see bios.cpp::triggerCdromEvent).
  bios->triggerCdromEvent(3);
  bios->triggerCdromEvent(2);

  // Reset the read-state BSS slots that bios.cpp's HLE sector copy reads.
  // These will move into PsyqState in Phase 2 — until then keep them in
  // sync with the hardware state.
  const auto &addrs = bios->psyqAddresses();
  if (addrs.cdRemaining) ctx->mem->write32(addrs.cdRemaining, 0);
  if (addrs.cdDestPtr)   ctx->mem->write32(addrs.cdDestPtr,   0);
  if (addrs.cdWordCount) ctx->mem->write32(addrs.cdWordCount, 0);

  ctx->r[V0] = 1; // success
}

// ── CdRead(sectors, *buf, mode) ───────────────────────────────────────────
//
// Asynchronous read.  Updates the BSS read state that bios.cpp consults from
// triggerCdromEvent(INT1)/drainPendingCallbacks, then issues CdlSetmode +
// CdlReadN.  CdlSetloc must already have been sent by the caller (PsyQ
// requires it before CdRead).
//
void hle_libcd_CdRead(recomp_context *ctx) {
  auto *bios = ctx->bios;
  if (!bios) { ctx->r[V0] = 0; return; }

  int32_t  sectors = static_cast<int32_t>(ctx->r[A0]);
  uint32_t bufPtr  = ctx->r[A1];
  uint8_t  mode    = static_cast<uint8_t>(ctx->r[A2]);

  // PsyQ uses 2048-byte sectors by default (Mode-2 form-1 with bit 5 of
  // mode_ unset), so the BIOS-side sector copy needs 512 32-bit words.
  // When mode bit 5 (0x20) is set the controller switches to 2340-byte
  // sectors (whole-sector mode); the BIOS copies 585 words in that case.
  const uint32_t wordCount = (mode & 0x20) ? 585u : 512u;

  const auto &addrs = bios->psyqAddresses();
  if (addrs.cdRemaining) ctx->mem->write32(addrs.cdRemaining, static_cast<uint32_t>(sectors));
  if (addrs.cdDestPtr)   ctx->mem->write32(addrs.cdDestPtr,   bufPtr);
  if (addrs.cdWordCount) ctx->mem->write32(addrs.cdWordCount, wordCount);

  if (auto *cdrom = bios->cdromController()) {
    // CdlSetmode with the requested mode byte.
    cdrom->writeRegister(CDR_PORT0, 0);
    cdrom->writeRegister(CDR_PORT2, mode);
    cdrom->writeRegister(CDR_PORT1, CDL_SETMODE);

    // CdlReadN — current Setloc target, no parameters.
    issueCommand(cdrom, CDL_READN);
  }

  ctx->r[V0] = 1;
}

// ── CdSync(mode, *result) ─────────────────────────────────────────────────
//
// In our model every CdControl-style call is synchronous (the controller
// fires INT3+INT2 inline), so by the time the game polls CdSync the state
// is already CdlComplete (2).  mode=0 should still block until completion;
// we route through Bios::waitForCdSync for that, falling back to the
// internal sync byte if the hook isn't wired (unit-test path).
//
void hle_libcd_CdSync(recomp_context *ctx) {
  uint32_t mode      = ctx->r[A0];
  uint32_t resultPtr = ctx->r[A1];

  uint8_t code = CDL_COMPLETE;
  if (mode == 0 && getConfig().waitForCdSync) {
    uint8_t v = getConfig().waitForCdSync(5000);
    code = (v == 0) ? CDL_NO_INTR : v; // 0 == timeout
  }

  if (resultPtr != 0) {
    // libcd places the 8-byte status response at *result.  We don't model
    // the full response layout, so write the status byte and zero the rest.
    ctx->mem->write8(resultPtr + 0, code);
    for (int i = 1; i < 8; ++i)
      ctx->mem->write8(resultPtr + i, 0);
  }

  ctx->r[V0] = code;
}

// ── CdReady(mode, *result) ────────────────────────────────────────────────
//
// Same shape as CdSync but for data-ready (INT1/INT4) interrupts.  The
// runtime delivers INT1 from cdrom_controller.tick() once a sector is
// buffered; in non-blocking mode we simply read the latest readiness byte
// the BIOS HLE wrote into BSS (cdReadyByte), since that mirrors the real
// PsyQ polling expectation.
//
void hle_libcd_CdReady(recomp_context *ctx) {
  uint32_t mode      = ctx->r[A0];
  uint32_t resultPtr = ctx->r[A1];

  uint8_t code = CDL_NO_INTR;
  if (mode == 0 && getConfig().waitForCdReady) {
    uint8_t v = getConfig().waitForCdReady(5000);
    code = (v == 0) ? CDL_NO_INTR : v;
  } else if (ctx->bios && ctx->bios->psyqAddresses().cdReadyByte) {
    code = ctx->mem->read8(ctx->bios->psyqAddresses().cdReadyByte);
    if (code == 0) code = CDL_NO_INTR;
  } else {
    // No bios / no per-game ready byte — assume data is ready so callers
    // that poll non-blocking and then hit CdGetSector won't deadlock.
    code = CDL_DATA_READY;
  }

  if (resultPtr != 0) {
    ctx->mem->write8(resultPtr + 0, code);
    for (int i = 1; i < 8; ++i)
      ctx->mem->write8(resultPtr + i, 0);
  }

  ctx->r[V0] = code;
}

// ── CdControl(com, *param, *result) ───────────────────────────────────────
//
// Push the right number of parameter bytes, issue the command, drain the
// status response.  The controller fires the appropriate interrupt inline
// (via interruptCallback wired in main_host.cpp), so this is effectively
// synchronous as far as the game thread is concerned.
//
void hle_libcd_CdControl(recomp_context *ctx) {
  uint8_t  com       = static_cast<uint8_t>(ctx->r[A0]);
  uint32_t paramPtr  = ctx->r[A1];
  uint32_t resultPtr = ctx->r[A2];

  auto *cdrom = cdromOf(ctx);
  if (!cdrom) {
    ctx->r[V0] = 0;
    return;
  }

  pushParams(ctx, cdrom, paramPtr, paramCountForCmd(com));
  issueCommand(cdrom, com);
  drainResponse(ctx, cdrom, resultPtr);

  ctx->r[V0] = 1;
}

// ── CdControlF(com, *param) ───────────────────────────────────────────────
//
// Fire-and-forget variant of CdControl: same dispatch, no result drain.
//
void hle_libcd_CdControlF(recomp_context *ctx) {
  uint8_t  com      = static_cast<uint8_t>(ctx->r[A0]);
  uint32_t paramPtr = ctx->r[A1];

  auto *cdrom = cdromOf(ctx);
  if (!cdrom) {
    ctx->r[V0] = 0;
    return;
  }

  pushParams(ctx, cdrom, paramPtr, paramCountForCmd(com));
  issueCommand(cdrom, com);

  ctx->r[V0] = 1;
}

// ── CdGetSector(*madr, size_words) ────────────────────────────────────────
//
// Streaming reads land in game RAM via the BIOS HLE sector copy (driven
// from triggerCdromEvent INT1 + drainPendingCallbacks), so by the time the
// game asks CdGetSector for the buffer there's nothing buffered on our
// side to hand back.  Returning 0 reports "not ready" — the typical caller
// (XA streamer) falls through; the data callback on INT1 is the real path.
//
void hle_libcd_CdGetSector(recomp_context *ctx) {
  ctx->r[V0] = 0;
}

namespace {

// Shared body for the three callback setters: read prev, store new, return prev
// in v0.  All three currently update cdDataCb because bios.cpp dispatches a
// single data callback for INT1 / INT4 (the difference between Read and Ready
// is mostly which interrupts the game expects, and our INT routing folds them).
void setDataCallback(recomp_context *ctx) {
  uint32_t fn = ctx->r[A0];
  uint32_t slot = ctx->bios ? ctx->bios->psyqAddresses().cdDataCb : 0;
  uint32_t prev = 0;
  if (slot) {
    prev = ctx->mem->read32(slot);
    ctx->mem->write32(slot, fn);
  }
  ctx->r[V0] = prev;
}

} // namespace

void hle_libcd_CdReadCallback(recomp_context *ctx)  { setDataCallback(ctx); }
void hle_libcd_CdReadyCallback(recomp_context *ctx) { setDataCallback(ctx); }
void hle_libcd_CdDataCallback(recomp_context *ctx)  { setDataCallback(ctx); }

// ── CdMix(*vol) ───────────────────────────────────────────────────────────
//
// CD audio mixing volume.  SPU CD-mix is not modeled (see CLAUDE.md "Long
// Term: SPU accuracy"), so this is a NOP that returns 1 (success).
//
void hle_libcd_CdMix(recomp_context *ctx) {
  (void)ctx;
  ctx->r[V0] = 1;
}

// ── CdReadBreak() ─────────────────────────────────────────────────────────
//
// Cancel an in-progress CdRead.  Stops the controller's read state machine
// and zeros the BSS read counters so the next INT1 doesn't re-enter the
// sector-copy path.
//
void hle_libcd_CdReadBreak(recomp_context *ctx) {
  auto *bios = ctx->bios;
  if (!bios) { ctx->r[V0] = 0; return; }

  if (auto *cdrom = bios->cdromController())
    cdrom->stopReading();

  const auto &addrs = bios->psyqAddresses();
  if (addrs.cdRemaining) ctx->mem->write32(addrs.cdRemaining, 0);
  if (addrs.cdDestPtr)   ctx->mem->write32(addrs.cdDestPtr,   0);
  if (addrs.cdWordCount) ctx->mem->write32(addrs.cdWordCount, 0);

  ctx->r[V0] = 1;
}

// ── StSetMask ─────────────────────────────────────────────────────────────
// XA-stream sector filter.  Not modeled.
void hle_libcd_StSetMask(recomp_context *ctx) {
  ctx->r[V0] = 0;
}

void psyq_register_libcd() {
  psyq_register("libcd_CdInit",          &hle_libcd_CdInit);
  psyq_register("libcd_CdRead",          &hle_libcd_CdRead);
  psyq_register("libcd_CdSync",          &hle_libcd_CdSync);
  psyq_register("libcd_CdReady",         &hle_libcd_CdReady);
  psyq_register("libcd_CdControl",       &hle_libcd_CdControl);
  psyq_register("libcd_CdControlF",      &hle_libcd_CdControlF);
  psyq_register("libcd_CdGetSector",     &hle_libcd_CdGetSector);
  psyq_register("libcd_CdReadCallback",  &hle_libcd_CdReadCallback);
  psyq_register("libcd_CdReadyCallback", &hle_libcd_CdReadyCallback);
  psyq_register("libcd_CdDataCallback",  &hle_libcd_CdDataCallback);
  psyq_register("libcd_CdMix",           &hle_libcd_CdMix);
  psyq_register("libcd_CdReadBreak",     &hle_libcd_CdReadBreak);
  psyq_register("libcd_StSetMask",       &hle_libcd_StSetMask);
}

} // namespace ps1::psyq
