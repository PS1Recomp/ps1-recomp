#pragma once

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
#include <queue>
#include <thread>

namespace ps1::gpu {
class GPU;
}
namespace ps1::input {
class InputController;
}
namespace ps1::cdrom {
class CdromController;
}
namespace ps1 {
class DMA;
}

namespace ps1::bios {

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
  // to call on any iteration of a polling loop — events are queued
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
  // since Phase 2.3 — `hle_libcd_CdSync`/`CdReady` poll those atomics
  // cooperatively without needing per-game BSS addresses.

  // Drain pending event callbacks — called from game thread at yield points
  // (testEvent, waitEvent, VSync, etc.) to safely dispatch mode-0x1000
  // handlers.
  void drainPendingCallbacks();

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
  // the queue's async semantics — they're drained from drainPendingCallbacks
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

  // CDROM interrupt pending — set on the game thread by triggerCdromEvent
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

  // Deferred CD exception — set by triggerCdromEvent when a B0:0x19 handler
  // is registered.  Consumed by drainPendingCallbacks, which calls
  // triggerCustomException() at a safe point (after the game enters its
  // polling loop).
  std::atomic<uint8_t> cdExceptionPending_{0};

  // Deferred VBlank exception — set by triggerVBlankEvent when a B0:0x19
  // handler is registered.  Fired from drainPendingCallbacks via
  // triggerCustomException() to avoid cross-thread register clobbering.
  std::atomic<uint8_t> vblankExceptionPending_{0};

  // Internal generic state (game-agnostic)
  //
  // VBlank state migrated to `psyq_state().vsyncCounter` in Phase 2.2.
  // CD sync/ready state migrated to `psyq_state().cdSyncByte`/`cdReadyByte`
  // in Phase 2.3 — atomic uint8_t polled cooperatively by libcd HLE.
  // CD callbacks / sector bookkeeping + GPU swap callback migrated to
  // `psyq_state()` in Phase 2.4 — `[psyq_addresses]` removed from TOMLs.

  // Global custom exception handler (SetCustomExitFromException, B0:0x19).
  // The lambda is installed on the game thread and invoked on the game thread
  // via `triggerCustomException()`.  `customExceptionRegistered_` is the
  // cross-thread gate read by `triggerVBlankEvent` (VBlank thread) to decide
  // whether to flag a deferred dispatch — std::function itself is not
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
// RA, SP, FP, S0..S7, GP — total 48 bytes).  Mirrors what the PSX BIOS does
// when `SetCustomExitFromException` fires: RA → PC, GPRs reloaded, COP0
// Cause = 0x400, Status Register exception stack popped, V0 = 1.
//
// Extracted as a free function so it can be tested without spinning up Bios
// (no event system, no CDROM, no heap).
void hle_longjmp_emulator(recomp_context &ctx, Memory &mem, uint32_t buf);

} // namespace ps1::bios
