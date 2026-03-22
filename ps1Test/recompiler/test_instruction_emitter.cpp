// Tests for ps1Recomp — Instruction Emitter
// Validates MIPS I → C++ translation

#include <gtest/gtest.h>
#include <ps1recomp/instruction_emitter.h>
#include <ps1recomp/mips_decoder.h>

using namespace ps1recomp;

// ──────────────────────────────────────────
// Encoding helpers (same as decoder tests)
// ──────────────────────────────────────────

static constexpr uint32_t encR(uint8_t op, uint8_t rs, uint8_t rt, uint8_t rd,
                               uint8_t shamt, uint8_t funct) {
  return (uint32_t(op) << 26) | (uint32_t(rs) << 21) | (uint32_t(rt) << 16) |
         (uint32_t(rd) << 11) | (uint32_t(shamt) << 6) | uint32_t(funct);
}
static constexpr uint32_t encI(uint8_t op, uint8_t rs, uint8_t rt,
                               uint16_t imm) {
  return (uint32_t(op) << 26) | (uint32_t(rs) << 21) | (uint32_t(rt) << 16) |
         uint32_t(imm);
}
static constexpr uint32_t encJ(uint8_t op, uint32_t target) {
  return (uint32_t(op) << 26) | (target & 0x03FFFFFF);
}

static InstructionEmitter makeEmitter() {
  InstructionEmitter emitter;
  emitter.setFuncResolver([](uint32_t addr) -> std::string {
    if (addr == 0x80020000)
      return "GameUpdate";
    if (addr == 0x80030000)
      return "RenderFrame";
    return "";
  });
  return emitter;
}

// ──────────────────────────────────────────
// ALU Tests
// ──────────────────────────────────────────

TEST(InstructionEmitter, ALU_ADDU) {
  auto emitter = makeEmitter();
  // ADDU $v0, $a0, $a1
  auto inst = MipsDecoder::decode(encR(0, 4, 5, 2, 0, 0x21));
  auto code = emitter.emitInstruction(inst, 0x80010000);
  EXPECT_NE(code.find("ctx->r2"), std::string::npos);
  EXPECT_NE(code.find("ctx->r4"), std::string::npos);
  EXPECT_NE(code.find("ctx->r5"), std::string::npos);
  EXPECT_NE(code.find("+"), std::string::npos);
}

TEST(InstructionEmitter, ALU_ADDIU) {
  auto emitter = makeEmitter();
  // ADDIU $sp, $sp, -32
  auto inst =
      MipsDecoder::decode(encI(0x09, 29, 29, static_cast<uint16_t>(-32)));
  auto code = emitter.emitInstruction(inst, 0x80010000);
  EXPECT_NE(code.find("ctx->r29"), std::string::npos);
  EXPECT_NE(code.find("-32"), std::string::npos);
}

TEST(InstructionEmitter, ALU_LUI) {
  auto emitter = makeEmitter();
  // LUI $v0, 0x8001
  auto inst = MipsDecoder::decode(encI(0x0F, 0, 2, 0x8001));
  auto code = emitter.emitInstruction(inst, 0x80010000);
  EXPECT_NE(code.find("ctx->r2"), std::string::npos);
  EXPECT_NE(code.find("80010000"), std::string::npos);
}

TEST(InstructionEmitter, ALU_BitwiseOps) {
  auto emitter = makeEmitter();

  // AND $v0, $a0, $a1
  auto code = emitter.emitInstruction(
      MipsDecoder::decode(encR(0, 4, 5, 2, 0, 0x24)), 0);
  EXPECT_NE(code.find("&"), std::string::npos);

  // OR
  code = emitter.emitInstruction(MipsDecoder::decode(encR(0, 4, 5, 2, 0, 0x25)),
                                 0);
  EXPECT_NE(code.find("|"), std::string::npos);

  // XOR
  code = emitter.emitInstruction(MipsDecoder::decode(encR(0, 4, 5, 2, 0, 0x26)),
                                 0);
  EXPECT_NE(code.find("^"), std::string::npos);
}

// ──────────────────────────────────────────
// Write-to-$zero Optimization
// ──────────────────────────────────────────

TEST(InstructionEmitter, SkipsWritesToZero) {
  auto emitter = makeEmitter();

  // ADDU $zero, $a0, $a1 — should be optimized away
  auto inst = MipsDecoder::decode(encR(0, 4, 5, 0, 0, 0x21));
  auto code = emitter.emitInstruction(inst, 0x80010000);
  EXPECT_NE(code.find("NOP"), std::string::npos);

  // LW $zero, 0($sp) — load into $zero, optimized away
  inst = MipsDecoder::decode(encI(0x23, 29, 0, 0));
  code = emitter.emitInstruction(inst, 0x80010000);
  EXPECT_NE(code.find("NOP"), std::string::npos);
}

// ──────────────────────────────────────────
// Shift Tests
// ──────────────────────────────────────────

TEST(InstructionEmitter, Shifts) {
  auto emitter = makeEmitter();

  // SLL $v0, $a0, 4
  auto code = emitter.emitInstruction(
      MipsDecoder::decode(encR(0, 0, 4, 2, 4, 0x00)), 0);
  EXPECT_NE(code.find("<<"), std::string::npos);
  EXPECT_NE(code.find("4"), std::string::npos);

  // SRL $v0, $a0, 5
  code = emitter.emitInstruction(MipsDecoder::decode(encR(0, 0, 4, 2, 5, 0x02)),
                                 0);
  EXPECT_NE(code.find(">>"), std::string::npos);
}

// ──────────────────────────────────────────
// Multiply / Divide Tests
// ──────────────────────────────────────────

TEST(InstructionEmitter, MulDiv) {
  auto emitter = makeEmitter();

  // MULT $a0, $a1
  auto code = emitter.emitInstruction(
      MipsDecoder::decode(encR(0, 4, 5, 0, 0, 0x18)), 0);
  EXPECT_NE(code.find("ctx->lo"), std::string::npos);
  EXPECT_NE(code.find("ctx->hi"), std::string::npos);

  // MFLO $v0
  code = emitter.emitInstruction(MipsDecoder::decode(encR(0, 0, 0, 2, 0, 0x12)),
                                 0);
  EXPECT_NE(code.find("ctx->r2"), std::string::npos);
  EXPECT_NE(code.find("ctx->lo"), std::string::npos);

  // DIV — should check for zero
  code = emitter.emitInstruction(MipsDecoder::decode(encR(0, 4, 5, 0, 0, 0x1A)),
                                 0);
  EXPECT_NE(code.find("!= 0"), std::string::npos);
}

// ──────────────────────────────────────────
// Load/Store Tests
// ──────────────────────────────────────────

TEST(InstructionEmitter, LoadWord) {
  auto emitter = makeEmitter();
  // LW $v0, 16($sp)
  auto inst = MipsDecoder::decode(encI(0x23, 29, 2, 16));
  auto code = emitter.emitInstruction(inst, 0x80010000);
  EXPECT_NE(code.find("MEM_READ32"), std::string::npos);
  EXPECT_NE(code.find("ctx->r2"), std::string::npos);
  EXPECT_NE(code.find("16"), std::string::npos);
}

TEST(InstructionEmitter, StoreWord) {
  auto emitter = makeEmitter();
  // SW $v0, 16($sp)
  auto inst = MipsDecoder::decode(encI(0x2B, 29, 2, 16));
  auto code = emitter.emitInstruction(inst, 0x80010000);
  EXPECT_NE(code.find("MEM_WRITE32"), std::string::npos);
}

TEST(InstructionEmitter, LoadByte) {
  auto emitter = makeEmitter();
  auto code =
      emitter.emitInstruction(MipsDecoder::decode(encI(0x20, 4, 2, 0)), 0);
  EXPECT_NE(code.find("MEM_READ8"), std::string::npos);
}

TEST(InstructionEmitter, UnalignedLoadStore) {
  auto emitter = makeEmitter();

  auto code =
      emitter.emitInstruction(MipsDecoder::decode(encI(0x22, 4, 2, 0)), 0);
  EXPECT_NE(code.find("DO_LWL"), std::string::npos);

  code = emitter.emitInstruction(MipsDecoder::decode(encI(0x26, 4, 2, 0)), 0);
  EXPECT_NE(code.find("DO_LWR"), std::string::npos);
}

// ──────────────────────────────────────────
// Branch Tests
// ──────────────────────────────────────────

TEST(InstructionEmitter, BranchBEQ) {
  auto emitter = makeEmitter();
  // BEQ $a0, $a1, +4
  auto inst = MipsDecoder::decode(encI(0x04, 4, 5, 4));
  auto code = emitter.emitInstruction(inst, 0x80010000);
  EXPECT_NE(code.find("=="), std::string::npos);
  EXPECT_NE(code.find("goto"), std::string::npos);
  EXPECT_NE(code.find("L_"), std::string::npos);
}

TEST(InstructionEmitter, BranchBNE) {
  auto emitter = makeEmitter();
  auto code = emitter.emitInstruction(MipsDecoder::decode(encI(0x05, 4, 5, 4)),
                                      0x80010000);
  EXPECT_NE(code.find("!="), std::string::npos);
  EXPECT_NE(code.find("goto"), std::string::npos);
}

TEST(InstructionEmitter, BranchComparisons) {
  auto emitter = makeEmitter();

  // BLEZ
  auto code = emitter.emitInstruction(MipsDecoder::decode(encI(0x06, 4, 0, 4)),
                                      0x80010000);
  EXPECT_NE(code.find("<= 0"), std::string::npos);

  // BGTZ
  code = emitter.emitInstruction(MipsDecoder::decode(encI(0x07, 4, 0, 4)),
                                 0x80010000);
  EXPECT_NE(code.find("> 0"), std::string::npos);
}

// ──────────────────────────────────────────
// Jump Tests
// ──────────────────────────────────────────

TEST(InstructionEmitter, Jump_J) {
  auto emitter = makeEmitter();
  auto inst = MipsDecoder::decode(encJ(0x02, 0x00004000));
  auto code = emitter.emitInstruction(inst, 0x80010000);
  EXPECT_NE(code.find("goto"), std::string::npos);
}

TEST(InstructionEmitter, Jump_JAL) {
  auto emitter = makeEmitter();
  // JAL to GameUpdate (at 0x80020000 = target 0x00008000)
  auto inst = MipsDecoder::decode(encJ(0x03, 0x00008000));
  auto code = emitter.emitInstruction(inst, 0x80010000);
  EXPECT_NE(code.find("GameUpdate"), std::string::npos);
  EXPECT_NE(code.find("rdram"), std::string::npos);
  EXPECT_NE(code.find("ctx"), std::string::npos);
}

TEST(InstructionEmitter, Jump_JR_RA) {
  auto emitter = makeEmitter();
  // JR $ra — should emit return
  auto inst = MipsDecoder::decode(encR(0, 31, 0, 0, 0, 0x08));
  auto code = emitter.emitInstruction(inst, 0x80010000);
  EXPECT_NE(code.find("return"), std::string::npos);
}

TEST(InstructionEmitter, Jump_JR_NonRA) {
  auto emitter = makeEmitter();
  // JR $t0 — indirect jump (not return)
  auto inst = MipsDecoder::decode(encR(0, 8, 0, 0, 0, 0x08));
  auto code = emitter.emitInstruction(inst, 0x80010000);
  EXPECT_NE(code.find("JUMP_INDIRECT"), std::string::npos);
}

// ──────────────────────────────────────────
// COP0 Tests
// ──────────────────────────────────────────

TEST(InstructionEmitter, COP0) {
  auto emitter = makeEmitter();

  // MFC0 $v0, SR
  auto code = emitter.emitInstruction(
      MipsDecoder::decode(encR(0x10, 0x00, 2, 12, 0, 0)), 0);
  EXPECT_NE(code.find("cop0"), std::string::npos);
  EXPECT_NE(code.find("SR"), std::string::npos);

  // MTC0 $v0, SR
  code = emitter.emitInstruction(
      MipsDecoder::decode(encR(0x10, 0x04, 2, 12, 0, 0)), 0);
  EXPECT_NE(code.find("cop0"), std::string::npos);
}

// ──────────────────────────────────────────
// GTE Tests
// ──────────────────────────────────────────

TEST(InstructionEmitter, GTE_RegisterMoves) {
  auto emitter = makeEmitter();

  // MFC2 $v0, r5
  auto code = emitter.emitInstruction(
      MipsDecoder::decode(encR(0x12, 0x00, 2, 5, 0, 0)), 0);
  EXPECT_NE(code.find("cop2d"), std::string::npos);
  EXPECT_NE(code.find("ctx->r2"), std::string::npos);

  // MTC2 $v0, r5
  code = emitter.emitInstruction(
      MipsDecoder::decode(encR(0x12, 0x04, 2, 5, 0, 0)), 0);
  EXPECT_NE(code.find("cop2d"), std::string::npos);
}

TEST(InstructionEmitter, GTE_Commands) {
  auto emitter = makeEmitter();

  // RTPS command
  uint32_t raw = (0x12u << 26) | (1u << 25) | 0x01;
  auto code = emitter.emitInstruction(MipsDecoder::decode(raw), 0);
  EXPECT_NE(code.find("ps1::GTE::RTPS"), std::string::npos);

  // NCLIP command
  raw = (0x12u << 26) | (1u << 25) | 0x06;
  code = emitter.emitInstruction(MipsDecoder::decode(raw), 0);
  EXPECT_NE(code.find("ps1::GTE::NCLIP"), std::string::npos);
}

// ──────────────────────────────────────────
// NOP Handling
// ──────────────────────────────────────────

TEST(InstructionEmitter, NOP) {
  auto emitter = makeEmitter();
  auto code = emitter.emitInstruction(MipsDecoder::decode(0), 0);
  EXPECT_NE(code.find("NOP"), std::string::npos);
}

// ──────────────────────────────────────────
// Function Emission
// ──────────────────────────────────────────

TEST(InstructionEmitter, EmitFunction) {
  auto emitter = makeEmitter();

  RecompFunction func;
  func.name = "test_func";
  func.address = 0x80010000;
  func.instructions = {
      encI(0x09, 29, 29, static_cast<uint16_t>(-32)), // ADDIU $sp, $sp, -32
      encI(0x2B, 29, 31, 28),                         // SW $ra, 28($sp)
      encJ(0x03, 0x00008000),                         // JAL GameUpdate
      0x00000000,                                     // NOP (delay slot)
      encI(0x23, 29, 31, 28),                         // LW $ra, 28($sp)
      encI(0x09, 29, 29, 32),                         // ADDIU $sp, $sp, 32
      encR(0, 31, 0, 0, 0, 0x08),                     // JR $ra
      0x00000000                                      // NOP (delay slot)
  };
  func.isLabelTarget.resize(func.instructions.size(), false);
  func.size = func.instructions.size() * 4;

  auto code = emitter.emitFunction(func);

  // Should contain function signature
  EXPECT_NE(code.find("test_func"), std::string::npos);
  EXPECT_NE(code.find("rdram"), std::string::npos);
  EXPECT_NE(code.find("recomp_context"), std::string::npos);

  // Should contain stack operations
  EXPECT_NE(code.find("-32"), std::string::npos);

  // Should contain function call
  EXPECT_NE(code.find("GameUpdate"), std::string::npos);

  // Should contain return
  EXPECT_NE(code.find("return"), std::string::npos);

  // Should handle delay slots
  EXPECT_NE(code.find("delay slot"), std::string::npos);
}

TEST(InstructionEmitter, EmitFunctionWithBranch) {
  auto emitter = makeEmitter();

  RecompFunction func;
  func.name = "branch_test";
  func.address = 0x80010000;
  func.instructions = {
      encI(0x04, 4, 0, 2),        // BEQ $a0, $zero, +2
      0x00000000,                 // NOP (delay slot)
      encI(0x09, 0, 2, 1),        // ADDIU $v0, $zero, 1
      encR(0, 31, 0, 0, 0, 0x08), // JR $ra
      0x00000000                  // NOP (delay slot)
  };
  func.isLabelTarget = {false, false, false, false, false};
  func.isLabelTarget.resize(func.instructions.size(), false);
  // The BEQ target would be instruction [3] (PC + 4 + 2*4 = addr+12)
  // We can mark it manually
  if (func.instructions.size() > 3)
    func.isLabelTarget[3] = true;
  func.size = func.instructions.size() * 4;

  auto code = emitter.emitFunction(func);

  // Should contain branch condition
  EXPECT_NE(code.find("=="), std::string::npos);
  EXPECT_NE(code.find("goto"), std::string::npos);

  // Should contain label for branch target
  EXPECT_NE(code.find("L_"), std::string::npos);
}

// ──────────────────────────────────────────
// Helpers
// ──────────────────────────────────────────

TEST(InstructionEmitter, RegHelper) {
  EXPECT_EQ(InstructionEmitter::reg(0), "(int32_t)0");
  EXPECT_EQ(InstructionEmitter::reg(2), "ctx->r2");
  EXPECT_EQ(InstructionEmitter::reg(29), "ctx->r29");
  EXPECT_EQ(InstructionEmitter::reg(31), "ctx->r31");
}

TEST(InstructionEmitter, LabelHelper) {
  EXPECT_EQ(InstructionEmitter::label(0x80010040), "L_80010040");
}
