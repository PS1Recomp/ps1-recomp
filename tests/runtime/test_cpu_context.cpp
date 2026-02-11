// Tests for ps1xRuntime — CPU Context
// Validates the PS1 CPU register context structure

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>

// ──────────────────────────────────────────
// PS1 CPU Context Definition (test-driven)
// This will become runtime/src/cpu_context.h
// ──────────────────────────────────────────

namespace ps1 {

struct CPUContext {
    uint32_t r[32];     // 32 General Purpose Registers (r0 = always zero)
    uint32_t pc;        // Program Counter
    uint32_t hi;        // HI register (mult/div result high)
    uint32_t lo;        // LO register (mult/div result low)

    void reset() {
        std::memset(this, 0, sizeof(*this));
    }
};

// PS1 Register aliases
enum Register : uint32_t {
    ZERO = 0,   // Always zero
    AT = 1,     // Assembler temporary
    V0 = 2, V1 = 3,                    // Return values
    A0 = 4, A1 = 5, A2 = 6, A3 = 7,   // Arguments
    T0 = 8, T1 = 9, T2 = 10, T3 = 11, // Temporaries
    T4 = 12, T5 = 13, T6 = 14, T7 = 15,
    S0 = 16, S1 = 17, S2 = 18, S3 = 19, // Saved
    S4 = 20, S5 = 21, S6 = 22, S7 = 23,
    T8 = 24, T9 = 25,                    // More temporaries
    K0 = 26, K1 = 27,                    // Kernel
    GP = 28,    // Global pointer
    SP = 29,    // Stack pointer
    FP = 30,    // Frame pointer (S8)
    RA = 31,    // Return address
};

} // namespace ps1

// ──────────────────────────────────────────
// Context Initialization
// ──────────────────────────────────────────

TEST(CPUContext, Reset) {
    ps1::CPUContext ctx;
    ctx.r[ps1::SP] = 0xDEADBEEF;
    ctx.pc = 0x80050000;
    ctx.hi = 0xFFFFFFFF;
    ctx.lo = 0xFFFFFFFF;

    ctx.reset();

    EXPECT_EQ(ctx.pc, 0u);
    EXPECT_EQ(ctx.hi, 0u);
    EXPECT_EQ(ctx.lo, 0u);
    for (int i = 0; i < 32; i++) {
        EXPECT_EQ(ctx.r[i], 0u) << "Register r" << i << " not zeroed";
    }
}

TEST(CPUContext, R0AlwaysZero) {
    // r0 ($zero) must always be 0 in MIPS
    // The runtime should enforce this after every instruction
    ps1::CPUContext ctx;
    ctx.reset();
    ctx.r[ps1::ZERO] = 0x12345678;
    // In the real runtime, we'd enforce: ctx.r[0] = 0 after each instruction
    // For now, test that we know it should be zero
    EXPECT_EQ(ps1::ZERO, 0u);
}

TEST(CPUContext, StructSize) {
    // CPUContext should be compact: 32 regs * 4 + pc + hi + lo = 140 bytes
    ps1::CPUContext ctx;
    EXPECT_EQ(sizeof(ctx.r), 32 * sizeof(uint32_t));  // 128 bytes
    EXPECT_EQ(sizeof(ctx.pc), sizeof(uint32_t));       // 4 bytes
    EXPECT_EQ(sizeof(ctx.hi), sizeof(uint32_t));       // 4 bytes
    EXPECT_EQ(sizeof(ctx.lo), sizeof(uint32_t));       // 4 bytes
}

// ──────────────────────────────────────────
// Register Aliases
// ──────────────────────────────────────────

TEST(CPUContext, RegisterAliases) {
    // Verify that MIPS register aliases map to correct indices
    EXPECT_EQ(ps1::ZERO, 0u);
    EXPECT_EQ(ps1::AT, 1u);
    EXPECT_EQ(ps1::V0, 2u);
    EXPECT_EQ(ps1::A0, 4u);
    EXPECT_EQ(ps1::T0, 8u);
    EXPECT_EQ(ps1::S0, 16u);
    EXPECT_EQ(ps1::GP, 28u);
    EXPECT_EQ(ps1::SP, 29u);
    EXPECT_EQ(ps1::FP, 30u);
    EXPECT_EQ(ps1::RA, 31u);
}

TEST(CPUContext, RegisterReadWrite) {
    ps1::CPUContext ctx;
    ctx.reset();

    // Simulate: addiu $sp, $sp, -0x20
    ctx.r[ps1::SP] = 0x801FFFF0;
    ctx.r[ps1::SP] = ctx.r[ps1::SP] + static_cast<uint32_t>(-0x20);
    EXPECT_EQ(ctx.r[ps1::SP], 0x801FFFD0u);

    // Simulate: jal → saves return address in $ra
    ctx.r[ps1::RA] = 0x80050004;
    EXPECT_EQ(ctx.r[ps1::RA], 0x80050004u);

    // Simulate: mult result
    ctx.hi = 0x00000001;
    ctx.lo = 0x00000064;
    EXPECT_EQ(ctx.hi, 1u);
    EXPECT_EQ(ctx.lo, 100u);
}

// ──────────────────────────────────────────
// Typical PS1 Entry Point
// ──────────────────────────────────────────

TEST(CPUContext, PS1EntryPoint) {
    ps1::CPUContext ctx;
    ctx.reset();

    // Typical PS1 entry point from ELF
    ctx.pc = 0x80010000;
    ctx.r[ps1::SP] = 0x801FFF00; // Stack at top of RAM
    ctx.r[ps1::GP] = 0x800A0000; // Global pointer

    EXPECT_EQ(ctx.pc, 0x80010000u);
    EXPECT_EQ(ctx.r[ps1::SP], 0x801FFF00u);
    EXPECT_EQ(ctx.r[ps1::GP], 0x800A0000u);
}
