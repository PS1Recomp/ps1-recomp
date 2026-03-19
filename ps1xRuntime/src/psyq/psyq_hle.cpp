#include "runtime/psyq/psyq_hle.h"
#include "runtime/memory.h"
#include <fmt/format.h>

namespace ps1::psyq {

// ── Module-level config ────────────────────────────────────────────────────
static HleConfig g_cfg;

void configure(const HleConfig &cfg) { g_cfg = cfg; }

// ── Helpers ────────────────────────────────────────────────────────────────

static inline uint32_t readVBlankCount(recomp_context *ctx) {
  if (g_cfg.vblankCounter == 0)
    return 0;
  return ctx->mem->read32(g_cfg.vblankCounter);
}

static inline void drainOnce() {
  if (g_cfg.drainCallbacks)
    g_cfg.drainCallbacks();
}

// ── VSync ─────────────────────────────────────────────────────────────────
//
// PsyQ VSync(n):
//   n == 0 → sync to next VBlank (wait for counter to change)
//   n  > 0 → wait until n more VBlanks have elapsed
//   Returns the total VBlank counter value.
//
// Implementation strategy: we spin-yield by draining pending callbacks.
// The main thread increments vblankCounter each VBlank via triggerVBlankEvent.
// Each drainOnce() call pumps the callback queue; the loop exits quickly once
// the main thread delivers the next VBlank tick.
//
void hle_VSync(recomp_context *ctx) {
  int32_t n = static_cast<int32_t>(ctx->r[A0]);

  if (g_cfg.vblankCounter == 0) {
    // No counter configured — just yield and return 0
    drainOnce();
    ctx->r[V0] = 0;
    return;
  }

  uint32_t start = readVBlankCount(ctx);

  if (n <= 0) {
    // Wait for at least one VBlank tick
    uint32_t target = start + 1;
    int safety = 0;
    while (readVBlankCount(ctx) < target && safety < 10000) {
      drainOnce();
      ++safety;
    }
  } else {
    uint32_t target = start + static_cast<uint32_t>(n);
    int safety = 0;
    while (readVBlankCount(ctx) < target && safety < 100000) {
      drainOnce();
      ++safety;
    }
  }

  ctx->r[V0] = readVBlankCount(ctx);
}

// ── DrawSync ──────────────────────────────────────────────────────────────
//
// PsyQ DrawSync(mode):
//   mode 0 → wait until GPU drawing is complete, return 0
//   mode 1 → return number of primitives remaining (non-blocking)
//
// Since the runtime GPU processes GP0 commands synchronously, drawing is
// always "complete".  Return 0 for both modes.
//
void hle_DrawSync(recomp_context *ctx) {
  // Drain once to keep event queue healthy
  drainOnce();
  ctx->r[V0] = 0; // 0 = complete / 0 primitives remaining
}

// ── ResetGraph ────────────────────────────────────────────────────────────
//
// PsyQ ResetGraph(mode):
//   mode 0 → reset + flush + clear display list
//   mode 3 → flush only
//
// The runtime GPU has no queued command list to flush, so this is a NOP.
//
void hle_ResetGraph(recomp_context *ctx) {
  (void)ctx;
  // NOP: runtime GPU is synchronous, no list to flush
}

// ── ClearOTag ─────────────────────────────────────────────────────────────
//
// PsyQ ClearOTag(ot, n):
//   Fills the first `n` entries of ordering table `ot` with end-of-list
//   terminators.  On real hardware each entry is a 24-bit linked-list pointer
//   that forms a chain; the last entry (ot[0]) must hold 0x00FFFFFF.
//
//   a0 = base address of the OT (uint32_t*)
//   a1 = number of entries
//
void hle_ClearOTag(recomp_context *ctx) {
  uint32_t base = ctx->r[A0];
  uint32_t n = ctx->r[A1];
  if (n == 0) {
    ctx->r[V0] = base;
    return;
  }
  // Fill [1 .. n-1] with self-pointers (each entry points to previous)
  for (uint32_t i = 0; i < n - 1; i++) {
    ctx->mem->write32(base + i * 4, base + (i - 1) * 4);
  }
  // Last entry = 0x00FFFFFF (end-of-list sentinel)
  ctx->mem->write32(base + (n - 1) * 4, 0x00FFFFFFu);
  ctx->r[V0] = base; // return pointer to ot
}

// ── ClearOTagR ────────────────────────────────────────────────────────────
//
// Same as ClearOTag but fills in reverse order — entries are linked
// high-to-low so GPU traverses them from ot[n-1] down to ot[0].
//
void hle_ClearOTagR(recomp_context *ctx) {
  uint32_t base = ctx->r[A0];
  uint32_t n = ctx->r[A1];
  if (n == 0) {
    ctx->r[V0] = base;
    return;
  }
  // ot[n-1] = end-of-list
  ctx->mem->write32(base + (n - 1) * 4, 0x00FFFFFFu);
  // ot[i] links forward: ot[i] -> ot[i+1]
  for (uint32_t i = 0; i < n - 1; i++) {
    ctx->mem->write32(base + i * 4, base + (i + 1) * 4);
  }
  ctx->r[V0] = base;
}

} // namespace ps1::psyq
