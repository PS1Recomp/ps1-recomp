// ps1xRecomp — Instruction Emitter Implementation
// Translates decoded MIPS I instructions to C++ code using runtime macros

#include "ps1recomp/instruction_emitter.h"
#include <fmt/format.h>

namespace ps1recomp {

// ─── Register Reference ──────────────────────────────────

std::string InstructionEmitter::reg(uint8_t r) {
  if (r == 0)
    return "(int32_t)0"; // $zero is always 0
  return fmt::format("ctx->r{}", r);
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

// ─── Signed/Unsigned cast helpers ────────────────────────

static std::string s32(const std::string &val) {
  return fmt::format("(int32_t)({})", val);
}

static std::string u32(const std::string &val) {
  return fmt::format("(uint32_t)({})", val);
}

// ─── ALU Emitter ─────────────────────────────────────────

std::string InstructionEmitter::emitALU(const Instruction &inst) const {
  auto rd = reg(inst.rd);
  auto rs = reg(inst.rs);
  auto rt = reg(inst.rt);
  auto dst = reg(inst.rt); // I-type dest is rt

  switch (inst.id) {
  // R-type ALU
  case InstrId::ADD:
  case InstrId::ADDU:
    return fmt::format("{} = (int32_t)({} + {});", rd, s32(rs), s32(rt));
  case InstrId::SUB:
  case InstrId::SUBU:
    return fmt::format("{} = (int32_t)({} - {});", rd, s32(rs), s32(rt));
  case InstrId::AND:
    return fmt::format("{} = {} & {};", rd, rs, rt);
  case InstrId::OR:
    return fmt::format("{} = {} | {};", rd, rs, rt);
  case InstrId::XOR:
    return fmt::format("{} = {} ^ {};", rd, rs, rt);
  case InstrId::NOR:
    return fmt::format("{} = ~({} | {});", rd, rs, rt);
  case InstrId::SLT:
    return fmt::format("{} = ({} < {}) ? 1 : 0;", rd, s32(rs), s32(rt));
  case InstrId::SLTU:
    return fmt::format("{} = ({} < {}) ? 1 : 0;", rd, u32(rs), u32(rt));

  // I-type ALU
  case InstrId::ADDI:
  case InstrId::ADDIU:
    return fmt::format("{} = (int32_t)({} + {});", dst, s32(reg(inst.rs)),
                       inst.imm16);
  case InstrId::ANDI:
    return fmt::format("{} = {} & 0x{:04X};", dst, reg(inst.rs),
                       static_cast<uint16_t>(inst.imm16));
  case InstrId::ORI:
    return fmt::format("{} = {} | 0x{:04X};", dst, reg(inst.rs),
                       static_cast<uint16_t>(inst.imm16));
  case InstrId::XORI:
    return fmt::format("{} = {} ^ 0x{:04X};", dst, reg(inst.rs),
                       static_cast<uint16_t>(inst.imm16));
  case InstrId::SLTI:
    return fmt::format("{} = ({} < {}) ? 1 : 0;", dst, s32(reg(inst.rs)),
                       inst.imm16);
  case InstrId::SLTIU:
    return fmt::format("{} = ({} < {}) ? 1 : 0;", dst, u32(reg(inst.rs)),
                       static_cast<uint32_t>(static_cast<int32_t>(inst.imm16)));
  case InstrId::LUI:
    return fmt::format("{} = 0x{:04X}0000;", dst,
                       static_cast<uint16_t>(inst.imm16));

  default:
    return fmt::format("// UNKNOWN ALU: {}", MipsDecoder::instrName(inst.id));
  }
}

// ─── Shift Emitter ───────────────────────────────────────

std::string InstructionEmitter::emitShift(const Instruction &inst) const {
  auto rd = reg(inst.rd);
  auto rt = reg(inst.rt);
  auto rs = reg(inst.rs);

  switch (inst.id) {
  case InstrId::SLL:
    return fmt::format("{} = (int32_t)({} << {});", rd, u32(rt), inst.shamt);
  case InstrId::SRL:
    return fmt::format("{} = (int32_t)({} >> {});", rd, u32(rt), inst.shamt);
  case InstrId::SRA:
    return fmt::format("{} = {} >> {};", rd, s32(rt), inst.shamt);
  case InstrId::SLLV:
    return fmt::format("{} = (int32_t)({} << ({} & 31));", rd, u32(rt), rs);
  case InstrId::SRLV:
    return fmt::format("{} = (int32_t)({} >> ({} & 31));", rd, u32(rt), rs);
  case InstrId::SRAV:
    return fmt::format("{} = {} >> ({} & 31);", rd, s32(rt), rs);
  default:
    return fmt::format("// UNKNOWN SHIFT: {}", MipsDecoder::instrName(inst.id));
  }
}

// ─── Multiply / Divide Emitter ──────────────────────────

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
    return fmt::format("{} = ctx->hi;", rd);
  case InstrId::MFLO:
    return fmt::format("{} = ctx->lo;", rd);
  case InstrId::MTHI:
    return fmt::format("ctx->hi = {};", rs);
  case InstrId::MTLO:
    return fmt::format("ctx->lo = {};", rs);
  default:
    return fmt::format("// UNKNOWN MULDIV: {}",
                       MipsDecoder::instrName(inst.id));
  }
}

// ─── Load Emitter ────────────────────────────────────────

std::string InstructionEmitter::emitLoad(const Instruction &inst) const {
  auto rt = reg(inst.rt);
  auto base = reg(inst.rs);
  auto offset = inst.imm16;

  switch (inst.id) {
  case InstrId::LB:
    return fmt::format("{} = MEM_READ8(ctx, {} + {});", rt, s32(base), offset);
  case InstrId::LBU:
    return fmt::format("{} = (uint8_t)MEM_READ8(ctx, {} + {});", rt, s32(base),
                       offset);
  case InstrId::LH:
    return fmt::format("{} = MEM_READ16(ctx, {} + {});", rt, s32(base), offset);
  case InstrId::LHU:
    return fmt::format("{} = (uint16_t)MEM_READ16(ctx, {} + {});", rt,
                       s32(base), offset);
  case InstrId::LW:
    return fmt::format("{} = MEM_READ32(ctx, {} + {});", rt, s32(base), offset);
  case InstrId::LWL:
    return fmt::format("{} = DO_LWL(ctx, {}, {} + {});", rt, rt, s32(base),
                       offset);
  case InstrId::LWR:
    return fmt::format("{} = DO_LWR(ctx, {}, {} + {});", rt, rt, s32(base),
                       offset);
  default:
    return fmt::format("// UNKNOWN LOAD: {}", MipsDecoder::instrName(inst.id));
  }
}

// ─── Store Emitter ───────────────────────────────────────

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

// ─── Branch Emitter ──────────────────────────────────────

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

// ─── Jump Emitter ────────────────────────────────────────

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

// ─── COP0 Emitter ────────────────────────────────────────

std::string InstructionEmitter::emitCOP0(const Instruction &inst) const {
  switch (inst.id) {
  case InstrId::MFC0:
    return fmt::format("{} = ctx->cop0[{}]; // {}", reg(inst.rt), inst.rd,
                       cop0Name(inst.rd));
  case InstrId::MTC0:
    return fmt::format("ctx->cop0[{}] = {}; // {}", inst.rd, reg(inst.rt),
                       cop0Name(inst.rd));
  case InstrId::RFE:
    return "COP0_RFE(ctx);";
  default:
    return fmt::format("// UNKNOWN COP0: {}", MipsDecoder::instrName(inst.id));
  }
}

// ─── GTE Register Move Emitter ──────────────────────────

std::string InstructionEmitter::emitGTEMove(const Instruction &inst) const {
  switch (inst.id) {
  case InstrId::MFC2:
    return fmt::format("{} = ctx->cop2d[{}];", reg(inst.rt), inst.rd);
  case InstrId::MTC2:
    return fmt::format("ctx->cop2d[{}] = {};", inst.rd, reg(inst.rt));
  case InstrId::CFC2:
    return fmt::format("{} = ctx->cop2c[{}];", reg(inst.rt), inst.rd);
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

// ─── System Emitter ──────────────────────────────────────

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

// ─── Main Dispatch ───────────────────────────────────────

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

// ─── Function Emitter ────────────────────────────────────

std::string InstructionEmitter::emitFunction(const RecompFunction &func) const {
  std::string result;

  // Function signature
  result += fmt::format("void {}(uint8_t* rdram, recomp_context* ctx) {{\n",
                        func.name);

  uint32_t pc = func.address;
  size_t numInstr = func.instructions.size();
  std::vector<uint32_t> oob_targets;

  for (size_t i = 0; i < numInstr; ++i) {
    uint32_t addr = func.address + static_cast<uint32_t>(i * 4);

    // Emit label if this address is a branch target
    if (!func.isLabelTarget.empty() && i < func.isLabelTarget.size() &&
        func.isLabelTarget[i]) {
      result += fmt::format("{}:\n", label(addr));
    }

    Instruction inst = MipsDecoder::decode(func.instructions[i]);

    if (inst.category == InstrCategory::Branch) {
      uint32_t target = inst.branchTarget(addr);
      if (target < func.address || target >= func.address + func.size) {
        oob_targets.push_back(target);
      }
    } else if (inst.category == InstrCategory::Jump && inst.id == InstrId::J) {
      uint32_t target = inst.jumpTarget(addr);
      if (target < func.address || target >= func.address + func.size) {
        oob_targets.push_back(target);
      }
    }

    std::string code = emitInstruction(inst, addr);

    // Handle branch delay slots: if this instruction has a delay slot,
    // emit the next instruction BEFORE the branch/jump
    if (inst.hasBranchDelaySlot() && i + 1 < numInstr) {
      Instruction delayInst = MipsDecoder::decode(func.instructions[i + 1]);
      std::string delayCode = emitInstruction(delayInst, addr + 4);
      result += fmt::format("    {} // delay slot\n", delayCode);
      result += fmt::format("    {}\n", code);
      ++i; // Skip delay slot instruction
    } else {
      result += fmt::format("    {}\n", code);
    }
  }

  for (uint32_t target : oob_targets) {
    result += fmt::format("{}:\n", label(target));
    if (m_resolver) {
      result +=
          fmt::format("    {}(rdram, ctx); return;\n", m_resolver(target));
    } else {
      result +=
          fmt::format("    JUMP_INDIRECT(ctx, 0x{:08X}); return;\n", target);
    }
  }

  result += "}\n";
  return result;
}

} // namespace ps1recomp
