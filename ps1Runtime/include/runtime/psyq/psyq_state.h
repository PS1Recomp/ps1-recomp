#pragma once
/**
 * @file psyq_state.h
 * @brief Centralised C++ home for PsyQ runtime state (Phase 2, Session 2.1).
 *
 * Until Phase 2, every piece of "PsyQ-side" state — VBlank counter, CD sync /
 * ready bytes, sector-read bookkeeping, async callbacks, GPU swap callback,
 * DrawSync OT tracking — lived in PS1 RAM at game-specific BSS addresses
 * declared in `[psyq_addresses]` of each game TOML.  That model needs the
 * recompiled binary to actually initialise those addresses, which broke on
 * Crash Bandicoot (`cd_data_cb` is never written from MIPS, so `bios.cpp`
 * INT1 never finds a callback to invoke and `dma2_count` stays at 2).
 *
 * `PsyqState` is the C++ replacement: a process-wide singleton holding all
 * the values the HLE layer used to poke into PS1 RAM.  It is created lazily
 * by `psyq_state()` and lives until program exit.
 *
 * **Phase 2 status (post-2.4):** every former BSS slot now lives here —
 * VSync counter (2.2), CD sync/ready bytes (2.3), CD sector bookkeeping +
 * callbacks + GPU swap callback + DrawSync OT slots (2.4).  The
 * `[psyq_addresses]` block has been removed from `rayman.toml`/`crash.toml`
 * and the `Bios::PsyqAddresses` struct deleted.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ps1::psyq {

/**
 * @brief OT (ordering-table) status slots formerly tracked at
 *        `gpu_drawsync_base` / `gpu_drawsync_index_addr` in BSS.
 *
 * Each entry mirrors one double-buffered OT and holds the per-slot
 * "drawing complete" flag that PsyQ's DrawSync polls.  `count` equals the
 * number of slots in use (2 for typical double-buffering); `index` is the
 * current write slot.  Slots beyond `count` are unused.
 *
 * The struct is intentionally shallow — Phase 2 only stores what the legacy
 * BSS layout stored.  `Bios::triggerVBlankEvent` stamps every active slot
 * with status == 2 (PsyQ "drawing complete") since the runtime GPU is
 * fully synchronous; future async-render work can refine to per-slot
 * atomics if needed.
 */
struct GpuDrawSync {
  /// Maximum number of OT slots we track.  Two is typical (front/back
  /// buffer); a few engines use 4.  Eight is generous head-room.
  static constexpr std::size_t kMaxSlots = 8;

  uint32_t status[kMaxSlots] = {}; ///< Per-slot completion words.
  uint32_t index = 0;              ///< Currently active slot.
  uint32_t count = 2;              ///< Number of valid slots in `status`.
};

/**
 * @brief Centralised PsyQ runtime state.
 *
 * Singleton — instantiated lazily on first call to `psyq_state()` and
 * shared by HLE handlers across `psyq_*.cpp` and the VBlank thread in
 * `main_host.cpp`.  Atomic fields are touched from multiple threads
 * (game thread + VBlank thread + CDROM IRQ thread); plain fields are
 * written only from the game thread.
 */
class PsyqState {
public:
  // ── VBlank ────────────────────────────────────────────────────────────
  /// Tick counter incremented by `main_host.cpp`'s VBlank thread at 60 Hz.
  /// `hle_VSync` reads this to compute the wait target.  Replaces the
  /// per-game `vsync_counter` BSS address.
  std::atomic<uint32_t> vsyncCounter{0};

  /// Cross-thread flag: VBlank thread sets `true` once per tick (release);
  /// game thread exchanges `false` at the end of each `hle_VSync`
  /// (acq_rel) and, when it observed `true`, runs the actual VBlank
  /// delivery (`Bios::triggerVBlankEvent`) on the game thread.  Phase 3.2
  /// pulled all PsyqState/event-system writes off the IRQ-context VBlank
  /// thread; only this single-byte atomic is touched there now.  Multiple
  /// VBlanks coalesce into a single delivery — PsyQ event semantics are
  /// status-flag based (TestEvent/EnableEvent), so coalescing does not
  /// drop user-visible state.
  std::atomic<bool> vblankPending{false};

  // ── CDROM sync flags ─────────────────────────────────────────────────
  /// Set by `Bios::triggerCdromEvent(CdlComplete)` to signal CdSync
  /// completion.  Replaces `cd_sync_byte` BSS slot.
  std::atomic<uint8_t> cdSyncByte{0};

  /// Set by `Bios::triggerCdromEvent(CdlDataReady|CdlDataEnd)` to signal
  /// CdReady completion.  Replaces `cd_ready_byte` BSS slot.
  std::atomic<uint8_t> cdReadyByte{0};

  // ── CDROM read bookkeeping ───────────────────────────────────────────
  // Touched only from the game thread (libcd HLE entries) and the
  // CDROM IRQ handler running on the same thread (synchronous host
  // model).  Plain integers — no atomic semantics required yet.
  uint32_t cdRemaining = 0; ///< Sectors still pending in the active read.
  uint32_t cdDestPtr   = 0; ///< PS1 RAM addr where the next sector lands.
  uint32_t cdWordCount = 0; ///< Words per sector (512 raw, 585 with subhdr).

  // ── CDROM async callbacks ────────────────────────────────────────────
  /// Game-side callback registered by `CdReadCallback` / `CdReadyCallback`.
  /// `bios.cpp` INT1/INT4 dispatch will invoke this once Phase 2.4 lands.
  uint32_t cdDataCb   = 0;
  /// Notify-side callback (`CdDataCallback` legacy alias).
  uint32_t cdNotifyCb = 0;

  // ── GPU async callback ───────────────────────────────────────────────
  /// Display-swap callback registered by `VSyncCallback`.  Replaces
  /// `gpu_swap_cb` BSS slot.
  uint32_t gpuSwapCb = 0;

  // ── GPU DrawSync OT slots ────────────────────────────────────────────
  /// Replaces the BSS triplet `gpu_drawsync_base` /
  /// `gpu_drawsync_index_addr` / `gpu_drawsync_count`.
  GpuDrawSync drawSync;

  // ── Test helpers ─────────────────────────────────────────────────────
  /// Reset every field to its default-constructed value.  Tests call
  /// this in `SetUp()` to isolate cases sharing the singleton.
  void reset();

  PsyqState(const PsyqState &) = delete;
  PsyqState &operator=(const PsyqState &) = delete;

private:
  PsyqState() = default;
  friend PsyqState &psyq_state();
};

/// Access the process-wide PsyqState singleton.
PsyqState &psyq_state();

} // namespace ps1::psyq
