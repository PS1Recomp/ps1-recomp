// ps1xRuntime — PS1 Hardware Simulation Runtime
// Executes recompiled C++ code with GPU, GTE, SPU, CD-ROM simulation

#include "runtime/ps1_runtime_macros.h"
#include <cstring>
#include <exception>
#include <fmt/format.h>
#include <iostream>
#include <ps1recomp/elf_parser.h>
#include <runtime/bios/bios.h>
#include <runtime/cdrom/virtual_fs.h>
#include <runtime/cpu_context.h>
#include <runtime/gpu/gpu.h>
#include <runtime/gpu/renderer_opengl.h>
#include <runtime/memory.h>
#include <string>
#include <vector>

extern void __start(uint8_t *rdram, recomp_context *ctx);

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: ps1xRuntime <ps1_elf_file>\n";
    return 1;
  }

  std::string elf_path = argv[1];

  std::cout << "ps1xRuntime — PS1 Hardware Simulation\n";

  // Initialize Memory and CPU Context
  ps1::Memory memory;
  ps1::gpu::GPU gpu;
  memory.setGPU(&gpu);

  recomp_context ctx;
  ctx.reset();
  ctx.mem = &memory;

  // Initialize renderer
  ps1::gpu::RendererOpenGL renderer(gpu);
  if (!renderer.init("ps1xRecomp - Phase 8 GPU")) {
    fmt::print(stderr, "Failed to initialize OpenGL renderer!\n");
    return 1;
  }

  ps1::cdrom::VirtualFs fs;
  ps1::bios::Bios bios(ctx, fs, memory);
  ctx.bios = &bios;

  // Load the ELF
  ps1recomp::ElfParser parser;
  if (!parser.load(elf_path)) {
    std::cerr << "Failed to load ELF: " << parser.getError() << "\n";
    return 1;
  }

  // Hand-rolled ELF loader to Memory
  auto sections = parser.getSections();
  int loaded_sections = 0;
  for (const auto &sec : sections) {
    if ((sec.type == ps1recomp::SectionType::Text ||
         sec.type == ps1recomp::SectionType::Data) &&
        sec.data != nullptr) {
      uint32_t phys = ps1::Memory::toPhysical(sec.vaddr);
      if (phys < ps1::Memory::RAM_SIZE) {
        std::memcpy(memory.ramPtr() + phys, sec.data, sec.size);
        loaded_sections++;
      }
    }
  }

  std::cout << "Loaded " << loaded_sections << " sections into PS1 memory.\n";

  // Boot the statically recompiled executable
  std::cout << "Booting statically recompiled executable at entry point "
               "0x80010000...\n";

  // Note: in a real emulator, we'd loop over PC or setup the event loop.
  // For the direct static recompiler, we just call the recompiled run method.
  // We only call it once as it implements a while(1) internally usually,
  // or we'd step through the scheduler.
  // The Spinning Cube is a while(1) loop without a scheduler, so this call will
  // block. In later phases we'd integrate it with the Thread/Scheduler system.

  while (true) {
    try {
      __start(memory.ramPtr(), &ctx);
      break; // Normal exit
    } catch (const ps1::CpuException &e) {
      if (e.cause == ps1::ExceptionCause::Syscall) {
        // Exception vector for syscall in real PS1 goes to 0x80000080
        // Our HLE BIOS router handles it differently, standard MIPS convention
        // doesn't directly map syscall to A0/B0/C0 routines usually (they use
        // JAL), but some games might trigger SYSCALL for specific kernel
        // functions.
        std::cerr << "SYSCALL exception caught!\n";
        // Handle SYSCALL, increment PC to not get stuck
        ctx.pc += 4;
      } else if (e.cause == ps1::ExceptionCause::Bp) {
        std::cerr << "BREAK exception caught!\n";
        // Outras exceções encerram a execução por agora
      } else {
        fmt::print("Unhandled CPU Exception {}, stopping.\n",
                   static_cast<uint32_t>(e.cause));
        break;
      }
    }

    // Render loop and events (simple temporary hook)
    if (!renderer.processEvents()) {
      break; // User closed window
    }
    renderer.renderFrame();
  }

  renderer.destroy();
  fmt::print("Simulation ended.\n");

  return 0;
}
