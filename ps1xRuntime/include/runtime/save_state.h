#pragma once

// ps1xRuntime — Save State System
// Serializes/deserializes complete PS1 hardware state to/from file

#include "runtime/cdrom/cdrom_controller.h"
#include "runtime/cpu_context.h"
#include "runtime/dma/dma.h"
#include "runtime/gpu/gpu.h"
#include "runtime/input/input.h"
#include "runtime/mdec/mdec.h"
#include "runtime/memory.h"
#include "runtime/spu/spu.h"
#include "runtime/timers/timers.h"
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace ps1 {

// Save state file format:
//   [Header: 16 bytes]
//   [CPU Context: ~300 bytes]
//   [RAM: 2MB]
//   [Scratchpad: 1KB]
//   [GPU VRAM: 1MB]
//   [GPU State: ~64 bytes]
//   [SPU Sound RAM: 512KB]
//   [SPU Registers: ~2KB]
//   [CDROM State: ~128 bytes]
//   [DMA State: ~256 bytes]
//   [Timers State: ~64 bytes]
//   [IRQ State: ~8 bytes]
//   [Input State: ~256KB (memory cards)]

struct SaveStateHeader {
  char magic[4] = {'P', 'S', '1', 'S'}; // "PS1S"
  uint32_t version = 1;
  uint32_t flags = 0;
  uint32_t checksum = 0; // Simple XOR checksum of all data
};

class SaveState {
public:
  // Save entire system state to file
  static bool save(const std::string &path, const recomp_context &ctx,
                   const Memory &mem, const gpu::GPU &gpu, const spu::SPU &spu,
                   const DMA &dma, const cdrom::CdromController &cdrom,
                   const Timers &timers, const InterruptController &irq,
                   const input::InputController &input);

  // Load entire system state from file
  static bool load(const std::string &path, recomp_context &ctx, Memory &mem,
                   gpu::GPU &gpu, spu::SPU &spu, DMA &dma,
                   cdrom::CdromController &cdrom, Timers &timers,
                   InterruptController &irq, input::InputController &input);

  // Check if file is a valid save state
  static bool isValid(const std::string &path);

private:
  // Helpers for binary I/O
  static void writeBytes(std::ofstream &f, const void *data, size_t size);
  static bool readBytes(std::ifstream &f, void *data, size_t size);
  static uint32_t computeChecksum(const void *data, size_t size);
};

} // namespace ps1
