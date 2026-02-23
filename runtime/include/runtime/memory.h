#pragma once

// ps1xRuntime — PS1 Memory Subsystem
// 2MB Main RAM + 1KB Scratchpad + BIOS ROM + I/O port routing

#include "runtime/gpu/gpu.h"
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace ps1 {

// ─── PS1 Memory Map ────────────────────────────────────
//
// 0x00000000 - 0x001FFFFF  Main RAM (2MB) — KUSEG
// 0x1F800000 - 0x1F8003FF  Scratchpad (1KB data cache)
// 0x1F801000 - 0x1F802FFF  I/O Ports (hardware registers)
// 0x1FC00000 - 0x1FC7FFFF  BIOS ROM (512KB)
// 0x80000000 - 0x801FFFFF  Main RAM mirror — KSEG0 (cached)
// 0x A0000000 - 0xA01FFFFF  Main RAM mirror — KSEG1 (uncached)
//
// ────────────────────────────────────────────────────────

class Memory {
public:
  static constexpr uint32_t RAM_SIZE = 2 * 1024 * 1024; // 2MB
  static constexpr uint32_t SCRATCHPAD_SIZE = 1024;     // 1KB
  static constexpr uint32_t BIOS_SIZE = 512 * 1024;     // 512KB

  // Memory region base addresses (physical)
  static constexpr uint32_t RAM_BASE = 0x00000000;
  static constexpr uint32_t SCRATCHPAD_BASE = 0x1F800000;
  static constexpr uint32_t IO_BASE = 0x1F801000;
  static constexpr uint32_t BIOS_BASE = 0x1FC00000;

  Memory() { reset(); }

  void reset() {
    std::memset(ram_, 0, sizeof(ram_));
    std::memset(scratchpad_, 0, sizeof(scratchpad_));
    std::memset(bios_, 0xFF, sizeof(bios_)); // BIOS starts as 0xFF (ROM)
  }

  // ─── Read ──────────────────────────────────────────

  uint8_t read8(uint32_t addr) const {
    uint32_t phys = toPhysical(addr);

    if (phys < RAM_SIZE) {
      return ram_[phys];
    }
    if (phys >= SCRATCHPAD_BASE && phys < SCRATCHPAD_BASE + SCRATCHPAD_SIZE) {
      return scratchpad_[phys - SCRATCHPAD_BASE];
    }
    if (phys >= BIOS_BASE && phys < BIOS_BASE + BIOS_SIZE) {
      return bios_[phys - BIOS_BASE];
    }
    // I/O ports or unmapped — return 0 for now
    return 0;
  }

  uint16_t read16(uint32_t addr) const {
    uint8_t lo = read8(addr);
    uint8_t hi = read8(addr + 1);
    return static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8);
  }

  uint32_t read32(uint32_t addr) const {
    uint32_t phys = toPhysical(addr);
    if (phys == 0x1F801810) {
      return gpu_ ? gpu_->readGPUREAD() : 0;
    }
    if (phys == 0x1F801814) {
      return gpu_ ? gpu_->readGPUSTAT() : 0;
    }

    uint8_t b0 = read8(addr);
    uint8_t b1 = read8(addr + 1);
    uint8_t b2 = read8(addr + 2);
    uint8_t b3 = read8(addr + 3);
    return static_cast<uint32_t>(b0) | (static_cast<uint32_t>(b1) << 8) |
           (static_cast<uint32_t>(b2) << 16) |
           (static_cast<uint32_t>(b3) << 24);
  }

  // ─── Write ─────────────────────────────────────────

  void write8(uint32_t addr, uint8_t val) {
    uint32_t phys = toPhysical(addr);

    if (phys < RAM_SIZE) {
      ram_[phys] = val;
      return;
    }
    if (phys >= SCRATCHPAD_BASE && phys < SCRATCHPAD_BASE + SCRATCHPAD_SIZE) {
      scratchpad_[phys - SCRATCHPAD_BASE] = val;
      return;
    }
    // BIOS is ROM — writes are ignored
    // I/O ports — ignored for now (will route to HW later)
  }

  void write16(uint32_t addr, uint16_t val) {
    write8(addr, static_cast<uint8_t>(val & 0xFF));
    write8(addr + 1, static_cast<uint8_t>((val >> 8) & 0xFF));
  }

  void write32(uint32_t addr, uint32_t val) {
    uint32_t phys = toPhysical(addr);
    if (phys == 0x1F801810) {
      if (gpu_)
        gpu_->writeGP0(val);
      return;
    }
    if (phys == 0x1F801814) {
      if (gpu_)
        gpu_->writeGP1(val);
      return;
    }

    write8(addr, static_cast<uint8_t>(val & 0xFF));
    write8(addr + 1, static_cast<uint8_t>((val >> 8) & 0xFF));
    write8(addr + 2, static_cast<uint8_t>((val >> 16) & 0xFF));
    write8(addr + 3, static_cast<uint8_t>((val >> 24) & 0xFF));
  }

  // ─── Direct access for loading binaries ────────────

  uint8_t *ramPtr() { return ram_; }
  const uint8_t *ramPtr() const { return ram_; }

  uint8_t *biosPtr() { return bios_; }
  const uint8_t *biosPtr() const { return bios_; }

  uint8_t *scratchpadPtr() { return scratchpad_; }
  const uint8_t *scratchpadPtr() const { return scratchpad_; }

private:
  uint8_t ram_[RAM_SIZE];
  uint8_t scratchpad_[SCRATCHPAD_SIZE];
  uint8_t bios_[BIOS_SIZE];

  ps1::gpu::GPU *gpu_ = nullptr;

public:
  void setGPU(ps1::gpu::GPU *gpu) { gpu_ = gpu; }

public:
  /// Convert virtual address to physical address
  /// PS1 uses 3 mirrors: KUSEG (0x0), KSEG0 (0x80), KSEG1 (0xA0)
  static uint32_t toPhysical(uint32_t addr) {
    // Strip KSEG0/KSEG1 bits
    // KSEG0: 0x80000000-0x9FFFFFFF → mask off bit 31
    // KSEG1: 0xA0000000-0xBFFFFFFF → mask off bits 31,29
    if (addr >= 0xA0000000) {
      return addr & 0x1FFFFFFF;
    }
    if (addr >= 0x80000000) {
      return addr & 0x1FFFFFFF;
    }
    return addr; // KUSEG — already physical
  }
};

} // namespace ps1
