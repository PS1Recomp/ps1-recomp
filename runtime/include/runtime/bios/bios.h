#pragma once

#include "runtime/bios/event_system.h"
#include "runtime/bios/file_io.h"
#include "runtime/bios/heap.h"
#include "runtime/cdrom/virtual_fs.h"
#include "runtime/cpu_context.h"
#include "runtime/memory.h"
#include <cstdint>
#include <memory>

namespace ps1::bios {

class Bios {
public:
  Bios(recomp_context &ctx, cdrom::VirtualFs &fs, Memory &mem);
  ~Bios();

  // The single entry-point when PC jumps to A0, B0 or C0.
  // Handles reading the function index and triggering the right stub.
  void executeA0();
  void executeB0();
  void executeC0();

private:
  recomp_context &ctx_;
  Heap heap_;
  EventSystem eventSystem_;
  FileIO fileIo_;

  // Specific Table handlers mapping
  void handleA0(uint32_t index);
  void handleB0(uint32_t index);
  void handleC0(uint32_t index);

  // Common memory card/IO stubs (A0)
  void stub_printf();
};

} // namespace ps1::bios
