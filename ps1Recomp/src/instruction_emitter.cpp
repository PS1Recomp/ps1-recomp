// ps1Recomp -- Instruction Emitter Implementation
// Translates decoded MIPS I instructions to C++ code using runtime macros

#include "ps1recomp/instruction_emitter.h"
#include <fmt/format.h>
#include <set>

namespace ps1recomp {

// Register Reference

std::string InstructionEmitter::reg(uint8_t r) {
  if (r == 0)
    return "(int32_t)0"; // $zero is always 0
  return fmt::format("ctx->r{}", r);
}

// Returns an expression suitable as a write destination.
// Writes to $zero are discarded in MIPS, so we wrap in (void).
std::string InstructionEmitter::regWrite(uint8_t r) {
  if (r == 0)
    return "/* $zero discard */"; // Will be used as: /* $zero discard */ = expr
                                  // -> becomes a comment
  return fmt::format("ctx->r{}", r);
}

// Wraps an assignment: if dest is $zero, discard the result
std::string InstructionEmitter::assignReg(uint8_t r, const std::string &expr) {
  if (r == 0)
    return fmt::format("(void)({}); /* $zero */", expr);
  return fmt::format("ctx->r{} = {};", r, expr);
}

std::string InstructionEmitter::label(uint32_t addr) {
  return fmt::format("L_{:08X}", addr);
}

std::string InstructionEmitter::defaultFuncName(uint32_t addr) const {
  if (m_resolver) {
    auto name = m_resolver(addr);
    if (!name.empty())
      return name;
  }
  return fmt::format("func_{:08X}", addr);
}

// Signed/Unsigned cast helpers

static std::string s32(const std::string &val) {
  return fmt::format("(int32_t)({})", val);
}

static std::string u32(const std::string &val) {
  return fmt::format("(uint32_t)({})", val);
}

// Delay Slot Conflict Detection
// Returns the GPR index written by an instruction, or -1 if none.
// Used to detect when a delay slot instruction modifies a register
// that the preceding branch/jump reads for its condition/target.

static int getDestGPR(const Instruction &inst) {
  if (inst.isNOP() || !inst.isValid())
    return -1;

  switch (inst.category) {
  case InstrCategory::ALU:
    // R-type (ADD..SLTU, SLL..SRAV) writes rd; I-type writes rt
    if (inst.id <= InstrId::SLTU ||
        (inst.id >= InstrId::SLL && inst.id <= InstrId::SRAV))
      return inst.rd;
    return inst.rt;
  case InstrCategory::Memory:
    return inst.isLoad() ? static_cast<int>(inst.rt) : -1;
  case InstrCategory::MulDiv:
    if (inst.id == InstrId::MFHI || inst.id == InstrId::MFLO)
      return inst.rd;
    return -1; // MULT, DIV etc. write HI/LO, not GPR
  case InstrCategory::COP0:
    if (inst.id == InstrId::MFC0)
      return inst.rt;
    return -1;
  case InstrCategory::GTE:
    if (inst.id == InstrId::MFC2 || inst.id == InstrId::CFC2)
      return inst.rt;
    return -1;
  case InstrCategory::Jump:
    if (inst.id == InstrId::JAL)
      return 31;
    if (inst.id == InstrId::JALR)
      return inst.rd;
    return -1;
  default:
    return -1;
  }
}

// Replace all occurrences of `from` with `to` in `str`, but only when the
// match is NOT followed by an ASCII digit (avoids replacing "ctx->r1" inside
// "ctx->r12").
static void replaceRegRef(std::string &str, const std::string &from,
                          const std::string &to) {
  size_t pos = 0;
  while ((pos = str.find(from, pos)) != std::string::npos) {
    size_t end = pos + from.size();
    // Make sure we're not matching a prefix of a longer register name
    if (end < str.size() && str[end] >= '0' && str[end] <= '9') {
      pos = end;
      continue;
    }
    str.replace(pos, from.size(), to);
    pos += to.size();
  }
}

// ALU Emitter

std::string InstructionEmitter::emitALU(const Instruction &inst) const {
  auto rs = reg(inst.rs);
  auto rt = reg(inst.rt);

  switch (inst.id) {
  // R-type ALU -- emit as unsigned to avoid C++ signed-overflow UB.
  // MIPS ADDU/SUBU are explicitly modular; PSY-Q ADD/SUB also rely on
  // modular wrap in practice (no overflow trap delivered to PSX user code).
  // Casting through uint32_t makes the wraparound well-defined; the trailing
  // `(int32_t)` cast is a no-op bit reinterpret on a 2's-complement target.
  case InstrId::ADD:
  case InstrId::ADDU:
    return assignReg(inst.rd,
                     fmt::format("(int32_t)({} + {})", u32(rs), u32(rt)));
  case InstrId::SUB:
  case InstrId::SUBU:
    return assignReg(inst.rd,
                     fmt::format("(int32_t)({} - {})", u32(rs), u32(rt)));
  case InstrId::AND:
    return assignReg(inst.rd, fmt::format("{} & {}", rs, rt));
  case InstrId::OR:
    return assignReg(inst.rd, fmt::format("{} | {}", rs, rt));
  case InstrId::XOR:
    return assignReg(inst.rd, fmt::format("{} ^ {}", rs, rt));
  case InstrId::NOR:
    return assignReg(inst.rd, fmt::format("~({} | {})", rs, rt));
  case InstrId::SLT:
    return assignReg(inst.rd,
                     fmt::format("({} < {}) ? 1 : 0", s32(rs), s32(rt)));
  case InstrId::SLTU:
    return assignReg(inst.rd,
                     fmt::format("({} < {}) ? 1 : 0", u32(rs), u32(rt)));

  // I-type ALU -- same modular-wrap reasoning as ADD/ADDU above.
  // Cast `inst.imm16` (signed 16-bit) to int32_t first so the sign-extension
  // happens before the unsigned conversion; the immediate is treated as a
  // signed offset by every PSY-Q caller that uses ADDIU.
  case InstrId::ADDI:
  case InstrId::ADDIU:
    return assignReg(inst.rt,
                     fmt::format("(int32_t)({} + (uint32_t)(int32_t){})",
                                 u32(reg(inst.rs)),
                                 static_cast<int32_t>(inst.imm16)));
  case InstrId::ANDI:
    return assignReg(inst.rt, fmt::format("{} & 0x{:04X}", reg(inst.rs),
                                          static_cast<uint16_t>(inst.imm16)));
  case InstrId::ORI:
    return assignReg(inst.rt, fmt::format("{} | 0x{:04X}", reg(inst.rs),
                                          static_cast<uint16_t>(inst.imm16)));
  case InstrId::XORI:
    return assignReg(inst.rt, fmt::format("{} ^ 0x{:04X}", reg(inst.rs),
                                          static_cast<uint16_t>(inst.imm16)));
  case InstrId::SLTI:
    return assignReg(inst.rt, fmt::format("({} < {}) ? 1 : 0",
                                          s32(reg(inst.rs)), inst.imm16));
  case InstrId::SLTIU:
    return assignReg(
        inst.rt,
        fmt::format("({} < {}) ? 1 : 0", u32(reg(inst.rs)),
                    static_cast<uint32_t>(static_cast<int32_t>(inst.imm16))));
  case InstrId::LUI:
    return assignReg(inst.rt, fmt::format("0x{:04X}0000",
                                          static_cast<uint16_t>(inst.imm16)));

  default:
    return fmt::format("// UNKNOWN ALU: {}", MipsDecoder::instrName(inst.id));
  }
}

// Shift Emitter

std::string InstructionEmitter::emitShift(const Instruction &inst) const {
  auto rt = reg(inst.rt);
  auto rs = reg(inst.rs);

  switch (inst.id) {
  case InstrId::SLL:
    return assignReg(inst.rd,
                     fmt::format("(int32_t)({} << {})", u32(rt), inst.shamt));
  case InstrId::SRL:
    return assignReg(inst.rd,
                     fmt::format("(int32_t)({} >> {})", u32(rt), inst.shamt));
  case InstrId::SRA:
    return assignReg(inst.rd, fmt::format("{} >> {}", s32(rt), inst.shamt));
  case InstrId::SLLV:
    return assignReg(inst.rd,
                     fmt::format("(int32_t)({} << ({} & 31))", u32(rt), rs));
  case InstrId::SRLV:
    return assignReg(inst.rd,
                     fmt::format("(int32_t)({} >> ({} & 31))", u32(rt), rs));
  case InstrId::SRAV:
    return assignReg(inst.rd, fmt::format("{} >> ({} & 31)", s32(rt), rs));
  default:
    return fmt::format("// UNKNOWN SHIFT: {}", MipsDecoder::instrName(inst.id));
  }
}

// Multiply / Divide Emitter

std::string InstructionEmitter::emitMulDiv(const Instruction &inst) const {
  auto rs = reg(inst.rs);
  auto rt = reg(inst.rt);
  auto rd = reg(inst.rd);

  switch (inst.id) {
  case InstrId::MULT:
    return fmt::format(
        "{{ int64_t result = (int64_t){} * (int64_t){}; "
        "ctx->lo = (int32_t)result; ctx->hi = (int32_t)(result >> 32); }}",
        s32(rs), s32(rt));
  case InstrId::MULTU:
    return fmt::format(
        "{{ uint64_t result = (uint64_t){} * (uint64_t){}; "
        "ctx->lo = (int32_t)result; ctx->hi = (int32_t)(result >> 32); }}",
        u32(rs), u32(rt));
  case InstrId::DIV:
    return fmt::format(
        "if ({} != 0) {{ ctx->lo = {} / {}; ctx->hi = {} % {}; }}", s32(rt),
        s32(rs), s32(rt), s32(rs), s32(rt));
  case InstrId::DIVU:
    return fmt::format("if ({} != 0) {{ ctx->lo = (int32_t)({} / {}); "
                       "ctx->hi = (int32_t)({} % {}); }}",
                       u32(rt), u32(rs), u32(rt), u32(rs), u32(rt));
  case InstrId::MFHI:
    return assignReg(inst.rd, "ctx->hi");
  case InstrId::MFLO:
    return assignReg(inst.rd, "ctx->lo");
  case InstrId::MTHI:
    return fmt::format("ctx->hi = {};", rs);
  case InstrId::MTLO:
    return fmt::format("ctx->lo = {};", rs);
  default:
    return fmt::format("// UNKNOWN MULDIV: {}",
                       MipsDecoder::instrName(inst.id));
  }
}

// Load Emitter

std::string InstructionEmitter::emitLoad(const Instruction &inst) const {
  auto base = reg(inst.rs);
  auto offset = inst.imm16;

  switch (inst.id) {
  case InstrId::LB:
    return assignReg(inst.rt,
                     fmt::format("MEM_READ8(ctx, {} + {})", s32(base), offset));
  case InstrId::LBU:
    return assignReg(inst.rt, fmt::format("(uint8_t)MEM_READ8(ctx, {} + {})",
                                          s32(base), offset));
  case InstrId::LH:
    return assignReg(
        inst.rt, fmt::format("MEM_READ16(ctx, {} + {})", s32(base), offset));
  case InstrId::LHU:
    return assignReg(inst.rt, fmt::format("(uint16_t)MEM_READ16(ctx, {} + {})",
                                          s32(base), offset));
  case InstrId::LW:
    return assignReg(
        inst.rt, fmt::format("MEM_READ32(ctx, {} + {})", s32(base), offset));
  case InstrId::LWL:
    return assignReg(inst.rt, fmt::format("DO_LWL(ctx, {}, {} + {})",
                                          reg(inst.rt), s32(base), offset));
  case InstrId::LWR:
    return assignReg(inst.rt, fmt::format("DO_LWR(ctx, {}, {} + {})",
                                          reg(inst.rt), s32(base), offset));
  default:
    return fmt::format("// UNKNOWN LOAD: {}", MipsDecoder::instrName(inst.id));
  }
}

// Store Emitter

std::string InstructionEmitter::emitStore(const Instruction &inst) const {
  auto rt = reg(inst.rt);
  auto base = reg(inst.rs);
  auto offset = inst.imm16;

  switch (inst.id) {
  case InstrId::SB:
    return fmt::format("MEM_WRITE8(ctx, {} + {}, {});", s32(base), offset, rt);
  case InstrId::SH:
    return fmt::format("MEM_WRITE16(ctx, {} + {}, {});", s32(base), offset, rt);
  case InstrId::SW:
    return fmt::format("MEM_WRITE32(ctx, {} + {}, {});", s32(base), offset, rt);
  case InstrId::SWL:
    return fmt::format("DO_SWL(ctx, {} + {}, {});", s32(base), offset, rt);
  case InstrId::SWR:
    return fmt::format("DO_SWR(ctx, {} + {}, {});", s32(base), offset, rt);
  default:
    return fmt::format("// UNKNOWN STORE: {}", MipsDecoder::instrName(inst.id));
  }
}

// Branch Emitter

std::string InstructionEmitter::emitBranch(const Instruction &inst,
                                           uint32_t pc) const {
  auto rs = reg(inst.rs);
  auto rt = reg(inst.rt);
  auto target = label(inst.branchTarget(pc));

  switch (inst.id) {
  case InstrId::BEQ:
    return fmt::format("if ({} == {}) goto {};", s32(rs), s32(rt), target);
  case InstrId::BNE:
    return fmt::format("if ({} != {}) goto {};", s32(rs), s32(rt), target);
  case InstrId::BLEZ:
    return fmt::format("if ({} <= 0) goto {};", s32(rs), target);
  case InstrId::BGTZ:
    return fmt::format("if ({} > 0) goto {};", s32(rs), target);
  case InstrId::BLTZ:
    return fmt::format("if ({} < 0) goto {};", s32(rs), target);
  case InstrId::BGEZ:
    return fmt::format("if ({} >= 0) goto {};", s32(rs), target);
  case InstrId::BLTZAL:
    return fmt::format("ctx->r31 = 0x{:08X}; if ({} < 0) goto {};", pc + 8,
                       s32(rs), target);
  case InstrId::BGEZAL:
    return fmt::format("ctx->r31 = 0x{:08X}; if ({} >= 0) goto {};", pc + 8,
                       s32(rs), target);
  default:
    return fmt::format("// UNKNOWN BRANCH: {}",
                       MipsDecoder::instrName(inst.id));
  }
}

// Jump Emitter

std::string InstructionEmitter::emitJump(const Instruction &inst,
                                         uint32_t pc) const {
  switch (inst.id) {
  case InstrId::J:
    return fmt::format("goto {};", label(inst.jumpTarget(pc)));
  case InstrId::JAL: {
    auto target = inst.jumpTarget(pc);
    auto name = defaultFuncName(target);
    return fmt::format("{}(rdram, ctx);", name);
  }
  case InstrId::JR:
    if (inst.rs == 31) {
      return "return;";
    }
    return fmt::format("JUMP_INDIRECT(ctx, {});", reg(inst.rs));
  case InstrId::JALR:
    return fmt::format("ctx->r{} = 0x{:08X}; CALL_INDIRECT(ctx, {});", inst.rd,
                       pc + 8, reg(inst.rs));
  default:
    return fmt::format("// UNKNOWN JUMP: {}", MipsDecoder::instrName(inst.id));
  }
}

// COP0 Emitter

std::string InstructionEmitter::emitCOP0(const Instruction &inst) const {
  switch (inst.id) {
  case InstrId::MFC0:
    return assignReg(inst.rt, fmt::format("ctx->cop0[{}] /* {} */", inst.rd,
                                          cop0Name(inst.rd)));
  case InstrId::MTC0:
    return fmt::format("ctx->cop0[{}] = {}; // {}", inst.rd, reg(inst.rt),
                       cop0Name(inst.rd));
  case InstrId::RFE:
    return "COP0_RFE(ctx);";
  default:
    return fmt::format("// UNKNOWN COP0: {}", MipsDecoder::instrName(inst.id));
  }
}

// GTE Register Move Emitter

std::string InstructionEmitter::emitGTEMove(const Instruction &inst) const {
  switch (inst.id) {
  case InstrId::MFC2:
    return assignReg(inst.rt, fmt::format("ctx->cop2d[{}]", inst.rd));
  case InstrId::MTC2:
    return fmt::format("ctx->cop2d[{}] = {};", inst.rd, reg(inst.rt));
  case InstrId::CFC2:
    return assignReg(inst.rt, fmt::format("ctx->cop2c[{}]", inst.rd));
  case InstrId::CTC2:
    return fmt::format("ctx->cop2c[{}] = {};", inst.rd, reg(inst.rt));
  case InstrId::LWC2:
    return fmt::format("ctx->cop2d[{}] = MEM_READ32(ctx, {} + {});", inst.rt,
                       s32(reg(inst.rs)), inst.imm16);
  case InstrId::SWC2:
    return fmt::format("MEM_WRITE32(ctx, {} + {}, ctx->cop2d[{}]);",
                       s32(reg(inst.rs)), inst.imm16, inst.rt);
  default:
    return fmt::format("// UNKNOWN GTE MOVE: {}",
                       MipsDecoder::instrName(inst.id));
  }
}

// System Emitter

std::string InstructionEmitter::emitSystem(const Instruction &inst) const {
  switch (inst.id) {
  case InstrId::SYSCALL:
    return "SYSCALL(ctx);";
  case InstrId::BREAK:
    return "BREAK(ctx);";
  default:
    return fmt::format("// UNKNOWN SYSTEM: {}",
                       MipsDecoder::instrName(inst.id));
  }
}

// Main Dispatch

std::string InstructionEmitter::emitInstruction(const Instruction &inst,
                                                uint32_t pc) const {
  if (inst.isNOP())
    return "// NOP";
  if (!inst.isValid())
    return fmt::format("// INVALID: 0x{:08X}", inst.raw);

  // Skip writes to $zero
  bool writesToZero = false;
  switch (inst.category) {
  case InstrCategory::ALU:
    // R-type writes to rd, I-type writes to rt
    if (inst.id <= InstrId::SLTU)
      writesToZero = (inst.rd == 0);
    else
      writesToZero = (inst.rt == 0);
    break;
  case InstrCategory::Memory:
    if (inst.isLoad())
      writesToZero = (inst.rt == 0);
    break;
  case InstrCategory::MulDiv:
    if (inst.id == InstrId::MFHI || inst.id == InstrId::MFLO)
      writesToZero = (inst.rd == 0);
    break;
  default:
    break;
  }
  if (writesToZero)
    return "// NOP (writes to $zero)";

  switch (inst.category) {
  case InstrCategory::ALU: {
    // Shifts are ALU-category but have separate emitter
    if (inst.id >= InstrId::SLL && inst.id <= InstrId::SRAV)
      return emitShift(inst);
    return emitALU(inst);
  }
  case InstrCategory::Memory:
    return inst.isStore() ? emitStore(inst) : emitLoad(inst);
  case InstrCategory::Branch:
    return emitBranch(inst, pc);
  case InstrCategory::Jump:
    return emitJump(inst, pc);
  case InstrCategory::MulDiv:
    return emitMulDiv(inst);
  case InstrCategory::System:
    return emitSystem(inst);
  case InstrCategory::COP0:
    return emitCOP0(inst);
  case InstrCategory::GTE: {
    // GTE commands vs register moves
    if (inst.id >= InstrId::GTE_RTPS && inst.id <= InstrId::GTE_NCCT) {
      bool sf = (inst.raw & (1 << 19)) != 0;
      bool lm = (inst.raw & (1 << 10)) != 0;
      std::string name_str{MipsDecoder::instrName(inst.id)};
      if (name_str.substr(0, 4) == "GTE_")
        name_str = name_str.substr(4);

      if (inst.id == InstrId::GTE_MVMVA) {
        uint8_t mx = (inst.raw >> 17) & 3;
        uint8_t mv = (inst.raw >> 15) & 3;
        uint8_t tv = (inst.raw >> 13) & 3;
        return fmt::format("ps1::GTE::MVMVA(ctx, {}, {}, {}, {}, {}); // MVMVA",
                           mx, mv, tv, sf ? "true" : "false",
                           lm ? "true" : "false");
      }
      return fmt::format("ps1::GTE::{}(ctx, {}, {}); // GTE cmd", name_str,
                         sf ? "true" : "false", lm ? "true" : "false");
    }
    return emitGTEMove(inst);
  }
  case InstrCategory::Special:
    return fmt::format("// {}: 0x{:08X}", MipsDecoder::instrName(inst.id),
                       inst.raw);
  default:
    return fmt::format("// UNHANDLED: 0x{:08X}", inst.raw);
  }
}

// Function Emitter

std::string InstructionEmitter::emitFunction(const RecompFunction &func) const {
  std::string result;

  // Function signature
  result += fmt::format("void {}(uint8_t* rdram, recomp_context* ctx) {{\n",
                        func.name);

  uint32_t pc = func.address;
  size_t numInstr = func.instructions.size();

  // Pass 1: Pre-scan ALL instructions
  // Scan every instruction to find all branch/jump targets.
  // For targets within the emitted instruction range: mark as label.
  // For anything else (OOB or in gap beyond loaded instrs): dispatch.
  //
  // IMPORTANT: After an unconditional jump (JR/JALR) + delay slot,
  // subsequent bytes may be DATA (jump tables, padding), not code.
  // We skip unreachable regions to avoid misinterpreting data as branches.
  std::set<uint32_t> oob_set; // deduplicated OOB targets
  // Copy isLabelTarget so we can add new targets discovered in data-as-code
  std::vector<bool> labelTargets = func.isLabelTarget;
  if (labelTargets.size() < numInstr) {
    labelTargets.resize(numInstr, false);
  }

  auto classifyTarget = [&](uint32_t target) {
    // Check if target falls within the actual emitted instructions
    if (target >= func.address) {
      size_t idx = (target - func.address) / 4;
      if (idx < labelTargets.size()) {
        // In-range of emitted instructions: mark as label
        labelTargets[idx] = true;
        return;
      }
    }
    // OOB: outside function OR in the gap beyond loaded instructions
    oob_set.insert(target);
  };

  // First pass: classify ALL targets from ALL instructions.
  // Even data words might appear as instructions in Pass 2 if reachability
  // diverges, so we must ensure every possible target has a label.
  for (size_t i = 0; i < numInstr; ++i) {
    uint32_t addr = func.address + static_cast<uint32_t>(i * 4);
    Instruction inst = MipsDecoder::decode(func.instructions[i]);

    if (inst.category == InstrCategory::Branch) {
      classifyTarget(inst.branchTarget(addr));
    } else if (inst.category == InstrCategory::Jump && inst.id == InstrId::J) {
      classifyTarget(inst.jumpTarget(addr));
    }
  }

  // Register jump table targets so labels are emitted for them.
  for (const auto &jt : func.jumpTables) {
    for (uint32_t target : jt.targets) {
      classifyTarget(target);
    }
  }

  // Pass 2: Emit code
  // Track reachability: after JR/JALR + delay slot, treat subsequent
  // words as data comments until the next branch target label.
  bool reachable = true;
  int skipAfterJump = 0;

  for (size_t i = 0; i < numInstr; ++i) {
    uint32_t addr = func.address + static_cast<uint32_t>(i * 4);

    // Emit label if this address is a branch target
    if (i < labelTargets.size() && labelTargets[i]) {
      result += fmt::format("{}:\n", label(addr));
      reachable = true;
      skipAfterJump = 0;
    }

    if (skipAfterJump > 0) {
      skipAfterJump--;
      if (skipAfterJump == 0) {
        reachable = false;
      }
    }

    if (!reachable) {
      // Emit as data comment instead of decoding
      result +=
          fmt::format("    // .word 0x{:08X} (data)\n", func.instructions[i]);
      continue;
    }

    Instruction inst = MipsDecoder::decode(func.instructions[i]);
    std::string code = emitInstruction(inst, addr);

    // Jump table override
    // If this JR $rx has a detected jump table, replace JUMP_INDIRECT
    // with a static switch/goto over the known target addresses.
    // A JUMP_INDIRECT fallback is kept for safety.
    if (inst.id == InstrId::JR && inst.rs != 31 && !func.jumpTables.empty()) {
      for (const auto &jt : func.jumpTables) {
        if (jt.jrInstrIdx == i && !jt.targets.empty()) {
          std::string sw;
          sw += fmt::format("{{ // switch table ({} entries)\n", jt.targets.size());
          sw += fmt::format("    uint32_t _sw_target = static_cast<uint32_t>({});\n",
                            reg(inst.rs));
          for (uint32_t target : jt.targets) {
            sw += fmt::format("    if (_sw_target == 0x{:08X}u) goto {};\n",
                              target, label(target));
          }
          sw += fmt::format("    JUMP_INDIRECT(ctx, {}); // fallback\n", reg(inst.rs));
          sw += "    }";
          code = sw;
          break;
        }
      }
    }

    // Handle branch delay slots: if this instruction has a delay slot,
    // emit the next instruction BEFORE the branch/jump.
    //
    // IMPORTANT -- MIPS delay slot semantics:
    //   In real hardware the branch condition (or jump target register)
    //   is evaluated BEFORE the delay slot executes.  If the delay slot
    //   writes to a register that the branch/jump reads, we must save
    //   that register to a temporary before the delay slot runs, then
    //   use the saved value in the branch/jump.
    if (inst.hasBranchDelaySlot() && i + 1 < numInstr) {
      Instruction delayInst = MipsDecoder::decode(func.instructions[i + 1]);
      std::string delayCode = emitInstruction(delayInst, addr + 4);

      int destGPR = getDestGPR(delayInst);

      // Detect conflict: delay slot writes to a register that the
      // branch condition or jump target reads.
      // For conditional branches: rs (and rt for BEQ/BNE).
      // For JR/JALR: rs (the target register).
      std::set<int> conflictRegs;
      if (destGPR > 0) { // r0 is hardwired to 0, no conflict possible
        if (inst.isBranch()) {
          if (destGPR == static_cast<int>(inst.rs))
            conflictRegs.insert(destGPR);
          if ((inst.id == InstrId::BEQ || inst.id == InstrId::BNE) &&
              destGPR == static_cast<int>(inst.rt))
            conflictRegs.insert(destGPR);
        } else if (inst.id == InstrId::JR || inst.id == InstrId::JALR) {
          if (destGPR == static_cast<int>(inst.rs))
            conflictRegs.insert(destGPR);
        }
      }

      // Auto-inject drainPendingCallbacks for backward branches
      // Backward branches indicate potential busy-wait / polling loops.
      // We inject a yield point so the game thread can process pending
      // hardware callbacks (CD-ROM sector delivery, VBlank events, etc).
      // Without this, any recompiled polling loop would spin forever.
      std::string yieldCode;
      if (inst.isBranch()) {
        uint32_t brTarget = inst.branchTarget(addr);
        if (brTarget <= addr) {
          yieldCode =
              "    if (ctx->bios) ctx->bios->drainPendingCallbacks();\n";
        }
      }

      if (!conflictRegs.empty()) {
        // Save conflicting registers, run delay slot, then branch/jump
        // using the saved values.  Wrap in a block scope so that the
        // local temporaries don't get crossed by gotos elsewhere in the
        // function (C++ forbids goto jumping over an initialiser).
        result += "    {\n";
        for (int r : conflictRegs) {
          result += fmt::format(
              "      uint32_t _ds_r{} = ctx->r{}; // save for branch\n", r, r);
        }
        result += fmt::format("      {} // delay slot\n", delayCode);
        std::string branchCode = code;
        for (int r : conflictRegs) {
          replaceRegRef(branchCode, fmt::format("ctx->r{}", r),
                        fmt::format("_ds_r{}", r));
        }
        result += yieldCode;
        result += fmt::format("      {}\n", branchCode);
        result += "    }\n";
      } else {
        // No conflict -- emit delay slot then branch/jump (original order)
        result += fmt::format("    {} // delay slot\n", delayCode);
        result += yieldCode;
        result += fmt::format("    {}\n", code);
      }
      ++i; // Skip delay slot instruction
    } else {
      result += fmt::format("    {}\n", code);
    }

    // Track unconditional jumps for reachability.
    // Only JR makes subsequent code unreachable -- JALR is a function call
    // that returns, so code after JALR is still reachable.
    if (inst.category == InstrCategory::Jump && inst.id == InstrId::JR) {
      skipAfterJump = 1; // delay slot was already consumed above, skip 1 more
    }
  }

  // Emit OOB target labels
  // Always use recomp_dispatch for OOB -- we can't trust that
  // the target corresponds to a known function.
  for (uint32_t target : oob_set) {
    result += fmt::format("{}:\n", label(target));
    result += fmt::format(
        "    recomp_dispatch(rdram, ctx, 0x{:08X}); return;\n", target);
  }

  // Post-pass: catch-all for undefined labels
  // Scan the generated code for all "goto L_XXXXXXXX" references
  // and ensure every one has a matching "L_XXXXXXXX:" definition.
  // This catches any edge cases the pre-scan missed.
  {
    std::set<uint32_t> referenced;
    std::set<uint32_t> defined;

    // Find all "goto L_" references
    size_t pos = 0;
    while ((pos = result.find("goto L_", pos)) != std::string::npos) {
      pos += 7; // skip "goto L_"
      if (pos + 8 <= result.size()) {
        try {
          uint32_t addr = std::stoul(result.substr(pos, 8), nullptr, 16);
          referenced.insert(addr);
        } catch (...) {
        }
      }
      pos += 8;
    }

    // Find all "L_XXXXXXXX:" label definitions
    pos = 0;
    while ((pos = result.find("L_", pos)) != std::string::npos) {
      size_t lpos = pos;
      pos += 2; // skip "L_"
      if (pos + 8 <= result.size() && pos + 8 < result.size() &&
          result[pos + 8] == ':') {
        // Check this isn't preceded by "goto " (i.e., it's a label def)
        if (lpos == 0 || result[lpos - 1] == '\n' || result[lpos - 1] == ' ') {
          try {
            uint32_t addr = std::stoul(result.substr(pos, 8), nullptr, 16);
            defined.insert(addr);
          } catch (...) {
          }
        }
      }
    }

    // Emit dispatch for any referenced-but-undefined labels
    for (uint32_t addr : referenced) {
      if (defined.find(addr) == defined.end()) {
        result += fmt::format("{}:\n", label(addr));
        result += fmt::format(
            "    recomp_dispatch(rdram, ctx, 0x{:08X}); return;\n", addr);
      }
    }
  }

  result += "}\n";
  return result;
}

} // namespace ps1recomp
