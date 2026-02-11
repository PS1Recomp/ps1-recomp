// Tests for ps1xRecomp — MIPS I Decoder
// Validates MIPS I instruction decoding and field extraction

#include <gtest/gtest.h>
#include <cstdint>

// ──────────────────────────────────────────
// MIPS I Instruction Format Helpers
// These will become the actual decoder — test-driven development
// ──────────────────────────────────────────

namespace mips {

// MIPS I instruction field extraction
constexpr uint32_t opcode(uint32_t instr)  { return (instr >> 26) & 0x3F; }
constexpr uint32_t rs(uint32_t instr)      { return (instr >> 21) & 0x1F; }
constexpr uint32_t rt(uint32_t instr)      { return (instr >> 16) & 0x1F; }
constexpr uint32_t rd(uint32_t instr)      { return (instr >> 11) & 0x1F; }
constexpr uint32_t shamt(uint32_t instr)   { return (instr >>  6) & 0x1F; }
constexpr uint32_t funct(uint32_t instr)   { return instr & 0x3F; }
constexpr int16_t  imm(uint32_t instr)     { return static_cast<int16_t>(instr & 0xFFFF); }
constexpr uint16_t imm_u(uint32_t instr)   { return instr & 0xFFFF; }
constexpr uint32_t target(uint32_t instr)  { return instr & 0x03FFFFFF; }

// MIPS I Opcode constants
enum Opcode : uint32_t {
    OP_SPECIAL = 0x00,  // R-type (opcode=0, use funct)
    OP_REGIMM  = 0x01,  // BLTZ, BGEZ, etc.
    OP_J       = 0x02,
    OP_JAL     = 0x03,
    OP_BEQ     = 0x04,
    OP_BNE     = 0x05,
    OP_BLEZ    = 0x06,
    OP_BGTZ    = 0x07,
    OP_ADDI    = 0x08,
    OP_ADDIU   = 0x09,
    OP_SLTI    = 0x0A,
    OP_SLTIU   = 0x0B,
    OP_ANDI    = 0x0C,
    OP_ORI     = 0x0D,
    OP_XORI    = 0x0E,
    OP_LUI     = 0x0F,
    OP_COP0    = 0x10,
    OP_COP2    = 0x12,  // GTE
    OP_LB      = 0x20,
    OP_LH      = 0x21,
    OP_LW      = 0x23,
    OP_LBU     = 0x24,
    OP_LHU     = 0x25,
    OP_SB      = 0x28,
    OP_SH      = 0x29,
    OP_SW      = 0x2B,
    OP_LWC2    = 0x32,  // Load Word to COP2 (GTE)
    OP_SWC2    = 0x3A,  // Store Word from COP2 (GTE)
};

// MIPS I Function codes (when opcode == SPECIAL)
enum Funct : uint32_t {
    FN_SLL     = 0x00,
    FN_SRL     = 0x02,
    FN_SRA     = 0x03,
    FN_SLLV    = 0x04,
    FN_SRLV    = 0x06,
    FN_SRAV    = 0x07,
    FN_JR      = 0x08,
    FN_JALR    = 0x09,
    FN_SYSCALL = 0x0C,
    FN_BREAK   = 0x0D,
    FN_MFHI    = 0x10,
    FN_MTHI    = 0x11,
    FN_MFLO    = 0x12,
    FN_MTLO    = 0x13,
    FN_MULT    = 0x18,
    FN_MULTU   = 0x19,
    FN_DIV     = 0x1A,
    FN_DIVU    = 0x1B,
    FN_ADD     = 0x20,
    FN_ADDU    = 0x21,
    FN_SUB     = 0x22,
    FN_SUBU    = 0x23,
    FN_AND     = 0x24,
    FN_OR      = 0x25,
    FN_XOR     = 0x26,
    FN_NOR     = 0x27,
    FN_SLT     = 0x2A,
    FN_SLTU    = 0x2B,
};

} // namespace mips

// ──────────────────────────────────────────
// Field Extraction Tests
// ──────────────────────────────────────────

TEST(MipsDecoder, ExtractOpcode) {
    // addiu $a0, $a0, 0x20 → 0x24840020
    // opcode = 0x09 (ADDIU)
    EXPECT_EQ(mips::opcode(0x24840020), mips::OP_ADDIU);

    // j 0x80050000 → 0x08014000
    // opcode = 0x02 (J)
    EXPECT_EQ(mips::opcode(0x08014000), mips::OP_J);

    // nop (sll $zero, $zero, 0) → 0x00000000
    // opcode = 0x00 (SPECIAL)
    EXPECT_EQ(mips::opcode(0x00000000), mips::OP_SPECIAL);

    // lui $a0, 0x8005 → 0x3C048005
    // opcode = 0x0F (LUI)
    EXPECT_EQ(mips::opcode(0x3C048005), mips::OP_LUI);
}

TEST(MipsDecoder, ExtractRegisters) {
    // addu $v0, $a0, $a1 → 0x00851021
    // rs=4($a0), rt=5($a1), rd=2($v0)
    uint32_t instr = 0x00851021;
    EXPECT_EQ(mips::rs(instr), 4);   // $a0
    EXPECT_EQ(mips::rt(instr), 5);   // $a1
    EXPECT_EQ(mips::rd(instr), 2);   // $v0
    EXPECT_EQ(mips::funct(instr), mips::FN_ADDU);
}

TEST(MipsDecoder, ExtractImmediate) {
    // addiu $sp, $sp, -0x20 → 0x27BDFFE0
    // imm = -32 (0xFFE0 sign-extended)
    EXPECT_EQ(mips::imm(0x27BDFFE0), -32);

    // ori $a0, $zero, 0x1234 → 0x34041234
    // imm_u = 0x1234
    EXPECT_EQ(mips::imm_u(0x34041234), 0x1234);
}

TEST(MipsDecoder, ExtractTarget) {
    // j 0x80050000 → 0x08014000
    // target = 0x014000 (word-aligned, shifted by 2 to get address)
    uint32_t instr = 0x08014000;
    EXPECT_EQ(mips::target(instr), 0x014000u);
    // Full address = (PC & 0xF0000000) | (target << 2)
    uint32_t full_addr = (0x80000000u & 0xF0000000u) | (mips::target(instr) << 2);
    EXPECT_EQ(full_addr, 0x80050000u);
}

TEST(MipsDecoder, ExtractShamt) {
    // sll $v0, $v0, 2 → 0x00021080
    // shamt = 2
    EXPECT_EQ(mips::shamt(0x00021080), 2);

    // srl $a0, $a0, 8 → 0x00042200 (shamt=8 is at bits 10:6)
    // Need to construct: opcode=0, rs=0, rt=4, rd=4, shamt=8, funct=SRL(0x02)
    // = 0x00 | (0<<21) | (4<<16) | (4<<11) | (8<<6) | 0x02
    uint32_t srl_instr = (4 << 16) | (4 << 11) | (8 << 6) | 0x02;
    EXPECT_EQ(mips::shamt(srl_instr), 8);
    EXPECT_EQ(mips::funct(srl_instr), mips::FN_SRL);
}

// ──────────────────────────────────────────
// Instruction Type Detection
// ──────────────────────────────────────────

TEST(MipsDecoder, DetectRType) {
    // R-type instructions have opcode = 0x00 (SPECIAL)
    EXPECT_EQ(mips::opcode(0x00851021), mips::OP_SPECIAL);  // addu
    EXPECT_EQ(mips::opcode(0x00000000), mips::OP_SPECIAL);  // nop/sll
}

TEST(MipsDecoder, DetectIType) {
    EXPECT_EQ(mips::opcode(0x24840020), mips::OP_ADDIU);    // addiu
    EXPECT_EQ(mips::opcode(0x3C048005), mips::OP_LUI);      // lui
    EXPECT_EQ(mips::opcode(0x8C820000), mips::OP_LW);       // lw
    EXPECT_EQ(mips::opcode(0xAC820000), mips::OP_SW);        // sw
}

TEST(MipsDecoder, DetectJType) {
    EXPECT_EQ(mips::opcode(0x08014000), mips::OP_J);        // j
    EXPECT_EQ(mips::opcode(0x0C014000), mips::OP_JAL);      // jal
}

TEST(MipsDecoder, DetectGTE) {
    // COP2 (GTE) instructions have opcode = 0x12
    // Opcode is in bits 31:26, so COP2 = (0x12 << 26) = 0x48000000
    EXPECT_EQ(mips::opcode(0x48000000), mips::OP_COP2);     // cop2 operation
    EXPECT_EQ(mips::OP_COP2, 0x12u);
    EXPECT_EQ(mips::OP_LWC2, 0x32u);
    EXPECT_EQ(mips::OP_SWC2, 0x3Au);
}

// ──────────────────────────────────────────
// NOP Detection
// ──────────────────────────────────────────

TEST(MipsDecoder, NopInstruction) {
    // NOP = sll $zero, $zero, 0 = 0x00000000
    uint32_t nop = 0x00000000;
    EXPECT_EQ(mips::opcode(nop), mips::OP_SPECIAL);
    EXPECT_EQ(mips::funct(nop), mips::FN_SLL);
    EXPECT_EQ(mips::rd(nop), 0);    // $zero
    EXPECT_EQ(mips::rt(nop), 0);    // $zero
    EXPECT_EQ(mips::shamt(nop), 0);
}
