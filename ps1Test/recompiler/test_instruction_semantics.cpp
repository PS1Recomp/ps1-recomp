// Tests: Instruction emitter and decoder.
// Checks that the emitted C++ carries the R3000A meaning of each instruction
// (psx-spx "CPU Specifications"): signedness of compares, shifts and
// multiplies, zero- vs sign-extended immediates, link addresses, delay slots.
// Assertions look for the semantically relevant fragment, not whole strings.

#include <gtest/gtest.h>
#include <ps1recomp/instruction_emitter.h>
#include <ps1recomp/mips_decoder.h>

#include <string>

using namespace ps1recomp;

namespace {

constexpr uint32_t encR(uint8_t op, uint8_t rs, uint8_t rt, uint8_t rd,
                        uint8_t shamt, uint8_t funct) {
  return (uint32_t(op) << 26) | (uint32_t(rs) << 21) | (uint32_t(rt) << 16) |
         (uint32_t(rd) << 11) | (uint32_t(shamt) << 6) | uint32_t(funct);
}
constexpr uint32_t encI(uint8_t op, uint8_t rs, uint8_t rt, uint16_t imm) {
  return (uint32_t(op) << 26) | (uint32_t(rs) << 21) | (uint32_t(rt) << 16) |
         uint32_t(imm);
}
constexpr uint32_t encJ(uint8_t op, uint32_t target) {
  return (uint32_t(op) << 26) | (target & 0x03FFFFFF);
}
constexpr uint32_t kNop = 0;
constexpr uint32_t kJrRa = encR(0, 31, 0, 0, 0, 0x08);
constexpr uint32_t kPc = 0x80010000;

std::string emit(uint32_t raw, uint32_t pc = kPc) {
  InstructionEmitter e;
  return e.emitInstruction(MipsDecoder::decode(raw), pc);
}

bool has(const std::string &code, const std::string &frag) {
  return code.find(frag) != std::string::npos;
}

RecompFunction makeFunc(std::vector<uint32_t> words, uint32_t addr = kPc) {
  RecompFunction f;
  f.name = "f";
  f.address = addr;
  f.instructions = std::move(words);
  f.isLabelTarget.assign(f.instructions.size(), false);
  f.size = static_cast<uint32_t>(f.instructions.size() * 4);
  return f;
}

std::string emitFn(const RecompFunction &f) {
  InstructionEmitter e;
  return e.emitFunction(f);
}

} // namespace

// ALU

TEST(InstructionSemantics, SubWrapsThroughUnsignedArithmetic) {
  // SUBU $v0, $a0, $a1: modular, so the subtraction must be done unsigned.
  auto code = emit(encR(0, 4, 5, 2, 0, 0x23));
  EXPECT_TRUE(has(code, "ctx->r2 = "));
  EXPECT_TRUE(has(code, "(uint32_t)(ctx->r4) - (uint32_t)(ctx->r5)")) << code;
}

TEST(InstructionSemantics, NorIsComplementOfOr) {
  auto code = emit(encR(0, 4, 5, 2, 0, 0x27));
  EXPECT_TRUE(has(code, "~(ctx->r4 | ctx->r5)")) << code;
}

TEST(InstructionSemantics, XorUsesBothSources) {
  auto code = emit(encR(0, 4, 5, 2, 0, 0x26));
  EXPECT_TRUE(has(code, "ctx->r4 ^ ctx->r5")) << code;
}

TEST(InstructionSemantics, SltComparesSignedAndSltuUnsigned) {
  auto slt = emit(encR(0, 4, 5, 2, 0, 0x2A));
  EXPECT_TRUE(has(slt, "(int32_t)(ctx->r4) < (int32_t)(ctx->r5)")) << slt;
  auto sltu = emit(encR(0, 4, 5, 2, 0, 0x2B));
  EXPECT_TRUE(has(sltu, "(uint32_t)(ctx->r4) < (uint32_t)(ctx->r5)")) << sltu;
}

TEST(InstructionSemantics, AddiSignExtendsItsImmediate) {
  // ADDI $t0, $a0, 0x8000 -> rs + (-32768)
  auto code = emit(encI(0x08, 4, 8, 0x8000));
  EXPECT_TRUE(has(code, "ctx->r8 = "));
  EXPECT_TRUE(has(code, "(uint32_t)(int32_t)-32768")) << code;
}

TEST(InstructionSemantics, LogicalImmediatesAreZeroExtended) {
  // psx-spx: andi/ori/xori take imm as 0000h..FFFFh, never negative.
  auto andi = emit(encI(0x0C, 4, 2, 0xFFFF));
  EXPECT_TRUE(has(andi, "ctx->r4 & 0xFFFF")) << andi;
  auto ori = emit(encI(0x0D, 4, 2, 0x8000));
  EXPECT_TRUE(has(ori, "ctx->r4 | 0x8000")) << ori;
  auto xori = emit(encI(0x0E, 4, 2, 0x8001));
  EXPECT_TRUE(has(xori, "ctx->r4 ^ 0x8001")) << xori;
}

TEST(InstructionSemantics, SltiComparesSignedAgainstSignExtendedImmediate) {
  auto code = emit(encI(0x0A, 4, 2, 0xFFFF)); // SLTI $v0, $a0, -1
  EXPECT_TRUE(has(code, "(int32_t)(ctx->r4) < -1")) << code;
}

TEST(InstructionSemantics, SltiuSignExtendsThenComparesUnsigned) {
  // psx-spx: sltiu imm is sign-extended (-8000h..+7FFFh) but compared unsigned.
  auto code = emit(encI(0x0B, 4, 2, 0xFFFF));
  EXPECT_TRUE(has(code, "(uint32_t)(ctx->r4) < 4294967295")) << code;
}

TEST(InstructionSemantics, LuiPlacesImmediateInUpperHalf) {
  auto code = emit(encI(0x0F, 0, 2, 0x1F80));
  EXPECT_TRUE(has(code, "ctx->r2 = 0x1F800000")) << code;
}

// Shifts

TEST(InstructionSemantics, SraIsArithmeticAndSrlIsLogical) {
  auto sra = emit(encR(0, 0, 4, 2, 3, 0x03));
  EXPECT_TRUE(has(sra, "(int32_t)(ctx->r4) >> 3")) << sra;
  auto srl = emit(encR(0, 0, 4, 2, 3, 0x02));
  EXPECT_TRUE(has(srl, "(uint32_t)(ctx->r4) >> 3")) << srl;
}

TEST(InstructionSemantics, VariableShiftsUseLowFiveBitsOfRs) {
  auto sllv = emit(encR(0, 5, 4, 2, 0, 0x04));
  EXPECT_TRUE(has(sllv, "(uint32_t)(ctx->r4) << (ctx->r5 & 31)")) << sllv;
  auto srlv = emit(encR(0, 5, 4, 2, 0, 0x06));
  EXPECT_TRUE(has(srlv, "(uint32_t)(ctx->r4) >> (ctx->r5 & 31)")) << srlv;
  auto srav = emit(encR(0, 5, 4, 2, 0, 0x07));
  EXPECT_TRUE(has(srav, "(int32_t)(ctx->r4) >> (ctx->r5 & 31)")) << srav;
}

// Multiply / divide

TEST(InstructionSemantics, MultIsSigned64BitAndMultuUnsigned) {
  auto mult = emit(encR(0, 4, 5, 0, 0, 0x18));
  EXPECT_TRUE(has(mult, "(int64_t)(int32_t)(ctx->r4) * (int64_t)(int32_t)(ctx->r5)"))
      << mult;
  EXPECT_TRUE(has(mult, "ctx->hi = (int32_t)(result >> 32)")) << mult;
  auto multu = emit(encR(0, 4, 5, 0, 0, 0x19));
  EXPECT_TRUE(has(multu, "(uint64_t)(uint32_t)(ctx->r4) * (uint64_t)(uint32_t)(ctx->r5)"))
      << multu;
  EXPECT_TRUE(has(multu, "ctx->lo = (int32_t)result")) << multu;
}

// Division goes through the runtime helpers, which define the hardware
// results for a zero divisor and 80000000h / -1 (tested in test_runtime_macros).
TEST(InstructionSemantics, DivAndDivuUseTheRuntimeHelpers) {
  EXPECT_EQ(emit(encR(0, 4, 5, 0, 0, 0x1A)), "DO_DIV(ctx, ctx->r4, ctx->r5);");
  EXPECT_EQ(emit(encR(0, 4, 5, 0, 0, 0x1B)), "DO_DIVU(ctx, ctx->r4, ctx->r5);");
}

// Shifts write rd.  Reading $zero is legal and still clears rd: the $zero
// write filter must look at rd, not at rt as for I-type ALU ops.
TEST(InstructionSemantics, ShiftOfZeroStillWritesRd) {
  EXPECT_TRUE(has(emit(encR(0, 0, 0, 8, 2, 0x00)), "ctx->r8 = ")); // sll $t0, $zero, 2
  EXPECT_TRUE(has(emit(encR(0, 4, 0, 8, 0, 0x04)), "ctx->r8 = ")); // sllv $t0, $zero, $a0
}

TEST(InstructionSemantics, MthiMtloWriteHiLoFromRs) {
  EXPECT_EQ(emit(encR(0, 7, 0, 0, 0, 0x11)), "ctx->hi = ctx->r7;");
  EXPECT_EQ(emit(encR(0, 7, 0, 0, 0, 0x13)), "ctx->lo = ctx->r7;");
}

TEST(InstructionSemantics, MfhiMfloCopyHiLoIntoRd) {
  EXPECT_EQ(emit(encR(0, 0, 0, 9, 0, 0x10)), "ctx->r9 = ctx->hi;");
  EXPECT_EQ(emit(encR(0, 0, 0, 9, 0, 0x12)), "ctx->r9 = ctx->lo;");
}

TEST(InstructionSemantics, MfhiIntoZeroIsDiscarded) {
  EXPECT_TRUE(has(emit(encR(0, 0, 0, 0, 0, 0x10)), "NOP"));
}

// Loads / stores

TEST(InstructionSemantics, LoadAddressIsSignedBasePlusSignedOffset) {
  auto code = emit(encI(0x23, 29, 2, 0xFFFC)); // LW $v0, -4($sp)
  EXPECT_TRUE(has(code, "MEM_READ32(ctx, (int32_t)(ctx->r29) + -4)")) << code;
}

TEST(InstructionSemantics, LwlLwrMergeIntoTheOldTargetValue) {
  // psx-spx: LWL/LWR only replace part of rt, so the old rt is an input.
  auto lwl = emit(encI(0x22, 4, 2, 3));
  EXPECT_TRUE(has(lwl, "ctx->r2 = DO_LWL(ctx, ctx->r2, (int32_t)(ctx->r4) + 3)"))
      << lwl;
  auto lwr = emit(encI(0x26, 4, 2, 0));
  EXPECT_TRUE(has(lwr, "ctx->r2 = DO_LWR(ctx, ctx->r2, (int32_t)(ctx->r4) + 0)"))
      << lwr;
}

TEST(InstructionSemantics, ByteAndHalfStoresUseMatchingWidth) {
  auto sb = emit(encI(0x28, 4, 2, 1));
  EXPECT_TRUE(has(sb, "MEM_WRITE8(ctx, (int32_t)(ctx->r4) + 1, ctx->r2)")) << sb;
  auto sh = emit(encI(0x29, 4, 2, 2));
  EXPECT_TRUE(has(sh, "MEM_WRITE16(ctx, (int32_t)(ctx->r4) + 2, ctx->r2)")) << sh;
}

TEST(InstructionSemantics, StoreOfZeroRegisterWritesZero) {
  // A store *from* $zero is real work, unlike a load *into* $zero.
  auto sw = emit(encI(0x2B, 29, 0, 8));
  EXPECT_TRUE(has(sw, "MEM_WRITE32(ctx, (int32_t)(ctx->r29) + 8, (int32_t)0)")) << sw;
  EXPECT_TRUE(has(emit(encI(0x23, 29, 0, 8)), "NOP"));
}

// Branches and jumps

TEST(InstructionSemantics, ZeroCompareBranchesAreSignedAndTargetPcPlus4) {
  // offset 3 -> 0x80010000 + 4 + 12
  auto blez = emit(encI(0x06, 4, 0, 3));
  EXPECT_TRUE(has(blez, "if ((int32_t)(ctx->r4) <= 0) goto L_80010010;")) << blez;
  auto bgtz = emit(encI(0x07, 4, 0, 3));
  EXPECT_TRUE(has(bgtz, "if ((int32_t)(ctx->r4) > 0) goto L_80010010;")) << bgtz;
  auto bltz = emit(encI(0x01, 4, 0x00, 3));
  EXPECT_TRUE(has(bltz, "if ((int32_t)(ctx->r4) < 0) goto L_80010010;")) << bltz;
  auto bgez = emit(encI(0x01, 4, 0x01, 3));
  EXPECT_TRUE(has(bgez, "if ((int32_t)(ctx->r4) >= 0) goto L_80010010;")) << bgez;
}

TEST(InstructionSemantics, BackwardBranchTargetsEarlierAddress) {
  auto code = emit(encI(0x05, 4, 5, 0xFFFF)); // BNE ..., -1 -> branch to self
  EXPECT_TRUE(has(code, "goto L_80010000;")) << code;
}

TEST(InstructionSemantics, LinkingBranchesSetRaEvenWhenNotTaken) {
  // psx-spx: bltzal/bgezal write $ra = PC+8 whether or not they branch.
  auto bltzal = emit(encI(0x01, 4, 0x10, 1));
  EXPECT_TRUE(has(bltzal, "ctx->r31 = 0x80010008; if ((int32_t)(ctx->r4) < 0)"))
      << bltzal;
  auto bgezal = emit(encI(0x01, 4, 0x11, 1));
  EXPECT_TRUE(has(bgezal, "ctx->r31 = 0x80010008; if ((int32_t)(ctx->r4) >= 0)"))
      << bgezal;
}

TEST(InstructionSemantics, JKeepsUpperFourBitsOfPc) {
  auto code = emit(encJ(0x02, 0x0004000), 0x80010000);
  EXPECT_EQ(code, "goto L_80010000;");
}

TEST(InstructionSemantics, JalWithoutResolverCallsDefaultNameAndLinks) {
  auto code = emit(encJ(0x03, 0x0010000)); // 0x80040000
  EXPECT_EQ(code, "ctx->r31 = 0x80010008; func_80040000(rdram, ctx);");
}

TEST(InstructionSemantics, JalrLinksIntoRdAndCallsThroughRs) {
  auto code = emit(encR(0, 25, 0, 31, 0, 0x09)); // JALR $ra, $t9
  EXPECT_TRUE(has(code, "ctx->r31 = 0x80010008;")) << code;
  EXPECT_TRUE(has(code, "CALL_INDIRECT_AT(ctx, ctx->r25, 0x80010000u)")) << code;
}

// System / coprocessors

TEST(InstructionSemantics, SyscallAndBreakTrapToRuntime) {
  EXPECT_EQ(emit(encR(0, 0, 0, 0, 0, 0x0C)), "SYSCALL(ctx);");
  EXPECT_EQ(emit(encR(0, 0, 0, 0, 0, 0x0D)), "BREAK(ctx);");
}

TEST(InstructionSemantics, Cop0MovesAddressTheNamedRegister) {
  auto mtc0 = emit(encR(0x10, 0x04, 8, 12, 0, 0)); // MTC0 $t0, SR
  EXPECT_TRUE(has(mtc0, "ctx->cop0[12] = ctx->r8;")) << mtc0;
  EXPECT_TRUE(has(mtc0, "SR")) << mtc0;
  auto mfc0 = emit(encR(0x10, 0x00, 2, 13, 0, 0)); // MFC0 $v0, Cause
  EXPECT_TRUE(has(mfc0, "ctx->r2 = ctx->cop0[13]")) << mfc0;
  EXPECT_TRUE(has(mfc0, "Cause")) << mfc0;
}

TEST(InstructionSemantics, RfeRestoresInterruptStack) {
  EXPECT_EQ(emit(encR(0x10, 0x10, 0, 0, 0, 0x10)), "COP0_RFE(ctx);");
}

TEST(InstructionSemantics, Cop2ControlMovesAndMemoryTransfers) {
  auto cfc2 = emit(encR(0x12, 0x02, 2, 31, 0, 0)); // CFC2 $v0, FLAG
  EXPECT_TRUE(has(cfc2, "ctx->r2 = ctx->cop2c[31]")) << cfc2;
  auto ctc2 = emit(encR(0x12, 0x06, 8, 5, 0, 0)); // CTC2 $t0, TRX
  EXPECT_TRUE(has(ctc2, "ctx->cop2c[5] = ctx->r8;")) << ctc2;
  auto lwc2 = emit(encI(0x32, 4, 0, 8)); // LWC2 VXY0, 8($a0)
  EXPECT_TRUE(has(lwc2, "gte_write_data(ctx, 0, MEM_READ32(ctx, (int32_t)(ctx->r4) + 8))"))
      << lwc2;
  auto swc2 = emit(encI(0x3A, 4, 7, 0xFFF8)); // SWC2 OTZ, -8($a0)
  EXPECT_TRUE(has(swc2, "MEM_WRITE32(ctx, (int32_t)(ctx->r4) + -8, gte_read_data(ctx, 7))"))
      << swc2;
}

// Data moves go through the GTE so LZCR, SXYP and IRGB/ORGB side effects happen.
TEST(InstructionSemantics, Cop2DataMovesUseGteRegisterAccess) {
  auto mfc2 = emit(encR(0x12, 0x00, 2, 31, 0, 0)); // MFC2 $v0, LZCR
  EXPECT_TRUE(has(mfc2, "ctx->r2 = gte_read_data(ctx, 31)")) << mfc2;
  auto mtc2 = emit(encR(0x12, 0x04, 4, 30, 0, 0)); // MTC2 $a0, LZCS
  EXPECT_TRUE(has(mtc2, "gte_write_data(ctx, 30, ctx->r4);")) << mtc2;
}

TEST(InstructionSemantics, GteCommandPassesSfAndLmBits) {
  // psx-spx GTE command encoding: bit 19 = sf (shift), bit 10 = lm (limit).
  const uint32_t rtps = 0x4A000001u;
  EXPECT_TRUE(has(emit(rtps), "RTPS(ctx, false, false)"));
  EXPECT_TRUE(has(emit(rtps | (1u << 19)), "RTPS(ctx, true, false)"));
  EXPECT_TRUE(has(emit(rtps | (1u << 10)), "RTPS(ctx, false, true)"));
}

TEST(InstructionSemantics, MvmvaDecodesMatrixVectorAndTranslationFields) {
  // mx = bits 17-18, v = bits 15-16, cv = bits 13-14.
  const uint32_t mvmva =
      0x4A000012u | (2u << 17) | (1u << 15) | (3u << 13) | (1u << 19);
  auto code = emit(mvmva);
  EXPECT_TRUE(has(code, "MVMVA(ctx, 2, 1, 3, true, false)")) << code;
}

TEST(InstructionSemantics, InvalidWordBecomesCommentWithRawValue) {
  auto code = emit(0xFC000000u); // opcode 0x3F is unused on the R3000A
  EXPECT_EQ(code, "// INVALID: 0xFC000000");
}

TEST(InstructionSemantics, SubEmittersReportMisroutedInstructions) {
  InstructionEmitter e;
  auto load = MipsDecoder::decode(encI(0x23, 4, 2, 0));
  auto add = MipsDecoder::decode(encR(0, 4, 5, 2, 0, 0x21));
  EXPECT_TRUE(has(e.emitALU(load), "UNKNOWN ALU: LW"));
  EXPECT_TRUE(has(e.emitShift(add), "UNKNOWN SHIFT: ADDU"));
  EXPECT_TRUE(has(e.emitMulDiv(add), "UNKNOWN MULDIV: ADDU"));
  EXPECT_TRUE(has(e.emitLoad(add), "UNKNOWN LOAD: ADDU"));
  EXPECT_TRUE(has(e.emitStore(load), "UNKNOWN STORE: LW"));
  EXPECT_TRUE(has(e.emitBranch(add, 0), "UNKNOWN BRANCH: ADDU"));
  EXPECT_TRUE(has(e.emitJump(add, 0), "UNKNOWN JUMP: ADDU"));
  EXPECT_TRUE(has(e.emitCOP0(add), "UNKNOWN COP0: ADDU"));
  EXPECT_TRUE(has(e.emitGTEMove(add), "UNKNOWN GTE MOVE: ADDU"));
  EXPECT_TRUE(has(e.emitSystem(add), "UNKNOWN SYSTEM: ADDU"));
}

TEST(InstructionSemantics, AssignToZeroEvaluatesButDiscards) {
  EXPECT_EQ(InstructionEmitter::assignReg(0, "x"), "(void)(x); /* $zero */");
  EXPECT_EQ(InstructionEmitter::regWrite(0), "/* $zero discard */");
  EXPECT_EQ(InstructionEmitter::regWrite(9), "ctx->r9");
}

// Function emission: delay slots

TEST(InstructionSemantics, DelaySlotRunsBeforeTheBranchTransfer) {
  auto f = makeFunc({encI(0x04, 4, 5, 2),       // BEQ $a0, $a1, +2
                     encI(0x09, 0, 2, 7),       // ADDIU $v0, $zero, 7 (slot)
                     kNop, kJrRa, kNop});
  auto code = emitFn(f);
  auto slot = code.find("ctx->r2 = ");
  auto branch = code.find("if ((int32_t)(ctx->r4) == (int32_t)(ctx->r5))");
  ASSERT_NE(slot, std::string::npos) << code;
  ASSERT_NE(branch, std::string::npos) << code;
  EXPECT_LT(slot, branch);
}

TEST(InstructionSemantics, BranchReadsRsBeforeDelaySlotOverwritesIt) {
  // bne $a0, $zero, loop ; addiu $a0, $a0, -1 -- the compare sees the old $a0.
  auto f = makeFunc({encI(0x05, 4, 0, 2), encI(0x09, 4, 4, 0xFFFF), kNop,
                     kJrRa, kNop});
  auto code = emitFn(f);
  EXPECT_TRUE(has(code, "uint32_t _ds_r4 = ctx->r4;")) << code;
  EXPECT_TRUE(has(code, "if ((int32_t)(_ds_r4) != (int32_t)(")) << code;
}

TEST(InstructionSemantics, BeqSnapshotsRtAndLeavesLongerRegisterNamesAlone) {
  // beq $at, $t4 ; lw $at, 0($sp): only r1 is saved, ctx->r12 stays intact.
  auto f = makeFunc({encI(0x04, 12, 1, 2), encI(0x23, 29, 1, 0), kNop,
                     kJrRa, kNop});
  auto code = emitFn(f);
  EXPECT_TRUE(has(code, "uint32_t _ds_r1 = ctx->r1;")) << code;
  EXPECT_TRUE(has(code, "(int32_t)(ctx->r12) == (int32_t)(_ds_r1)")) << code;
}

TEST(InstructionSemantics, JrTargetIsReadBeforeDelaySlotLoadsIt) {
  auto f = makeFunc({encR(0, 25, 0, 0, 0, 0x08), // JR $t9
                     encI(0x23, 29, 25, 0)});    // LW $t9, 0($sp) (slot)
  auto code = emitFn(f);
  EXPECT_TRUE(has(code, "uint32_t _ds_r25 = ctx->r25;")) << code;
  EXPECT_TRUE(has(code, "JUMP_INDIRECT_AT(ctx, _ds_r25, 0x80010000u)")) << code;
}

TEST(InstructionSemantics, BackwardBranchYieldsForPendingCallbacks) {
  auto f = makeFunc({kNop, encI(0x05, 4, 0, 0xFFFE), kNop, kJrRa, kNop});
  auto code = emitFn(f);
  EXPECT_TRUE(has(code, "drainPendingCallbacks()")) << code;
  EXPECT_TRUE(has(code, "L_80010000:")) << code;
}

TEST(InstructionSemantics, BranchToItselfYields) {
  // `b .` (offset -1) is the tightest busy-wait; it must still yield.
  auto f = makeFunc({encI(0x05, 4, 0, 0xFFFF), kNop, kJrRa, kNop});
  EXPECT_TRUE(has(emitFn(f), "drainPendingCallbacks()"));
}

TEST(InstructionSemantics, ForwardBranchDoesNotYield) {
  auto f = makeFunc({encI(0x05, 4, 0, 2), kNop, kNop, kJrRa, kNop});
  EXPECT_FALSE(has(emitFn(f), "drainPendingCallbacks"));
}

TEST(InstructionSemantics, DelaySlotThatIsAlsoATargetIsReemittedLabelled) {
  // [0] beq -> [3]; [1] slot; [2] bne -> [1] (the slot of [0])
  auto f = makeFunc({encI(0x04, 4, 5, 2), encI(0x09, 0, 2, 1),
                     encI(0x05, 4, 0, 0xFFFE), kNop, kJrRa, kNop});
  auto code = emitFn(f);
  EXPECT_TRUE(has(code, "goto L_dsdone_80010004;")) << code;
  EXPECT_TRUE(has(code, "L_80010004:\n")) << code;
  EXPECT_TRUE(has(code, "delay slot, also a branch target")) << code;
  EXPECT_TRUE(has(code, "L_dsdone_80010004: ;")) << code;
}

// Function emission: control flow outside the linear body

TEST(InstructionSemantics, WordsAfterJrAreDataUntilNextLabel) {
  auto f = makeFunc({kJrRa, kNop, 0xDEADBEEFu, 0x12345678u});
  auto code = emitFn(f);
  EXPECT_TRUE(has(code, "// .word 0xDEADBEEF (data)")) << code;
  EXPECT_TRUE(has(code, "// .word 0x12345678 (data)")) << code;
}

TEST(InstructionSemantics, BranchOutsideFunctionDispatches) {
  auto f = makeFunc({encI(0x04, 0, 0, 0x0100), kNop, kJrRa, kNop});
  auto code = emitFn(f);
  // target = 0x80010004 + 0x400
  EXPECT_TRUE(has(code, "L_80010404:\n    recomp_dispatch(rdram, ctx, 0x80010404); return;"))
      << code;
}

TEST(InstructionSemantics, JumpTableBecomesStaticGotoChain) {
  auto f = makeFunc({encR(0, 8, 0, 0, 0, 0x08), kNop, encI(0x09, 0, 2, 1),
                     kJrRa, kNop, encI(0x09, 0, 2, 2), kJrRa, kNop});
  f.jumpTables.push_back({0, {0x80010008, 0x80010014}});
  auto code = emitFn(f);
  EXPECT_TRUE(has(code, "switch table (2 entries)")) << code;
  EXPECT_TRUE(has(code, "if (_sw_target == 0x80010008u) goto L_80010008;")) << code;
  EXPECT_TRUE(has(code, "if (_sw_target == 0x80010014u) goto L_80010014;")) << code;
  EXPECT_TRUE(has(code, "L_80010014:")) << code;
  EXPECT_TRUE(has(code, "JUMP_INDIRECT_AT(ctx, ctx->r8, 0x80010000u)")) << code;
}

TEST(InstructionSemantics, UntabledJrMatchesInFunctionLabels) {
  // [0] beq -> [4] gives the function one label; jr $t0 at [2] tries it first.
  auto f = makeFunc({encI(0x04, 0, 0, 3), kNop, encR(0, 8, 0, 0, 0, 0x08),
                     kNop, kJrRa, kNop});
  auto code = emitFn(f);
  EXPECT_TRUE(has(code, "in-function labels (1 entries)")) << code;
  EXPECT_TRUE(has(code, "if (_sw_target == 0x80010010u) goto L_80010010;")) << code;
}

TEST(InstructionSemantics, UntabledJrWithoutLabelsJumpsIndirect) {
  auto f = makeFunc({encR(0, 8, 0, 0, 0, 0x08), kNop});
  auto code = emitFn(f);
  EXPECT_TRUE(has(code, "JUMP_INDIRECT_AT(ctx, ctx->r8, 0x80010000u);")) << code;
  EXPECT_FALSE(has(code, "_sw_target")) << code;
}

TEST(InstructionSemantics, RaPointedIntoFunctionTurnsJrRaIntoLocalResume) {
  // lui $ra, 0x8001 ; addiu $ra, $ra, 0x10 ; jr $t0 ; nop ; [0x10] jr $ra ; nop
  auto f = makeFunc({encI(0x0F, 0, 31, 0x8001), encI(0x09, 31, 31, 0x0010),
                     encR(0, 8, 0, 0, 0, 0x08), kNop, kJrRa, kNop});
  auto code = emitFn(f);
  EXPECT_TRUE(has(code, "JUMP_INDIRECT_RESUME_AT(ctx, ctx->r8, 0x80010008u);"))
      << code;
  EXPECT_TRUE(has(code, "if (ctx->r31 == 0x80010010u) goto L_80010010;")) << code;
  EXPECT_TRUE(has(code, "L_80010010:")) << code;
}
