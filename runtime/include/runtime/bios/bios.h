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
#include <memory>

namespace ps1::gpu { class GPU; }
namespace ps1::input { class InputController; }
namespace ps1::cdrom { class CdromController; }

namespace ps1::bios {

class Bios {
public:
  Bios(recomp_context &ctx, cdrom::VirtualFs &fs, Memory &mem);
  ~Bios();

  // Hardware attachment (call after construction)
  void setGPU(gpu::GPU *gpu) { gpu_ = gpu; }
  void setInputController(input::InputController *input) { input_ = input; }
  void setCdromController(cdrom::CdromController *cdrom) { cdrom_ = cdrom; }

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

  // Drain pending event callbacks — called from game thread at yield points
  // (testEvent, waitEvent, VSync, etc.) to safely dispatch mode-0x1000 handlers.
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

  // Specific Table handlers mapping
  void handleA0(uint32_t index);
  void handleB0(uint32_t index);
  void handleC0(uint32_t index);

  // Common memory card/IO stubs (A0)
  void stub_printf();
};

} // namespace ps1::bios
