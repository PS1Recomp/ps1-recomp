#pragma once

// ps1xRuntime — PS1 Memory Subsystem
// 2MB Main RAM + 1KB Scratchpad + BIOS ROM + I/O port routing to all hardware

#include "runtime/cdrom/cdrom_controller.h"
#include "runtime/dma/dma.h"
#include "runtime/gpu/gpu.h"
#include "runtime/input/input.h"
#include "runtime/mdec/mdec.h"
#include "runtime/spu/spu.h"
#include "runtime/timers/timers.h"
#include <atomic>
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
// 0xA0000000 - 0xA01FFFFF  Main RAM mirror — KSEG1 (uncached)
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
      // Acquire fence: prevents the compiler from hoisting RAM reads out of
      // busy-wait loops.  The main thread writes the VBlank counter while
      // the game thread spins on it in VSync's busy-wait (func_8003E638).
      std::atomic_thread_fence(std::memory_order_acquire);
      return ram_[phys];
    }
    if (phys >= SCRATCHPAD_BASE && phys < SCRATCHPAD_BASE + SCRATCHPAD_SIZE) {
      return scratchpad_[phys - SCRATCHPAD_BASE];
    }
    if (phys >= BIOS_BASE && phys < BIOS_BASE + BIOS_SIZE) {
      return bios_[phys - BIOS_BASE];
    }
    // ─── CD-ROM (byte-addressed reads) ──────────
    if (phys >= 0x1F801800 && phys < 0x1F801804) {
      if (cdrom_)
        return cdrom_->readRegister(phys);
      return 0;
    }
    // I/O ports — return 0 for byte reads not explicitly handled
    return 0;
  }

  uint16_t read16(uint32_t addr) const {
    uint32_t phys = toPhysical(addr);

    // ─── SPU registers (16-bit) ─────────────────
    if (phys >= 0x1F801C00 && phys < 0x1F802000) {
      if (spu_)
        return spu_->readRegister(phys);
      return 0;
    }

    // ─── Timer registers (16-bit) ───────────────
    if (phys >= 0x1F801100 && phys < 0x1F801130) {
      if (timers_)
        return timers_->readRegister(phys);
      return 0;
    }

    // ─── Input SIO registers (16-bit) ───────────
    if (phys >= 0x1F801040 && phys < 0x1F801050) {
      if (input_)
        return input_->readRegister16(phys);
      return 0;
    }

    // ─── Interrupt registers (16-bit) ───────────
    if (phys == 0x1F801070) {
      if (irqCtrl_)
        return irqCtrl_->readIStat() & 0xFFFF;
      return 0;
    }
    if (phys == 0x1F801074) {
      if (irqCtrl_)
        return irqCtrl_->readIMask() & 0xFFFF;
      return 0;
    }

    uint8_t lo = read8(addr);
    uint8_t hi = read8(addr + 1);
    return static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8);
  }

  uint32_t read32(uint32_t addr) const {
    uint32_t phys = toPhysical(addr);

    // ─── RAM fast-path (most common, avoids 4× read8 calls) ──
    if (phys < RAM_SIZE) {
      std::atomic_thread_fence(std::memory_order_acquire);
      uint32_t val;
      std::memcpy(&val, &ram_[phys], sizeof(val));
      return val;  // little-endian, matches PS1
    }

    // ─── GPU (32-bit) ───────────────────────────
    if (phys == 0x1F801810) {
      return gpu_ ? gpu_->readGPUREAD() : 0;
    }
    if (phys == 0x1F801814) {
      return gpu_ ? gpu_->readGPUSTAT() : 0;
    }

    // ─── DMA registers (32-bit) ─────────────────
    if (phys >= 0x1F801080 && phys <= 0x1F8010F4) {
      if (dma_)
        return dma_->readRegister(phys);
      return 0;
    }

    // ─── Interrupt registers (32-bit) ───────────
    if (phys == 0x1F801070) {
      if (irqCtrl_)
        return irqCtrl_->readIStat();
      return 0;
    }
    if (phys == 0x1F801074) {
      if (irqCtrl_)
        return irqCtrl_->readIMask();
      return 0;
    }

    // ─── MDEC (32-bit) ──────────────────────────
    if (phys == 0x1F801820) {
      if (mdec_)
        return mdec_->readData();
      return 0;
    }
    if (phys == 0x1F801824) {
      if (mdec_)
        return mdec_->readStatus();
      return 0;
    }

    // ─── Input SIO (32-bit reads) ───────────────
    if (phys >= 0x1F801040 && phys < 0x1F801050) {
      if (input_)
        return input_->readRegister(phys);
      return 0;
    }

    // ─── CD-ROM (8-bit mapped, return byte in low bits) ──
    if (phys >= 0x1F801800 && phys < 0x1F801804) {
      if (cdrom_)
        return cdrom_->readRegister(phys);
      return 0;
    }

    // ─── SPU (16-bit, compose 32-bit from two 16-bit reads)
    if (phys >= 0x1F801C00 && phys < 0x1F802000) {
      uint16_t lo = spu_ ? spu_->readRegister(phys) : 0;
      uint16_t hi = spu_ ? spu_->readRegister(phys + 2) : 0;
      return lo | (static_cast<uint32_t>(hi) << 16);
    }

    // ─── Memory Control / Expansion ─────────────
    if (phys >= 0x1F801000 && phys < 0x1F801040) {
      return 0; // Memory control registers (stub)
    }
    if (phys == 0x1F801060) {
      return 0x00000B88; // RAM size register
    }
    if (phys == 0xFFFE0130) {
      return 0; // Cache control register
    }

    // Default: compose from byte reads
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

    // ─── CD-ROM (byte-addressed) ────────────────
    if (phys >= 0x1F801800 && phys < 0x1F801804) {
      if (cdrom_)
        cdrom_->writeRegister(phys, val);
      return;
    }

    // BIOS is ROM — writes are ignored
    // Other I/O ports — ignored at byte level
  }

  void write16(uint32_t addr, uint16_t val) {
    uint32_t phys = toPhysical(addr);

    // ─── SPU registers (16-bit) ─────────────────
    if (phys >= 0x1F801C00 && phys < 0x1F802000) {
      if (spu_)
        spu_->writeRegister(phys, val);
      return;
    }

    // ─── Timer registers (16-bit) ───────────────
    if (phys >= 0x1F801100 && phys < 0x1F801130) {
      if (timers_)
        timers_->writeRegister(phys, val);
      return;
    }

    // ─── Input SIO (16-bit) ─────────────────────
    if (phys >= 0x1F801040 && phys < 0x1F801050) {
      if (input_)
        input_->writeRegister16(phys, val);
      return;
    }

    // ─── Interrupt registers (16-bit) ───────────
    if (phys == 0x1F801070) {
      if (irqCtrl_)
        irqCtrl_->writeIStat(val);
      return;
    }
    if (phys == 0x1F801074) {
      if (irqCtrl_)
        irqCtrl_->writeIMask(val);
      return;
    }

    write8(addr, static_cast<uint8_t>(val & 0xFF));
    write8(addr + 1, static_cast<uint8_t>((val >> 8) & 0xFF));
  }

  void write32(uint32_t addr, uint32_t val) {
    uint32_t phys = toPhysical(addr);

    // ─── RAM fast-path ──────────────────────────
    if (phys < RAM_SIZE) {
      std::memcpy(&ram_[phys], &val, sizeof(val));
      std::atomic_thread_fence(std::memory_order_release);
      return;
    }

    // ─── GPU (32-bit) ───────────────────────────
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

    // ─── DMA registers (32-bit) ─────────────────
    if (phys >= 0x1F801080 && phys <= 0x1F8010F4) {
      if (dma_)
        dma_->writeRegister(phys, val);
      return;
    }

    // ─── Interrupt registers (32-bit) ───────────
    if (phys == 0x1F801070) {
      if (irqCtrl_)
        irqCtrl_->writeIStat(val);
      return;
    }
    if (phys == 0x1F801074) {
      if (irqCtrl_)
        irqCtrl_->writeIMask(val);
      return;
    }

    // ─── MDEC (32-bit) ──────────────────────────
    if (phys == 0x1F801820) {
      if (mdec_)
        mdec_->writeCommand(val);
      return;
    }
    if (phys == 0x1F801824) {
      if (mdec_)
        mdec_->writeControl(val);
      return;
    }

    // ─── Input SIO (32-bit writes) ──────────────
    if (phys >= 0x1F801040 && phys < 0x1F801050) {
      if (input_)
        input_->writeRegister(phys, val);
      return;
    }

    // ─── CD-ROM (byte writes via 32-bit) ────────
    if (phys >= 0x1F801800 && phys < 0x1F801804) {
      if (cdrom_)
        cdrom_->writeRegister(phys, val & 0xFF);
      return;
    }

    // ─── SPU (32-bit → two 16-bit writes) ───────
    if (phys >= 0x1F801C00 && phys < 0x1F802000) {
      if (spu_) {
        spu_->writeRegister(phys, val & 0xFFFF);
        spu_->writeRegister(phys + 2, (val >> 16) & 0xFFFF);
      }
      return;
    }

    // ─── Memory Control (stub) ──────────────────
    if (phys >= 0x1F801000 && phys < 0x1F801040) {
      return; // Ignore memory control writes
    }
    if (phys == 0x1F801060) {
      return; // RAM size register
    }
    if (phys == 0xFFFE0130) {
      return; // Cache control register
    }

    // Default: decompose into byte writes
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

  // ─── Hardware Connections ──────────────────────────

  void setGPU(ps1::gpu::GPU *gpu) { gpu_ = gpu; }
  ps1::gpu::GPU *getGPU() const { return gpu_; }
  void setSPU(spu::SPU *spu) { spu_ = spu; }
  void setDMA(DMA *dma) { dma_ = dma; }
  void setCDROM(cdrom::CdromController *cdrom) { cdrom_ = cdrom; }
  void setInput(input::InputController *input) { input_ = input; }
  void setMDEC(mdec::MDEC *mdec) { mdec_ = mdec; }
  void setTimers(Timers *timers) { timers_ = timers; }
  void setInterruptController(InterruptController *irq) { irqCtrl_ = irq; }

private:
  uint8_t ram_[RAM_SIZE];
  uint8_t scratchpad_[SCRATCHPAD_SIZE];
  uint8_t bios_[BIOS_SIZE];

  // Hardware device pointers (non-owning)
  ps1::gpu::GPU *gpu_ = nullptr;
  spu::SPU *spu_ = nullptr;
  DMA *dma_ = nullptr;
  cdrom::CdromController *cdrom_ = nullptr;
  input::InputController *input_ = nullptr;
  mdec::MDEC *mdec_ = nullptr;
  Timers *timers_ = nullptr;
  InterruptController *irqCtrl_ = nullptr;

public:
  /// Convert virtual address to physical address
  /// PS1 uses 3 mirrors: KUSEG (0x0), KSEG0 (0x80), KSEG1 (0xA0)
  /// PS1 also mirrors 2MB RAM across the first 8MB of physical address space.
  static uint32_t toPhysical(uint32_t addr) {
    // Strip KSEG0/KSEG1 bits
    // KSEG0: 0x80000000-0x9FFFFFFF → mask off bit 31
    // KSEG1: 0xA0000000-0xBFFFFFFF → mask off bits 31,29
    uint32_t phys;
    if (addr >= 0xA0000000) {
      phys = addr & 0x1FFFFFFF;
    } else if (addr >= 0x80000000) {
      phys = addr & 0x1FFFFFFF;
    } else {
      phys = addr; // KUSEG — already physical
    }

    // PS1 RAM mirroring: 2MB is mirrored across the first 8MB
    // Physical addresses 0x000000-0x7FFFFF all map to the same 2MB.
    // This is critical for games that set SP to 0x807FFF00 (phys 0x7FFF00)
    // which must map to RAM offset 0x1FFF00.
    if (phys < 0x00800000) {
      phys &= (RAM_SIZE - 1);  // phys & 0x1FFFFF
    }

    return phys;
  }
};

} // namespace ps1
