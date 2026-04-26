#include "runtime/psyq/psyq_libgpu.h"
#include "runtime/memory.h"
#include "runtime/psyq/psyq_registry.h"

#include <cstdint>

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

void psyq_register_libgpu_extras() {
  psyq_register("libgpu_GetClut",     &hle_libgpu_GetClut);
  psyq_register("libgpu_SetShadeTex", &hle_libgpu_SetShadeTex);
  psyq_register("libgpu_SetPolyF4",   &hle_libgpu_SetPolyF4);
  psyq_register("libgpu_SetPolyFT4",  &hle_libgpu_SetPolyFT4);
  psyq_register("libgpu_SetSprt",     &hle_libgpu_SetSprt);
  psyq_register("libgpu_SetSprt8",    &hle_libgpu_SetSprt8);
  psyq_register("libgpu_SetSprt16",   &hle_libgpu_SetSprt16);
}

} // namespace ps1::psyq
