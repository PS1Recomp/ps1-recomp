#include "runtime/psyq/psyq_libetc.h"
#include "runtime/memory.h"
#include "runtime/psyq/psyq_registry.h"
#include "runtime/metrics.h"
#include <fmt/format.h>
#include "runtime/psyq/psyq_state.h"

#include <cstdint>

namespace ps1::psyq {

namespace {

// I_MASK / I_STAT live in the PS1 IO segment.  Real address is 0x1F801074;
// our memory_init / runtime model exposes it as a normal read/write target.
constexpr uint32_t kIMaskAddr = 0x1F801074;

// PSY-Q caps `n` for InterruptCallback/DMACallback at 8 slots.  Matches our
// PsyqState arrays (kIntrSlots).
inline std::size_t clampSlot(uint32_t n) {
  return (n < PsyqState::kIntrSlots) ? static_cast<std::size_t>(n) : 0;
}

} // namespace

// ResetCallback: re-initialises the interrupt subsystem.  Real PSY-Q
// allocates state, installs entry-point handlers and returns the address
// of an internal sentinel ("non-zero == initialised").  We have no IRQs
// to install -- callbacks are drained cooperatively from `hle_VSync` -- so
// reset the soft state and hand back a non-zero token.  A non-zero return
// is load-bearing: PSY-Q boilerplate checks it to detect init failure and
// aborts the main loop when it sees zero.
void hle_libetc_ResetCallback(recomp_context *ctx) {
  // ResetCallback *starts* the callback module; it does not forget what the
  // game registered:
  //
  //     int ResetCallback(void) { return D_800B7080->start(); }
  //
  // (PsyQ libetc, psyz decompilation src/libetc/intr.c.)  Clearing the slots
  // here was wrong and load-bearing: `PadInit` calls ResetCallback, so every
  // pad re-init silently unregistered Crash's Timer0 sound tick.  With the
  // tick gone the drain had nothing to dispatch, `DeliverEvent(0xF0000009,
  // 0x20)` stopped firing, and the level loader's `TestEvent(9)` spun forever
  // -- measured at 15 million polls against 9 hits.
  auto &s = psyq_state();
  s.callbacksEnabled = true;
  ctx->r[V0] = 1;
}

// StopCallback / RestartCallback: gate the cooperative callback path.
// We honour the flag for `CheckCallback` but the runtime drains anyway --
// real IRQ masking would suppress dispatch, which we cannot model without
// a full COP0 emulation.  Returning the previous state matches PSY-Q.
void hle_libetc_StopCallback(recomp_context *ctx) {
  auto &s = psyq_state();
  uint32_t prev = s.callbacksEnabled ? 1u : 0u;
  s.callbacksEnabled = false;
  ctx->r[V0] = prev;
}

void hle_libetc_RestartCallback(recomp_context *ctx) {
  auto &s = psyq_state();
  uint32_t prev = s.callbacksEnabled ? 1u : 0u;
  s.callbacksEnabled = true;
  ctx->r[V0] = prev;
}

// CheckCallback: returns non-zero iff currently executing inside a
// callback context.  In the cooperative model we are never inside one
// from the game thread's perspective.  Some PSY-Q routines branch on
// this to choose between sync and async paths; returning 0 selects the
// sync path which matches our runtime.
void hle_libetc_CheckCallback(recomp_context *ctx) {
  ctx->r[V0] = 0;
}

// InterruptCallback(n, fn): store the per-IRQ-line callback, return prev.
// Slot 0..7 follow the PSY-Q convention (VBlank, GPU, CDROM, DMA, RTC0/1/2,
// reserved).  Not actually dispatched today -- see note in psyq_state.h.
void hle_libetc_InterruptCallback(recomp_context *ctx) {
  std::size_t n = clampSlot(ctx->r[A0]);
  auto &s = psyq_state();
  uint32_t prev = s.intrCallback[n];
  s.intrCallback[n] = ctx->r[A1];
  ctx->r[V0] = prev;
  // Which IRQ lines a game actually hooks, and with what.  There is one slot
  // per line, so a second registration on the same line silently displaces
  // the first -- and the drain only ticks lines 0 and 4..6, so a handler
  // parked anywhere else never runs at all.
  if (ps1::metrics::enabled()) {
    ps1::metrics::count(fmt::format("psyq.intr_cb.{}.{:08X}", n, ctx->r[A1]));
    if (prev != 0 && prev != ctx->r[A1])
      ps1::metrics::count(fmt::format("psyq.intr_cb_displaced.{}.{:08X}", n, prev));
  }
}

void hle_libetc_DMACallback(recomp_context *ctx) {
  std::size_t n = clampSlot(ctx->r[A0]);
  auto &s = psyq_state();
  uint32_t prev = s.dmaCallback[n];
  s.dmaCallback[n] = ctx->r[A1];
  ctx->r[V0] = prev;
  if (ps1::metrics::enabled())
    ps1::metrics::count(fmt::format("psyq.dma_cb.{}.{:08X}", n, ctx->r[A1]));
}

// SetIntrMask / GetIntrMask: round-trip the I_MASK hardware register.
// Real silicon (0x1F801074) gates which IRQs the CPU sees; we maintain
// a software mirror in PsyqState plus a write-through to the IO address
// so other subsystems (cdrom_controller, dma) read the same value the
// game wrote.
void hle_libetc_SetIntrMask(recomp_context *ctx) {
  uint32_t mask = ctx->r[A0] & 0xFFFFu;
  auto &s = psyq_state();
  uint32_t prev = s.intrMask;
  s.intrMask = static_cast<uint16_t>(mask);
  ctx->mem->write32(kIMaskAddr, mask);
  ctx->r[V0] = prev;
}

void hle_libetc_GetIntrMask(recomp_context *ctx) {
  ctx->r[V0] = psyq_state().intrMask;
}

// startIntr / stopIntr / restartIntr: low-level entries that PSY-Q's
// ResetCallback / StopCallback / RestartCallback delegate to via the
// `D_800B7080->start/stop/restart` indirection.  Since the runtime has
// no IRQs to actually start/stop, these have the same observable effect
// as their high-level counterparts.  Aliasing keeps direct callers
// honest without duplicating logic.
void hle_libetc_startIntr(recomp_context *ctx)   { hle_libetc_ResetCallback(ctx); }
void hle_libetc_stopIntr(recomp_context *ctx)    { hle_libetc_StopCallback(ctx); }
void hle_libetc_restartIntr(recomp_context *ctx) { hle_libetc_RestartCallback(ctx); }

void psyq_register_libetc_intr() {
  psyq_register("libetc_ResetCallback",     &hle_libetc_ResetCallback);
  psyq_register("libetc_StopCallback",      &hle_libetc_StopCallback);
  psyq_register("libetc_RestartCallback",   &hle_libetc_RestartCallback);
  psyq_register("libetc_CheckCallback",     &hle_libetc_CheckCallback);
  psyq_register("libetc_InterruptCallback", &hle_libetc_InterruptCallback);
  psyq_register("libetc_DMACallback",       &hle_libetc_DMACallback);
  psyq_register("libetc_SetIntrMask",       &hle_libetc_SetIntrMask);
  psyq_register("libetc_GetIntrMask",       &hle_libetc_GetIntrMask);
  psyq_register("libetc_startIntr",         &hle_libetc_startIntr);
  psyq_register("libetc_stopIntr",          &hle_libetc_stopIntr);
  psyq_register("libetc_restartIntr",       &hle_libetc_restartIntr);
}

} // namespace ps1::psyq
