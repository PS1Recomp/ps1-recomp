// Tests for ps1Runtime — Runtime Macros
// Validates MEM_READ/WRITE macros, unaligned loads, and GTE register access

#include <gtest/gtest.h>
#include <runtime/ps1_runtime_macros.h>

using namespace ps1;

// GTE Register Access

TEST(RuntimeMacros, GteReadWriteData) {
  CPUContext ctx;
  ctx.reset();

  gte_write_data(&ctx, 8, 0x00001000); // IR0
  EXPECT_EQ(gte_read_data(&ctx, 8), 0x00001000u);
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
  gte_write_data(&ctx, 0x20, 0xABCD); // 32 → wraps to 0
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

  // LWL shift=0 → load high byte only, keep low 3 bytes of rt
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

  // LWR shift=3 → load low byte only, keep high 3 bytes of rt
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
