#pragma once
/**
 * @file psyq_libcd.h
 * @brief HLE for PsyQ libcd entries detected on the boot path (Sessao 0.7).
 *
 * The full CD command set (CdRead/CdControl/etc.) is still served by the
 * legacy CdInit-style HLE path; this header only covers the small handful
 * of libcd helpers that the hash matcher catches in Rayman.
 */

#include "runtime/cpu_context.h"

namespace ps1::psyq {

/// CdGetSector(*madr, size): copy the current sector into RAM.
/// `size` is in 32-bit words.  Returns 1 if a sector was copied, 0 otherwise.
void hle_libcd_CdGetSector(recomp_context *ctx);

/// StSetMask(table, n): set the XA streaming sector filter.  XA streaming is
/// not modeled, so this is a NOP that returns 0 (success).
void hle_libcd_StSetMask(recomp_context *ctx);

void psyq_register_libcd();

} // namespace ps1::psyq
