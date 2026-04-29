#pragma once

#include "runtime/bios/event_system.h"
#include "runtime/bios/file_io.h"
#include "runtime/bios/heap.h"
#include "runtime/cdrom/virtual_fs.h"
#include "runtime/cpu_context.h"
#include "runtime/memory.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>

namespace ps1::gpu {
class GPU;
}
namespace ps1::input {
class InputController;
}
namespace ps1::cdrom {
class CdromController;
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

  // Read-only accessor used by libcd HLE (psyq_libcd.cpp) to drive the
  // backend controller directly. Returns nullptr when not attached (typical
  // for unit tests that don't exercise the disc subsystem).
  cdrom::CdromController *cdromController() const { return cdrom_; }

  // Same idea for the libetc Pad HLE (psyq_pad.cpp). Returns nullptr in tests
  // or headless runs that don't wire SDL input.
  input::InputController *inputController() const { return input_; }

  // ── Per-game PsyQ BSS address configuration ──
  // These addresses vary by game (PsyQ version / link layout).
  // All default to 0 — MUST be configured per-game via env vars or TOML.
  // When an address is 0, the corresponding HLE logic is skipped.
  struct PsyqAddresses {
    uint32_t vblankCounter = 0; // rcnt[3] VBlank counter
    uint32_t cdSyncByte = 0;    // CD command completion flag
    uint32_t cdReadyByte = 0;   // CD data readiness flag
    uint32_t cdRemaining = 0;   // Sectors remaining to read
    uint32_t cdDestPtr = 0;     // Destination pointer for CD data
    uint32_t cdWordCount = 0;   // Words per sector to copy
    uint32_t cdDataCb = 0;      // CD data callback function pointer
    uint32_t cdNotifyCb = 0;    // CD notify callback function pointer
    uint32_t cdStatusHw = 0;    // CD status halfword (gate for PsyQ polling)
    uint32_t gpuSwapCb = 0; // PsyQ display swap callback (SysEnqIntRP chain)
    // ── DrawSync HLE ──
    // PsyQ DrawSync reads base_ptr from a BSS var, then polls:
    //   status_addr = base_ptr + (index << 5)  [stride=0x20, status at offset 0]
    // Since our GPU is fully synchronous, we mark all entries as complete.
    uint32_t gpuDrawSyncBase = 0;      // BSS addr holding POINTER to OT status array
    uint32_t gpuDrawSyncCount = 2;     // Number of OT entries (usually 2, double-buffered)
    uint32_t gpuDrawSyncIndexAddr = 0; // BSS addr holding current OT index (optional precision)
  };

  void setPsyqAddresses(const PsyqAddresses &addrs) {
    psyq_ = addrs;
    setupCdSyncWatchpoint();
  }
  const PsyqAddresses &psyqAddresses() const { return psyq_; }

  // The single entry-point when PC jumps to A0, B0 or C0.
  // Handles reading the function index and triggering the right stub.
  void executeA0();
  void executeB0();
  void executeC0();

  // Called every VBlank to update pad buffers (InitPAD/StartPAD)
  void updatePadBuffers();

  // Trigger CDROM events in the event system based on interrupt type.
  // Called from the main loop when CDROM controller has a pending interrupt.
  void triggerCdromEvent(uint8_t cdIntType);

  // Trigger VBlank event in the event system.
  void triggerVBlankEvent();

  // ── Generic internal-state accessors (game-agnostic) ──
  //
  // These let psyq_hle functions (VSync, CdSync) block on the runtime's
  // internal counter/state instead of polling per-game BSS addresses.
  //
  // waitVSync: blocks until `frames` more VBlanks have been delivered.
  //   Returns the new internal VBlank count.
  uint32_t waitVSync(uint32_t frames = 1);

  // waitForCdSync: blocks until a CD command completes (INT2/INT3) or
  //   a disk error (INT5) occurs.  Returns the sync state byte (2 or 5).
  //   Times out after `timeoutMs` ms and returns 0.
  uint8_t waitForCdSync(int timeoutMs = 5000);

  // waitForCdReady: blocks until CD data is ready (INT1/INT4) or error.
  //   Returns the ready state byte (1, 4, or 5).  Times out → 0.
  uint8_t waitForCdReady(int timeoutMs = 5000);

  // Current internal VBlank counter (monotonically increasing).
  uint32_t vblankCount() const {
    return vblankInternal_.load(std::memory_order_acquire);
  }

  // Drain pending event callbacks — called from game thread at yield points
  // (testEvent, waitEvent, VSync, etc.) to safely dispatch mode-0x1000
  // handlers.
  void drainPendingCallbacks();

private:
  recomp_context &ctx_;
  Heap heap_;
  EventSystem eventSystem_;
  FileIO fileIo_;

  // Hardware pointers (optional, set via setters)
  gpu::GPU *gpu_ = nullptr;
  input::InputController *input_ = nullptr;
  cdrom::CdromController *cdrom_ = nullptr;

  // Pad buffer state (InitPAD / StartPAD)
  uint32_t padBuf1Addr_ = 0;
  uint32_t padBuf1Size_ = 0;
  uint32_t padBuf2Addr_ = 0;
  uint32_t padBuf2Size_ = 0;
  bool padActive_ = false;

  // CDROM interrupt pending — set from main thread, consumed on game thread.
  // Simulates the SysEnqIntRP interrupt handler chain that never runs in our
  // recompiled environment.  Stores the INT type (1-5) or 0 if nothing pending.
  std::atomic<uint8_t> cdIntPending_{0};

  // Deferred CD exception — set by triggerCdromEvent when customExceptionExit_
  // is active.  Consumed by drainPendingCallbacks to fire the longjmp at a
  // safe point (after the game enters its polling loop).
  std::atomic<uint8_t> cdExceptionPending_{0};

  // Deferred VBlank exception — set by triggerVBlankEvent when
  // customExceptionExit_ is active.  Fired from drainPendingCallbacks
  // to avoid cross-thread register clobbering.
  std::atomic<uint8_t> vblankExceptionPending_{0};

  // ── Internal generic state (game-agnostic) ─────────────────────────────
  //
  // These replace the need for per-game BSS address configuration.
  // triggerCdromEvent() and triggerVBlankEvent() always update them so that
  // HLE functions (waitForCdSync, waitVSync) can block without knowing
  // game-specific memory addresses.

  // CD sync state: mirrors the value the PsyQ interrupt handler would write
  // to cdSyncByte.  0 = pending, 2 = complete (INT2/INT3), 5 = error (INT5).
  std::atomic<uint8_t> cdSyncInternal_{0};
  // CD ready state: mirrors the value written to cdReadyByte.
  // 0 = pending, 1 = data ready (INT1), 4 = data end (INT4), 5 = error.
  std::atomic<uint8_t> cdReadyInternal_{0};
  std::mutex cdInternalMtx_;
  std::condition_variable cdInternalCv_;

  // VBlank counter — incremented by triggerVBlankEvent() every frame.
  std::atomic<uint32_t> vblankInternal_{0};
  std::mutex vblankMtx_;
  std::condition_variable vblankCv_;

  // Per-game PsyQ BSS addresses
  PsyqAddresses psyq_;

  // Global custom exception handler (SetCustomExitFromException)
  uint32_t customExceptionExit_ = 0;

  // Specific Table handlers mapping
  void handleA0(uint32_t index);
  void handleB0(uint32_t index);
  void handleC0(uint32_t index);

  // Common memory card/IO stubs (A0)
  void stub_printf();

  // Set up the cdSyncByte write watchpoint so INT2 fires on the game thread
  // immediately after the game clears cdSyncByte (ACKs INT3).
  void setupCdSyncWatchpoint();
};

} // namespace ps1::bios
