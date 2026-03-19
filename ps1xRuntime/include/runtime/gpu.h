#pragma once

// ps1xRuntime — GPU Stub
// PS1 GPU (VRAM 1MB, GP0/GP1 command FIFO, rendering primitives)
// This is a stub for future incremental implementation.

#include <cstdint>
#include <cstring>

namespace ps1 {

class GPU {
public:
  static constexpr uint32_t VRAM_WIDTH = 1024;
  static constexpr uint32_t VRAM_HEIGHT = 512;
  static constexpr uint32_t VRAM_SIZE =
      VRAM_WIDTH * VRAM_HEIGHT * 2; // 16-bit pixels

  GPU() { reset(); }

  void reset() {
    std::memset(vram_, 0, sizeof(vram_));
    gpustat_ = 0x14802000; // Default GPUSTAT
  }

  // GP0 (Rendering/VRAM access) — stub
  void writeGP0(uint32_t cmd) { (void)cmd; /* TODO */ }

  // GP1 (Display control) — stub
  void writeGP1(uint32_t cmd) { (void)cmd; /* TODO */ }

  // GPUSTAT register
  uint32_t readGPUSTAT() const { return gpustat_; }

  // VRAM access
  uint16_t *vramPtr() { return vram_; }
  const uint16_t *vramPtr() const { return vram_; }

private:
  uint16_t vram_[VRAM_WIDTH * VRAM_HEIGHT];
  uint32_t gpustat_;
};

} // namespace ps1
