#pragma once

// ps1xRuntime — Runtime Macros
// These macros are emitted by InstructionEmitter and GteEmitter.
// They bridge recompiled C++ code → runtime Memory/GTE subsystems.

#include <cstdint>
#include <runtime/cpu_context.h>
#include <runtime/memory.h>

// ─── Memory Access Macros ──────────────────────────────
// Used by InstructionEmitter for LB/LH/LW/SB/SH/SW

#define MEM_READ8(ctx, addr) (ctx)->mem->read8(addr)
#define MEM_READ16(ctx, addr) (ctx)->mem->read16(addr)
#define MEM_READ32(ctx, addr) (ctx)->mem->read32(addr)

#define MEM_WRITE8(ctx, addr, val) (ctx)->mem->write8(addr, val)
#define MEM_WRITE16(ctx, addr, val) (ctx)->mem->write16(addr, val)
#define MEM_WRITE32(ctx, addr, val) (ctx)->mem->write32(addr, val)

// ─── Unaligned Load/Store ──────────────────────────────
// Used for LWL/LWR/SWL/SWR instructions

inline uint32_t DO_LWL(ps1::Memory *mem, uint32_t rt, uint32_t addr) {
  uint32_t aligned = addr & ~3u;
  uint32_t word = mem->read32(aligned);
  uint32_t shift = (addr & 3u);

  switch (shift) {
  case 0:
    return (rt & 0x00FFFFFF) | (word << 24);
  case 1:
    return (rt & 0x0000FFFF) | (word << 16);
  case 2:
    return (rt & 0x000000FF) | (word << 8);
  case 3:
    return word;
  default:
    return rt;
  }
}

inline uint32_t DO_LWR(ps1::Memory *mem, uint32_t rt, uint32_t addr) {
  uint32_t aligned = addr & ~3u;
  uint32_t word = mem->read32(aligned);
  uint32_t shift = (addr & 3u);

  switch (shift) {
  case 0:
    return word;
  case 1:
    return (rt & 0xFF000000) | (word >> 8);
  case 2:
    return (rt & 0xFFFF0000) | (word >> 16);
  case 3:
    return (rt & 0xFFFFFF00) | (word >> 24);
  default:
    return rt;
  }
}

inline void DO_SWL(ps1::Memory *mem, uint32_t rt, uint32_t addr) {
  uint32_t aligned = addr & ~3u;
  uint32_t word = mem->read32(aligned);
  uint32_t shift = (addr & 3u);

  switch (shift) {
  case 0:
    word = (word & 0xFFFFFF00) | (rt >> 24);
    break;
  case 1:
    word = (word & 0xFFFF0000) | (rt >> 16);
    break;
  case 2:
    word = (word & 0xFF000000) | (rt >> 8);
    break;
  case 3:
    word = rt;
    break;
  }
  mem->write32(aligned, word);
}

inline void DO_SWR(ps1::Memory *mem, uint32_t rt, uint32_t addr) {
  uint32_t aligned = addr & ~3u;
  uint32_t word = mem->read32(aligned);
  uint32_t shift = (addr & 3u);

  switch (shift) {
  case 0:
    word = rt;
    break;
  case 1:
    word = (word & 0x000000FF) | (rt << 8);
    break;
  case 2:
    word = (word & 0x0000FFFF) | (rt << 16);
    break;
  case 3:
    word = (word & 0x00FFFFFF) | (rt << 24);
    break;
  }
  mem->write32(aligned, word);
}

// ─── GTE Register Access ──────────────────────────────
// Used by GteEmitter for MFC2/MTC2/CFC2/CTC2

inline uint32_t gte_read_data(ps1::CPUContext *ctx, uint8_t reg) {
  return ctx->cop2d[reg & 0x1F];
}

inline void gte_write_data(ps1::CPUContext *ctx, uint8_t reg, uint32_t val) {
  ctx->cop2d[reg & 0x1F] = val;
}

inline uint32_t gte_read_control(ps1::CPUContext *ctx, uint8_t reg) {
  return ctx->cop2c[reg & 0x1F];
}

inline void gte_write_control(ps1::CPUContext *ctx, uint8_t reg, uint32_t val) {
  ctx->cop2c[reg & 0x1F] = val;
}
