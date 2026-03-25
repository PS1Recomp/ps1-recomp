#pragma once
/**
 * @file psyq_hle.h
 * @brief PsyQ SDK High-Level Emulation (HLE) interface.
 *
 * When ps1Recomp identifies PsyQ SDK functions by name (via `[stubs]` in the
 * game TOML), it emits calls into this module instead of translating the MIPS.
 * This gives VSync, DrawSync, DrawOTag, and display-environment functions
 * correct behaviour without requiring per-game address hacks.
 *
 * **Setup:** call `ps1::psyq::configure()` once from `main_host.cpp` after
 * `Bios::setPsyqAddresses()` so the HLE layer knows the VBlank counter
 * address, GPU callbacks, and drain function for the current game.
 */

#include "runtime/cpu_context.h"
#include <cstdint>
#include <functional>

namespace ps1::psyq {

// ── Global per-game configuration ─────────────────────────────────────────

/**
 * @brief Per-game configuration for the PsyQ HLE layer.
 *
 * Populated by `main_host.cpp` at startup and passed to `psyq::configure()`.
 * All `uint32_t` address fields are **physical PS1 RAM addresses** — apply
 * `addr & 0x1FFFFFFF` before indexing into `rdram`.
 */
struct HleConfig {
  uint32_t vblankCounter = 0; ///< PSX RAM address of the VBlank tick counter
  uint32_t drawSyncBase = 0;  ///< BSS addr holding pointer to OT status array
  uint32_t drawSyncIndexAddr = 0; ///< BSS addr holding current OT index
  uint32_t drawSyncCount = 2;     ///< Number of double-buffered OT entries

  /// Drain pending BIOS callbacks (must run on game thread at yield points).
  /// Set to Bios::drainPendingCallbacks via a lambda in main_host.cpp.
  std::function<void()> drainCallbacks;

  /// GPU command port (GP0) write — used by DrawOTag and PutDrawEnv.
  std::function<void(uint32_t)> writeGP0;

  /// GPU control port (GP1) write — used by PutDispEnv.
  std::function<void(uint32_t)> writeGP1;

  // ── Generic internal-state callbacks (game-agnostic) ──
  //
  // These replace per-game BSS address polling.  Wire to Bios methods in
  // main_host.cpp so HLE stubs can block without per-game configuration.

  /// Block until `frames` more VBlanks have fired. Returns new VBlank count.
  std::function<uint32_t(uint32_t frames)> waitVSync;

  /// Block until a CD command completes (INT2/INT3). Returns 2 on success,
  /// 5 on disk error, 0 on timeout.
  std::function<uint8_t(int timeoutMs)> waitForCdSync;

  /// Block until CD data is ready (INT1/INT4). Returns 1/4 on success,
  /// 5 on error, 0 on timeout.
  std::function<uint8_t(int timeoutMs)> waitForCdReady;
};

/// Configure the HLE layer for the current game.  Call from Bios or main_host
/// after setPsyqAddresses().
void configure(const HleConfig &cfg);

// ── PsyQ SDK HLE implementations ─────────────────────────────────────────

/// VSync(n) — wait for n vertical blanks then return the total VBlank count.
/// When n == 0, just drains pending callbacks and returns the current count.
void hle_VSync(recomp_context *ctx);

/// DrawSync(mode) — wait for GPU drawing to complete.
/// Since the runtime GPU is fully synchronous, this always returns 0
/// immediately (mode 0) or the current command-queue depth (mode 1).
void hle_DrawSync(recomp_context *ctx);

/// ResetGraph(mode) — reset GPU to a known state.
/// mode 0 → flush + clear; mode 3 → flush only.
/// Implemented as a NOP here because GPU reset is handled by the runtime.
void hle_ResetGraph(recomp_context *ctx);

/// ClearOTag(ot, n) — fill an ordering-table with end-of-list terminators.
/// a0 = ot pointer, a1 = n (number of entries, each 4 bytes).
void hle_ClearOTag(recomp_context *ctx);

/// ClearOTagR(ot, n) — same as ClearOTag but fills in reverse order.
void hle_ClearOTagR(recomp_context *ctx);

/// DrawOTag(ot) — traverse ordering-table linked list and submit GP0 commands.
/// a0 = pointer to the tail of the OT (highest priority node).
/// Each node: bits[31:24]=word_count, bits[23:0]=next_ptr; followed by GP0 words.
void hle_DrawOTag(recomp_context *ctx);

/// SetDefDispEnv(env, x, y, w, h) — initialise a DispEnv struct in PS1 RAM.
/// a0=*DispEnv, a1=x, a2=y, a3=w, stack+16=h
void hle_SetDefDispEnv(recomp_context *ctx);

/// PutDispEnv(env) — apply a DispEnv to the GPU via GP1 commands.
/// a0=*DispEnv
void hle_PutDispEnv(recomp_context *ctx);

/// SetDefDrawEnv(env, x, y, w, h) — initialise a DrawEnv struct in PS1 RAM.
/// a0=*DrawEnv, a1=x, a2=y, a3=w, stack+16=h
void hle_SetDefDrawEnv(recomp_context *ctx);

/// PutDrawEnv(env) — apply a DrawEnv to the GPU via GP0 commands.
/// a0=*DrawEnv
void hle_PutDrawEnv(recomp_context *ctx);

} // namespace ps1::psyq
