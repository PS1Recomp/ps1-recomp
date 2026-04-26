#include "runtime/psyq/psyq_libcd.h"
#include "runtime/psyq/psyq_registry.h"

namespace ps1::psyq {

// CdGetSector(*madr, size_words):
//   On real hardware this reads the most-recent CD sector out of the SPU bus
//   into RAM.  Streaming reads are routed through the BIOS cd-data callback
//   in our HLE, which copies data via DMA before any user code asks for it.
//   When this entry point IS hit, we have no buffered sector to hand back;
//   returning 0 reports "not ready" so the caller falls through.
void hle_libcd_CdGetSector(recomp_context *ctx) {
  ctx->r[V0] = 0;
}

// StSetMask(table, n): XA-stream sector filter.  Not modeled.
void hle_libcd_StSetMask(recomp_context *ctx) {
  ctx->r[V0] = 0;
}

void psyq_register_libcd() {
  psyq_register("libcd_CdGetSector", &hle_libcd_CdGetSector);
  psyq_register("libcd_StSetMask",   &hle_libcd_StSetMask);
}

} // namespace ps1::psyq
