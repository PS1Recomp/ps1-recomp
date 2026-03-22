#pragma once

// ps1Analyzer — Function Finder
// Detects function boundaries in PS1 MIPS binaries using heuristics and symbols

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "ps1recomp/elf_parser.h"

namespace ps1recomp {

// ─── MIPS Instruction Constants ──────────────────────────

namespace mips {
// Opcodes (bits 31-26)
constexpr uint32_t OP_SPECIAL = 0x00; // R-type
constexpr uint32_t OP_REGIMM = 0x01;  // BLTZ, BGEZ, etc.
constexpr uint32_t OP_J = 0x02;
constexpr uint32_t OP_JAL = 0x03;
constexpr uint32_t OP_BEQ = 0x04;
constexpr uint32_t OP_BNE = 0x05;
constexpr uint32_t OP_BLEZ = 0x06;
constexpr uint32_t OP_BGTZ = 0x07;
constexpr uint32_t OP_ADDIU = 0x09;

// SPECIAL function codes (bits 5-0)
constexpr uint32_t FUNC_JR = 0x08;
constexpr uint32_t FUNC_JALR = 0x09;

// Register numbers
constexpr uint32_t REG_SP = 29;
constexpr uint32_t REG_RA = 31;

// Field extraction helpers
inline uint32_t getOpcode(uint32_t instr) { return (instr >> 26) & 0x3F; }
inline uint32_t getRs(uint32_t instr) { return (instr >> 21) & 0x1F; }
inline uint32_t getRt(uint32_t instr) { return (instr >> 16) & 0x1F; }
inline uint32_t getRd(uint32_t instr) { return (instr >> 11) & 0x1F; }
inline uint32_t getFunction(uint32_t instr) { return instr & 0x3F; }
inline int16_t getImm16(uint32_t instr) {
  return static_cast<int16_t>(instr & 0xFFFF);
}
inline uint32_t getTarget26(uint32_t instr) { return instr & 0x03FFFFFF; }

/// Compute JAL target address: (PC & 0xF0000000) | (target26 << 2)
inline uint32_t jalTarget(uint32_t pc, uint32_t instr) {
  return (pc & 0xF0000000) | (getTarget26(instr) << 2);
}

/// Check if instruction is JAL
inline bool isJAL(uint32_t instr) { return getOpcode(instr) == OP_JAL; }

/// Check if instruction is JR $ra (function return)
inline bool isJR_RA(uint32_t instr) {
  return getOpcode(instr) == OP_SPECIAL && getFunction(instr) == FUNC_JR &&
         getRs(instr) == REG_RA;
}

/// Check if instruction is ADDIU $sp, $sp, -N (prologue)
inline bool isStackPrologue(uint32_t instr) {
  return getOpcode(instr) == OP_ADDIU && getRs(instr) == REG_SP &&
         getRt(instr) == REG_SP &&
         getImm16(instr) < 0; // Negative = allocating stack
}

/// Check if instruction is ADDIU $sp, $sp, +N (epilogue)
inline bool isStackEpilogue(uint32_t instr) {
  return getOpcode(instr) == OP_ADDIU && getRs(instr) == REG_SP &&
         getRt(instr) == REG_SP &&
         getImm16(instr) > 0; // Positive = deallocating stack
}

/// Check if instruction is a NOP (0x00000000)
inline bool isNOP(uint32_t instr) { return instr == 0; }

} // namespace mips

// ─── Function Detection Source ───────────────────────────

enum class FunctionSource {
  EntryPoint, // ELF entry point
  Symbol,     // From ELF symbol table (STT_FUNC)
  JALTarget,  // Target of a JAL instruction
  Prologue,   // Detected by ADDIU $sp, $sp, -N pattern
};

// ─── FunctionInfo ────────────────────────────────────────

struct FunctionInfo {
  uint32_t address;      // Start address
  uint32_t size;         // Size in bytes (0 if unknown)
  std::string name;      // Function name (from symbol or generated)
  FunctionSource source; // How this function was detected
  bool isLeaf;           // True if function never calls JAL (no stack frame)

  bool operator<(const FunctionInfo &other) const {
    return address < other.address;
  }
};

// ─── FunctionFinder ──────────────────────────────────────

class FunctionFinder {
public:
  FunctionFinder() = default;

  /// Run function detection on a loaded ELF.
  /// Combines all heuristics and sorts results.
  void findFunctions(const ElfParser &elf);

  /// Get all detected functions (sorted by address).
  const std::vector<FunctionInfo> &getFunctions() const { return m_functions; }

  /// Get function count.
  size_t getFunctionCount() const { return m_functions.size(); }

  /// Find function at exact address (binary search).
  const FunctionInfo *findByAddress(uint32_t addr) const;

  /// Find function containing address.
  const FunctionInfo *findContaining(uint32_t addr) const;

  /// Get all JAL target addresses found (for debug/analysis).
  const std::set<uint32_t> &getJALTargets() const { return m_jalTargets; }

  void addFunction(uint32_t addr, const std::string &name,
                   FunctionSource source);

private:
  std::vector<FunctionInfo> m_functions;
  std::set<uint32_t> m_jalTargets;

  // Detection passes
  void addEntryPoint(const ElfParser &elf);
  void addSymbolFunctions(const ElfParser &elf);
  void scanJALTargets(const Section &text);
  void scanPrologues(const Section &text);
  void computeBoundaries(const Section &text);

  // Helpers
  bool hasFunction(uint32_t addr) const;

  /// Read a 32-bit little-endian instruction from section data.
  static uint32_t readInstruction(const Section &sec, uint32_t offset);
};

} // namespace ps1recomp
