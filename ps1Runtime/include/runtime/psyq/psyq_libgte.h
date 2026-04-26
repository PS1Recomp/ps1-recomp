#pragma once
/**
 * @file psyq_libgte.h
 * @brief HLE for PsyQ libgte entries detected on the boot path (Sessao 0.7).
 *
 * Boot-time GTE setup writes a few cop2 control registers to configure the
 * projection.  The recompiler emits MTC2 instructions correctly for inline
 * GTE code, but PsyQ's libgte wrappers are linked as separate functions —
 * those are intercepted here and forward straight to `cop2c[]`.
 */

#include "runtime/cpu_context.h"

namespace ps1::psyq {

/// SetGeomOffset(ofx, ofy): screen-space drawing offset in 1/65536 fixed point.
void hle_libgte_SetGeomOffset(recomp_context *ctx);

/// SetGeomScreen(h): projection-plane distance H (16-bit).
void hle_libgte_SetGeomScreen(recomp_context *ctx);

/// SetDQA(dqa): depth-cue gradient (16-bit).
void hle_libgte_SetDQA(recomp_context *ctx);

void psyq_register_libgte();

} // namespace ps1::psyq
