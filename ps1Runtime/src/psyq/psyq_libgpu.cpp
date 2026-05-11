#include "runtime/psyq/psyq_libgpu.h"
#include "runtime/memory.h"
#include "runtime/psyq/psyq_hle.h"
#include "runtime/psyq/psyq_registry.h"
#include "runtime/psyq/psyq_state.h"

#include <cstdint>
#include <fmt/format.h>
#include <unordered_set>

namespace ps1::psyq {

namespace {

// PsyQ primitive layout: byte 3 = `len` (words after the tag word), byte 7 =
// `code` (GP0 command nibble).  setlen/setcode macros from <libgpu.h>.
inline void writeLen(recomp_context *ctx, uint32_t p, uint8_t len) {
  ctx->mem->write8(p + 3, len);
}
inline void writeCode(recomp_context *ctx, uint32_t p, uint8_t code) {
  ctx->mem->write8(p + 7, code);
}

// Reads the int16 RECT { x, y, w, h } at the given PS1 RAM pointer.
struct PsyqRect {
  int16_t x, y, w, h;
};

inline PsyqRect readRect(recomp_context *ctx, uint32_t p) {
  PsyqRect r;
  r.x = static_cast<int16_t>(ctx->mem->read16(p + 0));
  r.y = static_cast<int16_t>(ctx->mem->read16(p + 2));
  r.w = static_cast<int16_t>(ctx->mem->read16(p + 4));
  r.h = static_cast<int16_t>(ctx->mem->read16(p + 6));
  return r;
}

inline void writeGP0(uint32_t w) {
  const auto &cfg = getConfig();
  if (cfg.writeGP0) cfg.writeGP0(w);
}

inline void writeGP1(uint32_t w) {
  const auto &cfg = getConfig();
  if (cfg.writeGP1) cfg.writeGP1(w);
}

// Module-level state shared across libgpu HLE calls.
// videoMode: 0 = NTSC (default), 1 = PAL.
int g_videoMode = 0;

// DrawSyncCallback PS1-side function pointer. Recorded so we can return the
// previous one; not actually invoked (runtime GPU is synchronous).
// VSyncCallback's swap routine lives in psyq_state().gpuSwapCb so the BIOS
// VBlank thread can dispatch it without round-tripping through this module.
uint32_t g_drawSyncCallback = 0;

// "Once-per-name" warning helper for stubbed libgs entries.
void warnOnceFor(const char *name) {
  static std::unordered_set<std::string> seen;
  if (seen.insert(name).second)
    fmt::print(stderr, "[PSYQ] {} stubbed (NOP) — no-op for current HLE coverage\n", name);
}

} // namespace

// GetClut(x, y) → packed CLUT id. y occupies the high 9 bits, x the low 6
// (x is always a multiple of 16, hence the >>4).
void hle_libgpu_GetClut(recomp_context *ctx) {
  uint32_t x = ctx->r[A0];
  uint32_t y = ctx->r[A1];
  ctx->r[V0] = ((y & 0x1FFu) << 6) | ((x >> 4) & 0x3Fu);
}

// SetShadeTex(p, tge): toggle the raw-texture bit of the primitive's code.
//   tge != 0 → raw texture (no shading)
//   tge == 0 → modulated (shaded) texture
void hle_libgpu_SetShadeTex(recomp_context *ctx) {
  uint32_t p   = ctx->r[A0];
  bool tge     = ctx->r[A1] != 0;
  uint8_t code = ctx->mem->read8(p + 7);
  code = tge ? (code | 0x01u) : (code & 0xFEu);
  ctx->mem->write8(p + 7, code);
}

// Primitive initialisers — set len + code byte, leave colour/coords untouched.
//   POLY_F4 : 4 verts, flat shaded, untextured. code = 0x28, len = 5.
//   POLY_FT4: 4 verts, flat shaded, textured.   code = 0x2C, len = 9.
//   SPRT    : variable-size textured sprite.    code = 0x64, len = 4.
//   SPRT_8  : 8x8 textured sprite.              code = 0x74, len = 3.
//   SPRT_16 : 16x16 textured sprite.            code = 0x7C, len = 3.

void hle_libgpu_SetPolyF4(recomp_context *ctx) {
  uint32_t p = ctx->r[A0];
  writeLen(ctx, p, 5);
  writeCode(ctx, p, 0x28);
}

void hle_libgpu_SetPolyFT4(recomp_context *ctx) {
  uint32_t p = ctx->r[A0];
  writeLen(ctx, p, 9);
  writeCode(ctx, p, 0x2C);
}

void hle_libgpu_SetSprt(recomp_context *ctx) {
  uint32_t p = ctx->r[A0];
  writeLen(ctx, p, 4);
  writeCode(ctx, p, 0x64);
}

void hle_libgpu_SetSprt8(recomp_context *ctx) {
  uint32_t p = ctx->r[A0];
  writeLen(ctx, p, 3);
  writeCode(ctx, p, 0x74);
}

void hle_libgpu_SetSprt16(recomp_context *ctx) {
  uint32_t p = ctx->r[A0];
  writeLen(ctx, p, 3);
  writeCode(ctx, p, 0x7C);
}

// ── Group 1.A — display-area / VRAM-transfer / video-mode HLEs ─────────────
//
// All take args in the standard PsyQ a0..a3 + stack-spill convention; values
// are converted to GP0/GP1 commands and pushed via `writeGP0`/`writeGP1`.

// SetDispMask(mask): GP1(0x03, mask).  mask=1 enable, mask=0 disable.
void hle_libgpu_SetDispMask(recomp_context *ctx) {
  uint32_t mask = ctx->r[A0] & 0x1u;
  // GP1(0x03): bit 0 = display disable (1 = OFF, 0 = ON).
  // PsyQ SetDispMask(1) = enable, so we invert here.
  writeGP1(0x03000000u | (mask ? 0u : 1u));
}

// LoadImage(rect*, src*) — GP0(0xA0) + (w*h+1)/2 data words.
void hle_libgpu_LoadImage(recomp_context *ctx) {
  PsyqRect r = readRect(ctx, ctx->r[A0]);
  uint32_t src = ctx->r[A1];
  if (r.w <= 0 || r.h <= 0) return;

  writeGP0(0xA0000000u);
  writeGP0(static_cast<uint32_t>(r.y & 0xFFFF) << 16 |
           static_cast<uint32_t>(r.x & 0xFFFF));
  writeGP0(static_cast<uint32_t>(r.h & 0xFFFF) << 16 |
           static_cast<uint32_t>(r.w & 0xFFFF));

  // Data: w * h pixels at 16bpp = (w*h + 1) / 2 32-bit words.
  uint32_t pixels = static_cast<uint32_t>(r.w) * static_cast<uint32_t>(r.h);
  uint32_t words  = (pixels + 1u) / 2u;
  for (uint32_t i = 0; i < words; ++i)
    writeGP0(ctx->mem->read32(src + i * 4));
}

// StoreImage(rect*, dst*) — GP0(0xC0); drains GPUREAD into PS1 RAM.
// The runtime GPU implements VRAM→CPU via GPUREAD register polling. We can't
// reach `gpuRead_` directly from here, but the GPU's CPU→VRAM/VRAM→CPU state
// machine processes the rect on GP0(0xC0) submission. Until a `readGPUREAD`
// drain hook is exposed, this stub just pumps the command and leaves the
// destination buffer untouched. Logged so misuse is visible.
void hle_libgpu_StoreImage(recomp_context *ctx) {
  PsyqRect r = readRect(ctx, ctx->r[A0]);
  if (r.w <= 0 || r.h <= 0) return;
  writeGP0(0xC0000000u);
  writeGP0(static_cast<uint32_t>(r.y & 0xFFFF) << 16 |
           static_cast<uint32_t>(r.x & 0xFFFF));
  writeGP0(static_cast<uint32_t>(r.h & 0xFFFF) << 16 |
           static_cast<uint32_t>(r.w & 0xFFFF));
  warnOnceFor("libgpu_StoreImage");
}

// MoveImage(rect*, x, y) — GP0(0x80) VRAM→VRAM blit.
void hle_libgpu_MoveImage(recomp_context *ctx) {
  PsyqRect r  = readRect(ctx, ctx->r[A0]);
  uint16_t dx = static_cast<uint16_t>(ctx->r[A1]);
  uint16_t dy = static_cast<uint16_t>(ctx->r[A2]);
  if (r.w <= 0 || r.h <= 0) return;
  writeGP0(0x80000000u);
  writeGP0(static_cast<uint32_t>(r.y & 0xFFFF) << 16 |
           static_cast<uint32_t>(r.x & 0xFFFF));
  writeGP0(static_cast<uint32_t>(dy) << 16 | static_cast<uint32_t>(dx));
  writeGP0(static_cast<uint32_t>(r.h & 0xFFFF) << 16 |
           static_cast<uint32_t>(r.w & 0xFFFF));
}

// ClearImage(rect*, r, g, b) — GP0(0x02) FillRect.
void hle_libgpu_ClearImage(recomp_context *ctx) {
  PsyqRect r = readRect(ctx, ctx->r[A0]);
  uint8_t cr = static_cast<uint8_t>(ctx->r[A1]);
  uint8_t cg = static_cast<uint8_t>(ctx->r[A2]);
  uint8_t cb = static_cast<uint8_t>(ctx->r[A3]);
  if (r.w <= 0 || r.h <= 0) return;
  writeGP0(0x02000000u | (static_cast<uint32_t>(cb) << 16) |
           (static_cast<uint32_t>(cg) << 8) | cr);
  writeGP0(static_cast<uint32_t>(r.y & 0xFFFF) << 16 |
           static_cast<uint32_t>(r.x & 0xFFFF));
  writeGP0(static_cast<uint32_t>(r.h & 0xFFFF) << 16 |
           static_cast<uint32_t>(r.w & 0xFFFF));
}

// DrawSyncCallback(fn): record + return previous. Not invoked by the runtime
// because the GPU is fully synchronous (DrawSync always returns 0 immediately).
void hle_libgpu_DrawSyncCallback(recomp_context *ctx) {
  uint32_t prev = g_drawSyncCallback;
  g_drawSyncCallback = ctx->r[A0];
  ctx->r[V0] = prev;
}

// VSyncCallback(fn): record into psyq_state().gpuSwapCb + return previous.
// bios.cpp::triggerVBlankEvent queues this via queueCallbackWithArg(a0=4)
// so the game thread can dispatch it from drainPendingCallbacks safely.
void hle_libgpu_VSyncCallback(recomp_context *ctx) {
  auto &slot = psyq_state().gpuSwapCb;
  uint32_t prev = slot;
  slot = ctx->r[A0];
  ctx->r[V0] = prev;
}

// SetVideoMode(mode): 0=NTSC, 1=PAL. Stores in module state, returns prev.
// Real impl would also reissue GP1(0x08) with the new VRES bits; deferred
// until a game actually exercises mid-run mode switching.
void hle_libgpu_SetVideoMode(recomp_context *ctx) {
  int prev = g_videoMode;
  g_videoMode = static_cast<int>(ctx->r[A0]) & 0x1;
  ctx->r[V0] = static_cast<uint32_t>(prev);
}

void hle_libgpu_GetVideoMode(recomp_context *ctx) {
  ctx->r[V0] = static_cast<uint32_t>(g_videoMode);
}

// ── Group 1.A — libgs scene-graph stubs ────────────────────────────────────
//
// libgs is a higher-level wrapper around libgpu; neither Rayman nor Crash
// links it. These NOP stubs keep the registry dispatch happy and warn once
// per name so missing real implementations are visible.

void hle_libgs_GsInitGraph(recomp_context *ctx) {
  (void)ctx; warnOnceFor("libgs_GsInitGraph");
}
void hle_libgs_GsDefDispBuff(recomp_context *ctx) {
  (void)ctx; warnOnceFor("libgs_GsDefDispBuff");
}
void hle_libgs_GsSetWorkBase(recomp_context *ctx) {
  (void)ctx; warnOnceFor("libgs_GsSetWorkBase");
}
void hle_libgs_GsSortClear(recomp_context *ctx) {
  (void)ctx; warnOnceFor("libgs_GsSortClear");
}

void psyq_register_libgpu_extras() {
  psyq_register("libgpu_GetClut",     &hle_libgpu_GetClut);
  psyq_register("libgpu_SetShadeTex", &hle_libgpu_SetShadeTex);
  psyq_register("libgpu_SetPolyF4",   &hle_libgpu_SetPolyF4);
  psyq_register("libgpu_SetPolyFT4",  &hle_libgpu_SetPolyFT4);
  psyq_register("libgpu_SetSprt",     &hle_libgpu_SetSprt);
  psyq_register("libgpu_SetSprt8",    &hle_libgpu_SetSprt8);
  psyq_register("libgpu_SetSprt16",   &hle_libgpu_SetSprt16);

  // Group 1.A
  psyq_register("libgpu_SetDispMask",       &hle_libgpu_SetDispMask);
  psyq_register("libgpu_LoadImage",         &hle_libgpu_LoadImage);
  psyq_register("libgpu_StoreImage",        &hle_libgpu_StoreImage);
  psyq_register("libgpu_MoveImage",         &hle_libgpu_MoveImage);
  psyq_register("libgpu_ClearImage",        &hle_libgpu_ClearImage);
  psyq_register("libgpu_DrawSyncCallback",  &hle_libgpu_DrawSyncCallback);
  // VSyncCallback / SetVideoMode / GetVideoMode live in libetc per
  // psyq_signatures.toml (verified for v3.5/v4.0 LIBETC).
  psyq_register("libetc_VSyncCallback",     &hle_libgpu_VSyncCallback);
  // Crash Bandicoot calls VSyncCallbacks (plural) — same semantics in PsyQ
  // (registers a per-frame callback into psyq_state().gpuSwapCb).
  psyq_register("libetc_VSyncCallbacks",    &hle_libgpu_VSyncCallback);
  psyq_register("libetc_SetVideoMode",      &hle_libgpu_SetVideoMode);
  psyq_register("libetc_GetVideoMode",      &hle_libgpu_GetVideoMode);

  psyq_register("libgs_GsInitGraph",        &hle_libgs_GsInitGraph);
  psyq_register("libgs_GsDefDispBuff",      &hle_libgs_GsDefDispBuff);
  psyq_register("libgs_GsSetWorkBase",      &hle_libgs_GsSetWorkBase);
  psyq_register("libgs_GsSortClear",        &hle_libgs_GsSortClear);
}

} // namespace ps1::psyq
