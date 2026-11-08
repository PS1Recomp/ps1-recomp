// Tests for ps1Runtime -- Runtime Macros
// Validates MEM_READ/WRITE macros, unaligned loads, and GTE register access

#include <gtest/gtest.h>
#include <memory>
#include <runtime/ps1_runtime_macros.h>

using namespace ps1;

// GTE Register Access

TEST(RuntimeMacros, GteReadWriteData) {
  CPUContext ctx;
  ctx.reset();

  gte_write_data(&ctx, 8, 0x00001000); // IR0
  EXPECT_EQ(gte_read_data(&ctx, 8), 0x00001000u);
}

// psx-spx: LZCR reads the leading-bit count of LZCS (zeros, or ones if
// negative). libgte SquareRoot0 depends on it.
TEST(RuntimeMacros, GteLzcrCountsLeadingBitsOfLzcs) {
  CPUContext ctx;
  ctx.reset();

  gte_write_data(&ctx, 30, 0x01C80000); // LZCS
  EXPECT_EQ(gte_read_data(&ctx, 31), 7u);
  gte_write_data(&ctx, 30, 0xFFF00000);
  EXPECT_EQ(gte_read_data(&ctx, 31), 12u);
}

TEST(RuntimeMacros, GteSxypWritePushesTheScreenFifo) {
  CPUContext ctx;
  ctx.reset();

  gte_write_data(&ctx, 14, 0x00020002); // SXY2
  gte_write_data(&ctx, 15, 0x00030003); // SXYP
  EXPECT_EQ(gte_read_data(&ctx, 13), 0x00020002u);
  EXPECT_EQ(gte_read_data(&ctx, 14), 0x00030003u);
}

TEST(RuntimeMacros, GteReadWriteControl) {
  CPUContext ctx;
  ctx.reset();

  gte_write_control(&ctx, 31, 0x7F87E000); // FLAG
  EXPECT_EQ(gte_read_control(&ctx, 31), 0x7F87E000u);
}

TEST(RuntimeMacros, GteRegisterMasking) {
  CPUContext ctx;
  ctx.reset();

  // Register index is masked to 0-31
  gte_write_data(&ctx, 0x20, 0xABCD); // 32 -> wraps to 0
  EXPECT_EQ(gte_read_data(&ctx, 0), 0xABCDu);
}

// Unaligned Loads (LWL / LWR)

TEST(RuntimeMacros, LWLAligned) {
  Memory mem;
  recomp_context ctx;
  ctx.reset();
  ctx.mem = &mem;
  ctx.bios = nullptr;
  mem.write32(0x1000, 0xAABBCCDD);

  // LWL with shift=3 (fully aligned word)
  uint32_t result = DO_LWL(&ctx, 0x00000000, 0x1003);
  EXPECT_EQ(result, 0xAABBCCDDu);
}

TEST(RuntimeMacros, LWRAligned) {
  Memory mem;
  recomp_context ctx;
  ctx.reset();
  ctx.mem = &mem;
  ctx.bios = nullptr;
  mem.write32(0x1000, 0xAABBCCDD);

  // LWR with shift=0 (fully aligned word)
  uint32_t result = DO_LWR(&ctx, 0x00000000, 0x1000);
  EXPECT_EQ(result, 0xAABBCCDDu);
}

TEST(RuntimeMacros, LWLPartialMerge) {
  Memory mem;
  recomp_context ctx;
  ctx.reset();
  ctx.mem = &mem;
  ctx.bios = nullptr;
  mem.write32(0x1000, 0xAABBCCDD);

  // LWL shift=0 -> load high byte only, keep low 3 bytes of rt
  uint32_t result = DO_LWL(&ctx, 0x11223344, 0x1000);
  EXPECT_EQ(result, (0x11223344u & 0x00FFFFFF) | (0xAABBCCDD << 24));
}

TEST(RuntimeMacros, LWRPartialMerge) {
  Memory mem;
  recomp_context ctx;
  ctx.reset();
  ctx.mem = &mem;
  ctx.bios = nullptr;
  mem.write32(0x1000, 0xAABBCCDD);

  // LWR shift=3 -> load low byte only, keep high 3 bytes of rt
  uint32_t result = DO_LWR(&ctx, 0x11223344, 0x1003);
  EXPECT_EQ(result, (0x11223344u & 0xFFFFFF00) | (0xAABBCCDD >> 24));
}

// Unaligned Stores (SWL / SWR)

TEST(RuntimeMacros, SWLFullWrite) {
  Memory mem;
  recomp_context ctx;
  ctx.reset();
  ctx.mem = &mem;
  ctx.bios = nullptr;
  mem.write32(0x2000, 0x00000000);

  DO_SWL(&ctx, 0xAABBCCDD, 0x2003);
  EXPECT_EQ(mem.read32(0x2000), 0xAABBCCDDu);
}

TEST(RuntimeMacros, SWRFullWrite) {
  Memory mem;
  recomp_context ctx;
  ctx.reset();
  ctx.mem = &mem;
  ctx.bios = nullptr;
  mem.write32(0x2000, 0x00000000);

  DO_SWR(&ctx, 0xAABBCCDD, 0x2000);
  EXPECT_EQ(mem.read32(0x2000), 0xAABBCCDDu);
}

// MEM_READ/WRITE Macros (via recomp_context)

TEST(RuntimeMacros, MemMacros32) {
  Memory mem;
  recomp_context ctx;
  ctx.reset();
  ctx.mem = &mem;
  ctx.bios = nullptr;

  MEM_WRITE32(&ctx, 0x80010000, 0xDEADBEEF);
  EXPECT_EQ(MEM_READ32(&ctx, 0x80010000), 0xDEADBEEFu);
}

TEST(RuntimeMacros, MemMacros16) {
  Memory mem;
  recomp_context ctx;
  ctx.reset();
  ctx.mem = &mem;
  ctx.bios = nullptr;

  MEM_WRITE16(&ctx, 0x80020000, 0xBEEF);
  EXPECT_EQ(MEM_READ16(&ctx, 0x80020000), 0xBEEFu);
}

TEST(RuntimeMacros, MemMacros8) {
  Memory mem;
  recomp_context ctx;
  ctx.reset();
  ctx.mem = &mem;
  ctx.bios = nullptr;

  MEM_WRITE8(&ctx, 0x80030000, 0x42);
  EXPECT_EQ(MEM_READ8(&ctx, 0x80030000), 0x42u);
}

// Divide (psx-spx "CPU Specifications", divide errors): the R3000A defines
// every result, including the ones C++ leaves undefined.
TEST(RuntimeMacros, DivComputesQuotientAndRemainder) {
  CPUContext ctx;
  ctx.reset();
  DO_DIV(&ctx, static_cast<uint32_t>(-7), 2);
  EXPECT_EQ(static_cast<int32_t>(ctx.lo), -3);
  EXPECT_EQ(static_cast<int32_t>(ctx.hi), -1);
  DO_DIVU(&ctx, 7, 2);
  EXPECT_EQ(ctx.lo, 3u);
  EXPECT_EQ(ctx.hi, 1u);
}

TEST(RuntimeMacros, DivByZeroGivesTheHardwareResult) {
  CPUContext ctx;
  ctx.reset();
  DO_DIV(&ctx, 5, 0); // positive: lo = -1, hi = rs
  EXPECT_EQ(ctx.lo, 0xFFFFFFFFu);
  EXPECT_EQ(ctx.hi, 5u);
  DO_DIV(&ctx, static_cast<uint32_t>(-5), 0); // negative: lo = +1
  EXPECT_EQ(ctx.lo, 1u);
  EXPECT_EQ(ctx.hi, static_cast<uint32_t>(-5));
  DO_DIVU(&ctx, 9, 0); // unsigned: lo = FFFFFFFFh, hi = rs
  EXPECT_EQ(ctx.lo, 0xFFFFFFFFu);
  EXPECT_EQ(ctx.hi, 9u);
}

TEST(RuntimeMacros, DivOfMinIntByMinusOneDoesNotTrap) {
  CPUContext ctx;
  ctx.reset();
  DO_DIV(&ctx, 0x80000000u, 0xFFFFFFFFu);
  EXPECT_EQ(ctx.lo, 0x80000000u);
  EXPECT_EQ(ctx.hi, 0u);
}

// Unaligned word access, every byte offset
//
// Contract: psx-spx cpuspecifications.md, "LWL/LWR/SWL/SWR": on the
// little-endian R3000A the pair LWR(a) + LWL(a+3) loads the 4 bytes at a..a+3,
// and SWR(a) + SWL(a+3) stores them, without touching the bytes around them.
// The expected words are built byte by byte from memory, not from the macros.

namespace {
class UnalignedAccess : public ::testing::TestWithParam<uint32_t> {
protected:
  std::unique_ptr<Memory> mem = std::make_unique<Memory>();
  recomp_context ctx;
  static constexpr uint32_t kBase = 0x1000;

  void SetUp() override {
    ctx.reset();
    ctx.mem = mem.get();
    for (uint32_t i = 0; i < 12; ++i)
      mem->write8(kBase + i, static_cast<uint8_t>(0x10 + i));
  }
  uint32_t bytesAt(uint32_t a) const {
    return mem->read8(a) | (mem->read8(a + 1) << 8) | (mem->read8(a + 2) << 16) |
           (static_cast<uint32_t>(mem->read8(a + 3)) << 24);
  }
};
} // namespace

TEST_P(UnalignedAccess, LwrThenLwlLoadsTheFourBytesAtTheAddress) {
  const uint32_t a = kBase + 4 + GetParam();
  const uint32_t expected = bytesAt(a);

  uint32_t rt = DO_LWR(&ctx, 0xDEADBEEF, a);
  rt = DO_LWL(&ctx, rt, a + 3);

  EXPECT_EQ(rt, expected);
}

TEST_P(UnalignedAccess, LwlAloneKeepsTheLowBytesOfTheRegister) {
  const uint32_t k = GetParam(); // LWL at offset k fills the top k+1 bytes
  const uint32_t a = kBase + k;
  const uint32_t keepMask = k == 3 ? 0 : (0xFFFFFFFFu >> (8 * (k + 1)));

  const uint32_t rt = DO_LWL(&ctx, 0xA1B2C3D4, a);

  EXPECT_EQ(rt & keepMask, 0xA1B2C3D4u & keepMask);
  EXPECT_EQ(rt >> 24, mem->read8(a)) << "top byte is the addressed byte";
}

TEST_P(UnalignedAccess, LwrAloneKeepsTheHighBytesOfTheRegister) {
  const uint32_t k = GetParam(); // LWR at offset k fills the low 4-k bytes
  const uint32_t a = kBase + k;
  const uint32_t keepMask = k == 0 ? 0 : (0xFFFFFFFFu << (8 * (4 - k)));

  const uint32_t rt = DO_LWR(&ctx, 0xA1B2C3D4, a);

  EXPECT_EQ(rt & keepMask, 0xA1B2C3D4u & keepMask);
  EXPECT_EQ(rt & 0xFF, mem->read8(a)) << "low byte is the addressed byte";
}

TEST_P(UnalignedAccess, SwrThenSwlStoresTheWordAtTheAddressOnly) {
  const uint32_t a = kBase + 4 + GetParam();
  const uint8_t before = mem->read8(a - 1);
  const uint8_t after = mem->read8(a + 4);

  DO_SWR(&ctx, 0xCAFEF00D, a);
  DO_SWL(&ctx, 0xCAFEF00D, a + 3);

  EXPECT_EQ(bytesAt(a), 0xCAFEF00Du);
  EXPECT_EQ(mem->read8(a - 1), before);
  EXPECT_EQ(mem->read8(a + 4), after);
}

INSTANTIATE_TEST_SUITE_P(EveryByteOffset, UnalignedAccess,
                         ::testing::Values(0u, 1u, 2u, 3u));

TEST(RuntimeMacros, DivOfMinIntByAnotherDivisorIsOrdinary) {
  CPUContext ctx;
  ctx.reset();

  DO_DIV(&ctx, 0x80000000u, 2);

  EXPECT_EQ(ctx.lo, 0xC0000000u);
  EXPECT_EQ(ctx.hi, 0u);
}
