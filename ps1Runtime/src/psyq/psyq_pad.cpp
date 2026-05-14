#include "runtime/psyq/psyq_pad.h"
#include "runtime/bios/bios.h"
#include "runtime/input/input.h"
#include "runtime/memory.h"
#include "runtime/psyq/psyq_registry.h"

#include <cstdint>

namespace ps1::psyq {

namespace {

// Per-port direct-mode buffer addresses set by PadInitDirect.  Either may be
// 0, in which case PadRead skips refreshing that side — the "no buffer"
// case is normal when only one slot is wired or the game uses PadRead's
// return value exclusively.
struct PadModuleState {
  uint32_t buf1Addr = 0; // port 0 (slot 1)
  uint32_t buf2Addr = 0; // port 1 (slot 2)
  bool active = false;   // PadInit / PadStartCom raise; PadStopCom lowers
};

PadModuleState g_state;

input::InputController *inputOf(recomp_context *ctx) {
  return ctx && ctx->bios ? ctx->bios->inputController() : nullptr;
}

// Mirror of Bios::updatePadBuffers's byte layout, factored so PadInitDirect
// and PadRead can refresh a single 34-byte buffer at a time.  Real PsyQ
// fills more bytes for analog/DualShock; we cap at 4 because every PsyQ
// title reads bytes 0..3 (status/type/buttons) and ignores higher offsets
// for digital pads.
void writePadBuffer(recomp_context *ctx, input::InputController *input,
                    uint32_t bufAddr, int port) {
  if (bufAddr == 0)
    return;

  if (!input) {
    // No backend: synthesise a "controller absent" header so the game's
    // type-check (byte 1 == 0xFF) trips and it skips the slot cleanly.
    ctx->mem->write8(bufAddr + 0, 0xFF);
    ctx->mem->write8(bufAddr + 1, 0xFF);
    return;
  }

  input::PadType padType = input->getPadType(port);
  if (padType == input::PadType::None) {
    ctx->mem->write8(bufAddr + 0, 0xFF);
    ctx->mem->write8(bufAddr + 1, 0xFF);
    return;
  }

  uint16_t buttons = input->buttonState(port);
  ctx->mem->write8(bufAddr + 0, 0x00);                          // status OK
  ctx->mem->write8(bufAddr + 1, static_cast<uint8_t>(padType)); // type ID
  ctx->mem->write8(bufAddr + 2, static_cast<uint8_t>(buttons & 0xFF));
  ctx->mem->write8(bufAddr + 3, static_cast<uint8_t>((buttons >> 8) & 0xFF));
}

} // namespace

//  PadInit(mode) — `mode` is documented as reserved (caller passes 0).
//  Returns 0 in $v0.  We mark the module active so subsequent PadRead /
//  direct-buffer refreshes do real work, mirroring how libetc flips its
//  internal `PadInit` flag.
void hle_libetc_PadInit(recomp_context *ctx) {
  g_state.active = true;
  ctx->r[V0] = 0;
}

//  PadStartCom() — re-enable pad SIO traffic.  Real libetc unmasks IRQ7
//  and resumes the per-VBlank pad poll; in our model SIO is always live
//  on the host side, so this is bookkeeping only.
void hle_libetc_PadStartCom(recomp_context *ctx) {
  g_state.active = true;
  ctx->r[V0] = 0;
}

//  PadStopCom() — temporarily halt pad polling so MemCard ops own SIO.
//  PadRead/PadGetState below still answer truthfully — disabling them
//  would cause games that poll input during a save dialog to misread.
void hle_libetc_PadStopCom(recomp_context *ctx) {
  g_state.active = false;
  ctx->r[V0] = 0;
}

//  PadInitDirect(buf1, buf2) — register the user-supplied 34-byte status
//  buffers that the pad subsystem refreshes each VBlank.  We don't have a
//  hook on the VBlank thread for libetc, so we refresh the buffers here
//  (initial seed) and again on every PadRead — close enough since games
//  always call PadRead immediately after VSync.
void hle_libetc_PadInitDirect(recomp_context *ctx) {
  g_state.buf1Addr = ctx->r[A0];
  g_state.buf2Addr = ctx->r[A1];
  g_state.active   = true;

  auto *input = inputOf(ctx);
  writePadBuffer(ctx, input, g_state.buf1Addr, 0);
  writePadBuffer(ctx, input, g_state.buf2Addr, 1);

  ctx->r[V0] = 0;
}

//  PadGetState(port) — boil the 6-state PsyQ link machine down to the two
//  outcomes our backend can actually distinguish.  Games overwhelmingly
//  branch on `state == PAD_STATE_STABLE`, so returning that whenever the
//  port reports a known PadType is sufficient.  Unknown ports report
//  Discovery so the game's "wait for stable" loops still bail out cleanly.
void hle_libetc_PadGetState(recomp_context *ctx) {
  int port = static_cast<int>(static_cast<int32_t>(ctx->r[A0]));
  auto *input = inputOf(ctx);
  if (!input || port < 0 || port > 1) {
    ctx->r[V0] = PAD_STATE_DISCOVERY;
    return;
  }
  ctx->r[V0] = (input->getPadType(port) == input::PadType::None)
                   ? PAD_STATE_DISCOVERY
                   : PAD_STATE_STABLE;
}

//  PadRead(n) — packed 32-bit pad word: port 1 in the low half, port 2 in
//  the high half.  Each half is the PsyQ active-low button mask (bit clear
//  = pressed) which is exactly what `InputController::buttonState()`
//  already returns; no extra inversion needed.
//
//  Whether the module is "active" doesn't gate reading on real libetc —
//  PadRead just samples the most recent VBlank snapshot — so we ignore
//  `g_state.active` here and always answer.
void hle_libetc_PadRead(recomp_context *ctx) {
  auto *input = inputOf(ctx);

  uint16_t port1 = input ? input->buttonState(0) : 0xFFFF;
  uint16_t port2 = input ? input->buttonState(1) : 0xFFFF;

  // Refresh direct-mode buffers as a side-effect — see PadInitDirect.
  writePadBuffer(ctx, input, g_state.buf1Addr, 0);
  writePadBuffer(ctx, input, g_state.buf2Addr, 1);

  ctx->r[V0] = (static_cast<uint32_t>(port2) << 16) |
               static_cast<uint32_t>(port1);
}

// Test-only state hooks

void psyq_pad_reset_for_tests() { g_state = PadModuleState{}; }

void psyq_pad_refresh_direct_buffers(recomp_context *ctx) {
  auto *input = inputOf(ctx);
  writePadBuffer(ctx, input, g_state.buf1Addr, 0);
  writePadBuffer(ctx, input, g_state.buf2Addr, 1);
}

// Registry wiring

void psyq_register_libetc_pad() {
  psyq_register("libetc_PadInit",       &hle_libetc_PadInit);
  psyq_register("libetc_PadStartCom",   &hle_libetc_PadStartCom);
  psyq_register("libetc_PadStopCom",    &hle_libetc_PadStopCom);
  psyq_register("libetc_PadInitDirect", &hle_libetc_PadInitDirect);
  psyq_register("libetc_PadGetState",   &hle_libetc_PadGetState);
  psyq_register("libetc_PadRead",       &hle_libetc_PadRead);
}

} // namespace ps1::psyq
