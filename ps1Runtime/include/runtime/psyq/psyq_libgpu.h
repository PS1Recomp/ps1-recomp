#pragma once
/**
 * @file psyq_libgpu.h
 * @brief Additional libgpu HLE entries detected on the Rayman boot path
 *        (Sessao 0.7).
 *
 * The 10 built-in display/OT helpers (`hle_VSync`, `hle_DrawSync`, etc.) live
 * in `psyq_hle.h`; this header covers primitive-init macros (SetPoly*,
 * SetSprt*, SetShadeTex) and the GetClut() helper that the hash matcher
 * detects in compiled libgpu.lib.
 */

#include "runtime/cpu_context.h"

namespace ps1::psyq {

/// GetClut(x, y) → 16-bit CLUT id used in the GP0 textured-polygon command.
void hle_libgpu_GetClut(recomp_context *ctx);

/// SetShadeTex(p, tge): toggle bit 0 of the primitive's code byte
/// (raw-texture vs. modulated-texture).
void hle_libgpu_SetShadeTex(recomp_context *ctx);

void hle_libgpu_SetPolyF4(recomp_context *ctx);
void hle_libgpu_SetPolyFT4(recomp_context *ctx);
void hle_libgpu_SetSprt(recomp_context *ctx);
void hle_libgpu_SetSprt8(recomp_context *ctx);
void hle_libgpu_SetSprt16(recomp_context *ctx);

void psyq_register_libgpu_extras();

} // namespace ps1::psyq
