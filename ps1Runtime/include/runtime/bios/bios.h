#pragma once
/**
 * @file bios.h
 * @brief PS1 BIOS HLE -- A0/B0/C0 syscall dispatch, event system, callback queue.
 *
 * `ps1::bios::Bios` is the High-Level Emulation of the PS1 kernel.  The
 * runtime contains no original Sony code; instead the three syscall tables
 * (A, B, C) are implemented as native C++ in `syscall_a.cpp`, `syscall_b.cpp`
 * and `syscall_c.cpp`, with file I/O, heap, and event-system helpers split
 * into their own translation units.
 *
 * Threading: most state is touched only from the game thread.  CDROM IRQs
 * arrive from the SDL render thread and are pushed onto a queue
 * (`queueCdromEvent`) that the game thread drains via
 * `drainPendingCallbacks` -- see Phase 3.3 for the rationale.
 */

#include "runtime/bios/event_system.h"
#include "runtime/bios/file_io.h"
#include "runtime/bios/heap.h"
#include "runtime/cdrom/virtual_fs.h"
#include "runtime/cpu_context.h"
#include "runtime/memory.h"
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <queue>
#include <thread>

namespace ps1::gpu {
class GPU;
}
namespace ps1::input {
class InputController;
class MemoryCard;
}
namespace ps1::cdrom {
class CdromController;
}
namespace ps1 {
class DMA;
}

namespace ps1::bios {

/**
 * @brief HLE replacement for the PS1 kernel -- syscall dispatch + events + callbacks.
 *
 * Constructed once per process and held by `main_host.cpp`.  Hardware
 * subsystems (GPU/CDROM/Input/DMA) are attached after construction via
 * `setGPU`/`setCdromController`/etc., so the BIOS can drive them when a
 * recompiled syscall lands on its table.
 */
class Bios {
public:
  Bios(recomp_context &ctx, cdrom::VirtualFs &fs, Memory &mem);
  ~Bios();

  // Hardware attachment (call after construction)
  void setGPU(gpu::GPU *gpu) { gpu_ = gpu; }
  void setInputController(input::InputController *input) { input_ = input; }
  void setCdromController(cdrom::CdromController *cdrom) { cdrom_ = cdrom; }
  void setDma(::ps1::DMA *dma) { dma_ = dma; }

  // Read-only accessor used by libcd HLE (psyq_libcd.cpp) to drive the
  // backend controller directly. Returns nullptr when not attached (typical
  // for unit tests that don't exercise the disc subsystem).
  cdrom::CdromController *cdromController() const { return cdrom_; }

  // Same idea for the libetc Pad HLE (psyq_pad.cpp). Returns nullptr in tests
  // or headless runs that don't wire SDL input.
  input::InputController *inputController() const { return input_; }

  // The single entry-point when PC jumps to A0, B0 or C0.
  // Handles reading the function index and triggering the right stub.
  void executeA0();
  void executeB0();
  void executeC0();

  // Called every VBlank to update pad buffers (InitPAD/StartPAD)
  void updatePadBuffers();

  // Trigger CDROM events in the event system based on interrupt type.
  // Called on the GAME thread (from libcd HLE syscall handlers and from
  // `drainCdromEventQueue` consuming queued cross-thread IRQs).  Touches
  // non-atomic event-system state, so it must NOT be invoked directly
  // from the SDL render thread or any other thread that does not own the
  // recompilation context.
  void triggerCdromEvent(uint8_t cdIntType);

  // Cross-thread CDROM IRQ entry-point (Phase 3.3).  The cdrom controller's
  // `interruptCallback` is wired here so SDL-render-thread `tick()` and
  // game-thread `writeRegister` can both raise IRQs without racing on
  // event-system state.  Push is `O(1)` under a short mutex.  Game-thread
  // code drains the queue via `drainCdromEventQueue` (called from
  // `drainPendingCallbacks` and `hle_libcd_CdSync`).
  void queueCdromEvent(uint8_t cdIntType);

  // Drain queued CDROM IRQs and run `triggerCdromEvent` for each on the
  // current (game) thread.  Returns the number of events drained.  Safe
  // to call on any iteration of a polling loop -- events are queued
  // (push) cross-thread and drained (pop + dispatch) game-thread-only.
  std::size_t drainCdromEventQueue();

  // Trigger VBlank event in the event system.
  void triggerVBlankEvent();

  // Invoke the handler installed via B0:0x19 (SetCustomExitFromException),
  // if any.  Must run on the game thread (touches `recomp_context` GPRs).
  void triggerCustomException();

  // Generic internal-state accessors (game-agnostic)
  //
  // VBlank waits live in `psyq_state().vsyncCounter` since Phase 2.2.
  // CD sync/ready waits live in `psyq_state().cdSyncByte/cdReadyByte`
  // since Phase 2.3 -- `hle_libcd_CdSync`/`CdReady` poll those atomics
  // cooperatively without needing per-game BSS addresses.

  // Drain pending event callbacks -- called from game thread at yield points
  // (testEvent, waitEvent, VSync, etc.) to safely dispatch mode-0x1000
  // handlers.
  //
  // This is also the yield point the recompiler injects at *every backward
  // branch*, so it runs orders of magnitude more often than it has work to
  // do.  The slow path below takes three mutexes and half a dozen atomic
  // read-modify-writes; paying that per loop iteration costs more than the
  // emulated loop body.  So the entry point is a lock-free gate over the
  // same set of pending flags, and the real work lives in
  // `drainPendingCallbacksSlow`.  Set `PS1_DRAIN_GATE=0` to bypass the gate
  // and always take the slow path (A/B measurement).
  void drainPendingCallbacks() {
    ++drainCalls_;
    if ((++pumpCounter_ & 0x3FFu) == 0)
      pumpVBlank();
    if (spGuard_)
      checkStackPointer();
    // A yield point is only safe where the guest has a usable stack.
    //
    // Hand-written PS1 assembly is free to save the whole register file to a
    // context block and then use $sp as a general-purpose register -- Crash's
    // model transform `func_80035E10` parks $sp in the scratchpad and packs
    // GTE operands through it for the length of the routine.  On hardware an
    // interrupt there is harmless: the exception handler runs on its own
    // stack.  Here, dispatching a callback would run recompiled code that
    // does `addiu $sp,-32; sw $ra,24($sp)` against a data value, so its
    // locals land in nowhere -- measured: the sound tick's loop counter never
    // read back what it wrote and span 1.4 billion times, wedging the game
    // thread behind the re-entrancy guard.
    //
    // Deferring costs nothing: the callback stays queued and is dispatched at
    // the next yield point that does have a stack, a few microseconds later.
    if (!guestStackUsable()) {
      ++drainDeferredNoStack_;
      return;
    }
    if (drainGate_ && vsyncPtr_ != nullptr &&
        cdEventQueueDepth_.load(std::memory_order_relaxed) == 0 &&
        cdIntPending_.load(std::memory_order_relaxed) == 0 &&
        cdExceptionPending_.load(std::memory_order_relaxed) == 0 &&
        vblankExceptionPending_.load(std::memory_order_relaxed) == 0 &&
        !eventSystem_.hasPendingCallbacks() &&
        vsyncPtr_->load(std::memory_order_relaxed) == lastIntrTickFrame_) {
      return;
    }
    drainPendingCallbacksSlow();
  }

  // Yield point that keeps the host clock running but delivers nothing.
  //
  // Inside a routine that must not be interrupted, the guest still needs the
  // VBlank pump to advance -- the decompressor runs for millions of iterations
  // and stopping the clock there wedges every VSync wait behind it. What it
  // cannot tolerate is a dispatched callback: it finishes with the output
  // count complete but a pending run count, and its exit condition can then
  // never be met. So pump, check the stack, dispatch nothing.
  void pumpOnly() {
    ++drainCalls_;
    if ((++pumpCounter_ & 0x3FFu) == 0)
      pumpVBlank();
    if (spGuard_)
      checkStackPointer();
  }

  void drainPendingCallbacksSlow();

  // Host VBlank pump, run on the GAME thread.
  //
  // The 60 Hz tick used to live on its own thread, which meant the host
  // advanced the guest clock, refreshed the pad buffer and snapshotted VRAM
  // while recompiled code was running -- concurrent access to guest state that
  // showed up as ~180 KB of globals turning to garbage in a single frame,
  // roughly one run in four.  Proven by A/B: adding any work to that thread
  // made the corruption disappear.
  //
  // Driving the same tick from the game thread's own yield points removes the
  // concurrency without losing the clock during spin-waits: loops that never
  // call VSync (the NSF loader is one) still reach a backward branch, and the
  // counter below keeps the pump cheap enough for that path.
  void setVBlankPump(std::function<void()> pump) {
    vblankPump_ = std::move(pump);
  }
  void pumpVBlank() {
    if (vblankPump_)
      vblankPump_();
  }

  // Global $sp watchpoint (`PS1_SP_GUARD=1`).
  //
  // The recompiler injects a drain at every backward branch, which makes this
  // the one place every loop in the program passes through -- so validating
  // the guest stack pointer here catches corruption at the first loop after
  // it happens, anywhere, without knowing in advance which function to watch.
  // A guest $sp must land in RAM or the scratchpad; anything else means a
  // recompilation bug (a clobbered $sp save/restore, a bad computed jump)
  // rather than game behaviour.
  void checkStackPointer() {
    const uint32_t sp = ctx_.r29;
    const uint32_t masked = sp & 0x1FFFFFFFu;
    if (masked < 0x200000u ||
        (masked >= 0x1F800000u && masked < 0x1F800400u))
      return;
    reportBadStackPointer(sp);
  }

  // Can a dispatched callback push a frame at the current guest $sp?
  //
  // Main RAM only: the 1 KiB scratchpad is small enough that routines which
  // park $sp there are using it as working storage, so a callback frame would
  // overwrite live data rather than fail loudly.  A frame needs headroom
  // below $sp, hence the floor.
  bool guestStackUsable() const {
    const uint32_t masked = ctx_.r29 & 0x1FFFFFFFu;
    return masked >= 0x400u && masked < 0x200000u;
  }

  // Diagnostic snapshot (PS1_METRICS).  Written on the game thread, read
  // from the reporting thread.
  struct DrainMetrics {
    uint64_t calls;   // every injected yield point
    uint64_t slow;    // reached the slow path
    uint64_t nested;  // refused by the re-entrancy guard
    uint64_t dispatched;
    uint64_t noStack; // deferred: guest $sp was not a usable stack
  };
  DrainMetrics drainMetrics() const {
    return {drainCalls_, drainSlow_, drainNested_, drainDispatched_,
            drainDeferredNoStack_};
  }
  const EventSystem &eventSystem() const { return eventSystem_; }

  /// Re-entrancy guard for `drainPendingCallbacks`.
  ///
  /// A drained callback is recompiled game code, and the recompiler injects a
  /// drain at every backward branch -- so a callback with a loop in it calls
  /// back into the drain, which dispatches the same callback again. Measured in
  /// Crash Bandicoot: the sound-engine tick `func_800466A0` re-entered without
  /// bound, 85 million drain calls against 186 thousand real yield points, and
  /// the game thread never returned to its main loop.
  ///
  /// Hardware has the same rule -- an interrupt handler does not re-enter the
  /// dispatcher -- so refusing the nested call is the faithful behaviour, not a
  /// workaround.
  bool draining_ = false;

  // BSS mirrors for legacy MIPS polling.  Phase 2.3/2.4 retired the BSS
  // writes of cd_sync_byte / cd_ready_byte in favor of `psyq_state()`
  // atomics; recompiled native MIPS code may still poll the original BSS
  // address (Rayman PsyQ CdReset reads `0x801CF1D8` / `0x801CF1DC`).
  // When set non-zero, `triggerCdromEvent` writes the same INT-mapped
  // value to PS1 RAM that the legacy BIOS handler used to write.
  void setBssMirrors(uint32_t cdSyncMirror, uint32_t cdReadyMirror) {
    cdSyncMirror_ = cdSyncMirror;
    cdReadyMirror_ = cdReadyMirror;
  }

  // Identify the game thread so `queueCdromEvent` can drain inline when
  // called from it (recompiled MIPS writes to CDROM ports trigger the IRQ
  // callback synchronously on the game thread; without inline drain, the
  // BSS mirror writes never happen and PsyQ polling spins on stale BSS).
  // Cross-thread pushes (cdromCtrl.tick from the SDL render thread) keep
  // the queue's async semantics -- they're drained from drainPendingCallbacks
  // / hle_libcd_CdSync.
  void setGameThreadId(std::thread::id id) { gameThreadId_ = id; }

private:
  recomp_context &ctx_;
  Heap heap_;
  EventSystem eventSystem_;
  FileIO fileIo_;

  // Hardware pointers (optional, set via setters)
  gpu::GPU *gpu_ = nullptr;
  input::InputController *input_ = nullptr;
  cdrom::CdromController *cdrom_ = nullptr;
  ::ps1::DMA *dma_ = nullptr;

  // Memory card BIOS functions (syscall_b.cpp).  I/O completes at once and
  // reports through the SwCARD/HwCARD events.
  enum class CardOp { Info, Read, Write };
  uint32_t cardRequest(uint32_t port, CardOp op, uint32_t sector, uint32_t addr);
  bool cardChangeAcked_ = false;     // _new_card: ignore the change flag once
  uint8_t cardStatus_[2] = {1, 1};   // _card_status per slot

  // Memory card files ("bu00:"/"bu10:").  Their handles start above FileIO's
  // so one fd space covers both devices.
  struct CardFile {
    bool open = false;
    int slot = 0;
    int block = -1; // first block
    uint32_t pos = 0;
    uint32_t size = 0;
  };
  static constexpr int32_t kCardFdBase = 16;
  CardFile cardFiles_[16];
  std::string cardSearch_; // firstfile/nextfile pattern, without the device
  int cardSearchSlot_ = 0;
  int cardSearchNext_ = 0;
  input::MemoryCard *cardForPath(const std::string &path, std::string &name);
  int32_t cardOpen(const std::string &path, uint32_t mode);
  int32_t cardTransfer(int32_t fd, uint32_t addr, int32_t len, bool write);
  uint32_t cardFindNext(uint32_t direntry);

  // Pad buffer state (InitPAD / StartPAD)
  uint32_t padBuf1Addr_ = 0;
  uint32_t padBuf1Size_ = 0;
  uint32_t padBuf2Addr_ = 0;
  uint32_t padBuf2Size_ = 0;
  bool padActive_ = false;

  // BSS mirror addresses for legacy MIPS polling (set via setBssMirrors).
  // 0 = disabled (HLE-only games leave them unset).
  uint32_t cdSyncMirror_ = 0;
  uint32_t cdReadyMirror_ = 0;

  // Game thread identity for inline-drain optimisation in queueCdromEvent.
  // Default-constructed id never matches any real thread, so before
  // setGameThreadId is called every push goes through the queue.
  std::thread::id gameThreadId_{};

  // CDROM interrupt pending -- set on the game thread by triggerCdromEvent
  // (which now runs only on the game thread post-3.3) and consumed by the
  // pump loop in drainPendingCallbacks (also game thread).  Carries the
  // INT type (1-5) or 0 if nothing pending.  Cross-thread IRQ delivery
  // goes through `cdEventQueue_` below; this atomic is the single-INT
  // hand-off slot between the queue drain and the pump loop dispatch.
  std::atomic<uint8_t> cdIntPending_{0};

  // Cross-thread CDROM IRQ queue (Phase 3.3).  Pushed from any thread
  // by `queueCdromEvent`; popped only by `drainCdromEventQueue` on the
  // game thread.  Mutex is held only for the short push/swap, so it
  // does not contend with normal game-thread work.
  std::mutex cdEventQueueMtx_;
  std::queue<uint8_t> cdEventQueue_;

  // Lock-free mirror of `cdEventQueue_.size()`, written only under
  // `cdEventQueueMtx_`.  Lets the drain gate skip the lock when idle.
  std::atomic<std::size_t> cdEventQueueDepth_{0};

  // Drain gate + accounting.  `vsyncPtr_` caches
  // `&psyq_state().vsyncCounter` so the gate does not pay for the
  // singleton's initialisation guard; it is null until the first slow-path
  // call, which forces the first drain through the slow path.
  const bool drainGate_ = drainGateEnabled();
  std::atomic<uint32_t> *vsyncPtr_ = nullptr;
  static bool drainGateEnabled();

  const bool spGuard_ = spGuardEnabled();
  static bool spGuardEnabled();
  // Out-of-line so the host backtrace has a real frame to walk from; the
  // first few hits print where the recompiled code was when $sp went bad.
  [[gnu::noinline]] void reportBadStackPointer(uint32_t sp);
  unsigned badSpReports_ = 0;

  // Counters are game-thread-only writes; the reporting thread reads them
  // without synchronisation, which is why they are plain integers read
  // through a value-copy snapshot rather than atomics (a torn read costs a
  // wrong diagnostic line, never behaviour).
  uint64_t drainCalls_ = 0;
  uint64_t drainSlow_ = 0;
  uint64_t drainNested_ = 0;
  uint64_t drainDispatched_ = 0;
  uint64_t drainDeferredNoStack_ = 0;

  std::function<void()> vblankPump_;
  // Yield points are hit tens of millions of times a second, so the pump is
  // rate-limited by a plain counter rather than a clock read.
  uint32_t pumpCounter_ = 0;

  // Deferred CD exception -- set by triggerCdromEvent when a B0:0x19 handler
  // is registered.  Consumed by drainPendingCallbacks, which calls
  // triggerCustomException() at a safe point (after the game enters its
  // polling loop).
  std::atomic<uint8_t> cdExceptionPending_{0};

  // Deferred VBlank exception -- set by triggerVBlankEvent when a B0:0x19
  // handler is registered.  Fired from drainPendingCallbacks via
  // triggerCustomException() to avoid cross-thread register clobbering.
  std::atomic<uint8_t> vblankExceptionPending_{0};

  // Last psyq_state().vsyncCounter value at which the libetc timer
  // InterruptCallback handlers were ticked from drainPendingCallbacks.
  // Game-thread only.
  uint32_t lastIntrTickFrame_ = 0;

  // Internal generic state (game-agnostic)
  //
  // VBlank state migrated to `psyq_state().vsyncCounter` in Phase 2.2.
  // CD sync/ready state migrated to `psyq_state().cdSyncByte`/`cdReadyByte`
  // in Phase 2.3 -- atomic uint8_t polled cooperatively by libcd HLE.
  // CD callbacks / sector bookkeeping + GPU swap callback migrated to
  // `psyq_state()` in Phase 2.4 -- `[psyq_addresses]` removed from TOMLs.

  // Global custom exception handler (SetCustomExitFromException, B0:0x19).
  // The lambda is installed on the game thread and invoked on the game thread
  // via `triggerCustomException()`.  `customExceptionRegistered_` is the
  // cross-thread gate read by `triggerVBlankEvent` (VBlank thread) to decide
  // whether to flag a deferred dispatch -- std::function itself is not
  // safe to read concurrently with assignment.
  std::function<void()> customExceptionCallback_;
  std::atomic<bool> customExceptionRegistered_{false};

  // Specific Table handlers mapping
  void handleA0(uint32_t index);
  void handleB0(uint32_t index);
  void handleC0(uint32_t index);

  // Common memory card/IO stubs (A0)
  void stub_printf();
};

// Restore CPU state from a PsyQ jmp_buf at `buf` (a kernel-mode address holding
// RA, SP, FP, S0..S7, GP -- total 48 bytes).  Mirrors what the PSX BIOS does
// when `SetCustomExitFromException` fires: RA -> PC, GPRs reloaded, COP0
// Cause = 0x400, Status Register exception stack popped, V0 = 1.
//
// Extracted as a free function so it can be tested without spinning up Bios
// (no event system, no CDROM, no heap).
void hle_longjmp_emulator(recomp_context &ctx, Memory &mem, uint32_t buf);

} // namespace ps1::bios
