#include "runtime/psyq/psyq_libgte.h"
#include "runtime/gte.h"
#include "runtime/psyq/psyq_registry.h"

#include <cstdint>

namespace ps1::psyq {

// SetGeomOffset(ofx, ofy):
//   PsyQ stores OFX/OFY as 1/65536 fixed-point screen offsets.  The libgte
//   wrapper does:  cop2c[OFX] = ofx << 16; cop2c[OFY] = ofy << 16;
void hle_libgte_SetGeomOffset(recomp_context *ctx) {
  int32_t ofx = static_cast<int32_t>(ctx->r[A0]);
  int32_t ofy = static_cast<int32_t>(ctx->r[A1]);
  ctx->cop2c[GTE_OFX] = static_cast<uint32_t>(ofx) << 16;
  ctx->cop2c[GTE_OFY] = static_cast<uint32_t>(ofy) << 16;
}

// SetGeomScreen(h): projection-plane distance, 16-bit unsigned.
void hle_libgte_SetGeomScreen(recomp_context *ctx) {
  ctx->cop2c[GTE_H] = ctx->r[A0] & 0xFFFFu;
}

// SetDQA(dqa): depth-cue gradient, 16-bit signed (stored as 32-bit).
void hle_libgte_SetDQA(recomp_context *ctx) {
  ctx->cop2c[GTE_DQA] = ctx->r[A0] & 0xFFFFu;
}

void psyq_register_libgte() {
  psyq_register("libgte_SetGeomOffset", &hle_libgte_SetGeomOffset);
  psyq_register("libgte_SetGeomScreen", &hle_libgte_SetGeomScreen);
  psyq_register("libgte_SetDQA",        &hle_libgte_SetDQA);
}

} // namespace ps1::psyq
