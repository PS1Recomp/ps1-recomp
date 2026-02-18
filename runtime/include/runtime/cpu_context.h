#pragma once

// ps1xRuntime — CPU Context
// PS1 R3000A CPU register state for recompiled code execution

#include <cstdint>
#include <cstring>

namespace ps1 {

// ─── Register aliases ────────────────────────────────────

enum Register : uint32_t {
  ZERO = 0,
  AT = 1,
  V0 = 2,
  V1 = 3,
  A0 = 4,
  A1 = 5,
  A2 = 6,
  A3 = 7,
  T0 = 8,
  T1 = 9,
  T2 = 10,
  T3 = 11,
  T4 = 12,
  T5 = 13,
  T6 = 14,
  T7 = 15,
  S0 = 16,
  S1 = 17,
  S2 = 18,
  S3 = 19,
  S4 = 20,
  S5 = 21,
  S6 = 22,
  S7 = 23,
  T8 = 24,
  T9 = 25,
  K0 = 26,
  K1 = 27,
  GP = 28,
  SP = 29,
  FP = 30, // Also S8
  RA = 31,
};

// ─── COP0 Register indices ──────────────────────────────

enum COP0Reg : uint32_t {
  COP0_BPC = 3,      // Breakpoint on execute
  COP0_BDA = 5,      // Breakpoint on data access
  COP0_JUMPDEST = 6, // Jump destination
  COP0_DCIC = 7,     // Breakpoint control
  COP0_BADVADDR = 8, // Bad virtual address
  COP0_BDAM = 9,     // Data access breakpoint mask
  COP0_BPCM = 11,    // Execute breakpoint mask
  COP0_SR = 12,      // Status Register
  COP0_CAUSE = 13,   // Cause Register
  COP0_EPC = 14,     // Exception PC
  COP0_PRID = 15,    // Processor ID
};

// ─── CPU Context ────────────────────────────────────────

/// PS1 R3000A CPU state — used by recompiled code
/// Fields match what the InstructionEmitter and GteEmitter reference:
///   ctx->rN, ctx->hi, ctx->lo, ctx->cop0[N], ctx->cop2d[N], ctx->cop2c[N]
struct CPUContext {
  // General Purpose Registers (r0 = always zero)
  uint32_t r[32];

  // Program Counter
  uint32_t pc;

  // Multiply/Divide result registers
  uint32_t hi;
  uint32_t lo;

  // COP0 — System Control Coprocessor (16 registers used on PS1)
  uint32_t cop0[16];

  // COP2 — GTE (Geometry Transform Engine)
  uint32_t cop2d[32]; // Data registers (VXY0, IR0, RGBC, SXY0, MAC0, etc.)
  uint32_t cop2c[32]; // Control registers (RT11RT12, TRX, FLAG, etc.)

  /// Reset all registers to zero
  void reset() { std::memset(this, 0, sizeof(*this)); }

  /// Enforce r0 = 0 (call after each instruction)
  void enforceR0() { r[0] = 0; }
};

} // namespace ps1
