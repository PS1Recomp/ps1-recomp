#pragma once

// ps1xRecomp — Instruction Emitter
// Translates decoded MIPS I instructions to C++ code

#include "mips_decoder.h"
#include <cstdint>
#include <string>
#include <vector>
#include <functional>

namespace ps1recomp {

// ─── Function context for emission ──────────────────────

/// Represents a function being recompiled
struct RecompFunction {
    std::string name;
    uint32_t    address = 0;
    uint32_t    size    = 0;
    std::vector<uint32_t> instructions;  // Raw 32-bit words

    /// Whether a given address within this function is a branch/jump target
    /// (used for label generation)
    std::vector<bool> isLabelTarget;
};

// ─── Function call resolver ─────────────────────────────

/// Callback to resolve JAL target address to function name
using FuncResolver = std::function<std::string(uint32_t address)>;

// ─── Instruction Emitter ─────────────────────────────────

class InstructionEmitter {
public:
    /// Set the function call resolver (for JAL targets → names)
    void setFuncResolver(FuncResolver resolver) { m_resolver = std::move(resolver); }

    /// Emit a single instruction as C++ code
    /// @param inst  Decoded instruction
    /// @param pc    Address of this instruction
    /// @return C++ code line(s) as string
    std::string emitInstruction(const Instruction& inst, uint32_t pc) const;

    /// Emit an entire function as C++ code
    /// @param func  Function context with instructions
    /// @return Complete C++ function body
    std::string emitFunction(const RecompFunction& func) const;

    // ── Individual emitters (public for testing) ──

    std::string emitALU(const Instruction& inst) const;
    std::string emitShift(const Instruction& inst) const;
    std::string emitMulDiv(const Instruction& inst) const;
    std::string emitLoad(const Instruction& inst) const;
    std::string emitStore(const Instruction& inst) const;
    std::string emitBranch(const Instruction& inst, uint32_t pc) const;
    std::string emitJump(const Instruction& inst, uint32_t pc) const;
    std::string emitCOP0(const Instruction& inst) const;
    std::string emitGTEMove(const Instruction& inst) const;
    std::string emitSystem(const Instruction& inst) const;

    // ── Helpers ──

    /// Format a GPR reference: ctx->r{N}
    static std::string reg(uint8_t r);

    /// Format a label: L_{address:08X}
    static std::string label(uint32_t addr);

private:
    FuncResolver m_resolver;

    std::string defaultFuncName(uint32_t addr) const;
};

} // namespace ps1recomp
