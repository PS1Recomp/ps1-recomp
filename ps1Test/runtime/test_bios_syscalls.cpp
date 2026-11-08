// Tests: BIOS syscall tables.
//
// Contract: psx-spx "Kernel (BIOS)" (kernelbios.md) -- "BIOS Function
// Summary", "BIOS String Functions", "BIOS Memory Functions", "BIOS Misc
// Functions", "BIOS GPU Functions", "BIOS Joypad Functions".  Values asserted
// here come from those sections, not from the HLE's current output.
#include "runtime/bios/bios.h"
#include "runtime/cdrom/cdrom_controller.h"
#include "runtime/cdrom/virtual_fs.h"
#include "runtime/cpu_context.h"
#include "runtime/dma/dma.h"
#include "runtime/gpu/gpu.h"
#include "runtime/input/input.h"
#include "runtime/memory.h"
#include "runtime/psyq/psyq_state.h"
#include <cstring>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace ps1;
using namespace ps1::bios;

namespace {

constexpr uint32_t kBufA = 0x80010000;
constexpr uint32_t kBufB = 0x80011000;
constexpr uint32_t kBufC = 0x80012000;

class BiosSyscallTest : public ::testing::Test {
protected:
  Memory mem;
  recomp_context ctx;
  cdrom::VirtualFs fs;
  std::unique_ptr<Bios> bios;

  void SetUp() override {
    ctx.reset();
    ctx.mem = &mem;
    ctx.r[SP] = 0x801FF000;
    bios = std::make_unique<Bios>(ctx, fs, mem);
    ps1::psyq::psyq_state().reset();
  }
  void TearDown() override { ps1::psyq::psyq_state().reset(); }

  void a0(uint32_t fn) {
    ctx.r[T1] = fn;
    bios->executeA0();
  }
  void b0(uint32_t fn) {
    ctx.r[T1] = fn;
    bios->executeB0();
  }

  void writeString(uint32_t addr, const char *s) {
    do {
      mem.write8(addr++, static_cast<uint8_t>(*s));
    } while (*s++);
  }
  std::string readString(uint32_t addr) {
    std::string s;
    for (char c; (c = static_cast<char>(mem.read8(addr++))) != '\0';)
      s += c;
    return s;
  }
  void writeBytes(uint32_t addr, std::initializer_list<uint8_t> bytes) {
    for (uint8_t b : bytes)
      mem.write8(addr++, b);
  }
};

// String functions

TEST_F(BiosSyscallTest, StrncatAppendsAtMostMaxlenCharsAndTerminates) {
  // psx-spx A(16h): a src longer than maxlen is clipped and 00h-terminated.
  writeString(kBufA, "foo");
  writeString(kBufB, "barbaz");
  ctx.r[A0] = kBufA; ctx.r[A1] = kBufB; ctx.r[A2] = 3;
  a0(0x16);
  EXPECT_EQ(readString(kBufA), "foobar");
  EXPECT_EQ(ctx.r[V0], kBufA);
}

TEST_F(BiosSyscallTest, StrncatCopiesAShorterSourceWhole) {
  writeString(kBufA, "ab");
  writeString(kBufB, "cd");
  ctx.r[A0] = kBufA; ctx.r[A1] = kBufB; ctx.r[A2] = 10;
  a0(0x16);
  EXPECT_EQ(readString(kBufA), "abcd");
}

TEST_F(BiosSyscallTest, StrcmpReturnsSignExpandedByteDifference) {
  // psx-spx A(17h): [str1+N]-[str2+N], both bytes sign-expanded to 32 bits.
  writeBytes(kBufA, {'a', 0xF0, 0});
  writeBytes(kBufB, {'a', 0x10, 0});
  ctx.r[A0] = kBufA; ctx.r[A1] = kBufB;
  a0(0x17);
  EXPECT_EQ(static_cast<int32_t>(ctx.r[V0]), -16 - 0x10);
}

TEST_F(BiosSyscallTest, StrncmpWithZeroMaxlenIsEqual) {
  writeString(kBufA, "abc");
  writeString(kBufB, "xyz");
  ctx.r[A0] = kBufA; ctx.r[A1] = kBufB; ctx.r[A2] = 0;
  a0(0x18);
  EXPECT_EQ(ctx.r[V0], 0u);
}

TEST_F(BiosSyscallTest, StrncmpIgnoresMismatchesPastMaxlen) {
  writeString(kBufA, "abcX");
  writeString(kBufB, "abcY");
  ctx.r[A0] = kBufA; ctx.r[A1] = kBufB; ctx.r[A2] = 3;
  a0(0x18);
  EXPECT_EQ(ctx.r[V0], 0u);
  ctx.r[A2] = 4;
  a0(0x18);
  EXPECT_EQ(static_cast<int32_t>(ctx.r[V0]), 'X' - 'Y');
}

TEST_F(BiosSyscallTest, StrncpyPadsAShortSourceWithZeros) {
  // psx-spx A(1Ah): "the remaining bytes in dst are padded with 00h bytes".
  writeBytes(kBufA, {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA});
  writeString(kBufB, "ab");
  ctx.r[A0] = kBufA; ctx.r[A1] = kBufB; ctx.r[A2] = 5;
  a0(0x1A);
  EXPECT_EQ(mem.read8(kBufA + 0), 'a');
  EXPECT_EQ(mem.read8(kBufA + 1), 'b');
  for (uint32_t i = 2; i < 5; ++i)
    EXPECT_EQ(mem.read8(kBufA + i), 0) << "byte " << i;
  EXPECT_EQ(mem.read8(kBufA + 5), 0xAA) << "wrote past maxlen";
  EXPECT_EQ(ctx.r[V0], kBufA);
}

TEST_F(BiosSyscallTest, StrncpyDoesNotTerminateALongSource) {
  // psx-spx A(1Ah): src >= maxlen copies maxlen chars without a 00h.
  writeBytes(kBufA, {0xAA, 0xAA, 0xAA, 0xAA});
  writeString(kBufB, "abcdef");
  ctx.r[A0] = kBufA; ctx.r[A1] = kBufB; ctx.r[A2] = 3;
  a0(0x1A);
  EXPECT_EQ(mem.read8(kBufA + 2), 'c');
  EXPECT_EQ(mem.read8(kBufA + 3), 0xAA);
}

TEST_F(BiosSyscallTest, StrchrAndStrrchrFindFirstAndLastOccurrence) {
  writeString(kBufA, "banana");
  ctx.r[A0] = kBufA; ctx.r[A1] = 'n';
  a0(0x1E); // strchr
  EXPECT_EQ(ctx.r[V0], kBufA + 2);
  a0(0x1F); // strrchr
  EXPECT_EQ(ctx.r[V0], kBufA + 4);
  ctx.r[A1] = 'z';
  a0(0x1F);
  EXPECT_EQ(ctx.r[V0], 0u);
}

TEST_F(BiosSyscallTest, IndexAndRindexOfNulReturnTheEndAddress) {
  // psx-spx A(1Ch..1Fh): "Char may be 00h (returns the end address)".
  writeString(kBufA, "abc");
  ctx.r[A0] = kBufA; ctx.r[A1] = 0;
  a0(0x1C);
  EXPECT_EQ(ctx.r[V0], kBufA + 3);
  a0(0x1D);
  EXPECT_EQ(ctx.r[V0], kBufA + 3);
}

// Character functions

TEST_F(BiosSyscallTest, ToupperAndTolowerConvertOnlyLetters) {
  ctx.r[A0] = 'q';
  a0(0x25);
  EXPECT_EQ(ctx.r[V0], static_cast<uint32_t>('Q'));
  ctx.r[A0] = '{';
  a0(0x25);
  EXPECT_EQ(ctx.r[V0], static_cast<uint32_t>('{'));
  ctx.r[A0] = 'Q';
  a0(0x26);
  EXPECT_EQ(ctx.r[V0], static_cast<uint32_t>('q'));
  ctx.r[A0] = '5';
  a0(0x26);
  EXPECT_EQ(ctx.r[V0], static_cast<uint32_t>('5'));
}

TEST_F(BiosSyscallTest, ToupperMasksTheArgumentToOneByte) {
  // psx-spx A(25h): "Returns the incoming character, ANDed with FFh".
  ctx.r[A0] = 0x1261;
  a0(0x25);
  EXPECT_EQ(ctx.r[V0], 0x41u);
}

// Memory functions

TEST_F(BiosSyscallTest, BcopyTakesSourceFirst) {
  // psx-spx A(27h): same as memcpy with src and dst exchanged.
  writeBytes(kBufA, {1, 2, 3, 4});
  ctx.r[A0] = kBufA; ctx.r[A1] = kBufB; ctx.r[A2] = 3;
  a0(0x27);
  EXPECT_EQ(mem.read8(kBufB + 0), 1);
  EXPECT_EQ(mem.read8(kBufB + 2), 3);
  EXPECT_EQ(mem.read8(kBufB + 3), 0) << "copied past len";
}

TEST_F(BiosSyscallTest, BzeroClearsExactlyLenBytes) {
  writeBytes(kBufA, {9, 9, 9, 9});
  ctx.r[A0] = kBufA; ctx.r[A1] = 3;
  a0(0x28);
  EXPECT_EQ(mem.read8(kBufA + 0), 0);
  EXPECT_EQ(mem.read8(kBufA + 2), 0);
  EXPECT_EQ(mem.read8(kBufA + 3), 9);
}

TEST_F(BiosSyscallTest, BcmpReportsEqualAndUnequalBlocks) {
  writeBytes(kBufA, {1, 5, 9});
  writeBytes(kBufB, {1, 5, 9});
  writeBytes(kBufC, {1, 3, 7});
  ctx.r[A0] = kBufA; ctx.r[A1] = kBufB; ctx.r[A2] = 3;
  a0(0x29);
  EXPECT_EQ(ctx.r[V0], 0u);
  ctx.r[A1] = kBufC;
  a0(0x29);
  EXPECT_NE(ctx.r[V0], 0u);
}

TEST_F(BiosSyscallTest, MemcmpReturnsTheByteDifference) {
  // The bytes after the mismatch differ by the same amount, so the intended
  // [src1+N]-[src2+N] and the documented off-by-one bug (psx-spx A(2Dh)) agree.
  writeBytes(kBufA, {1, 5, 9});
  writeBytes(kBufB, {1, 3, 7});
  ctx.r[A0] = kBufA; ctx.r[A1] = kBufB; ctx.r[A2] = 3;
  a0(0x2D);
  EXPECT_EQ(static_cast<int32_t>(ctx.r[V0]), 2);
  ctx.r[A0] = kBufB; ctx.r[A1] = kBufA;
  a0(0x2D);
  EXPECT_EQ(static_cast<int32_t>(ctx.r[V0]), -2);
  ctx.r[A1] = kBufB;
  a0(0x2D);
  EXPECT_EQ(ctx.r[V0], 0u);
}

TEST_F(BiosSyscallTest, MemmoveCopiesOverlappingRegionsForward) {
  writeBytes(kBufA, {1, 2, 3, 4, 5});
  ctx.r[A0] = kBufA; ctx.r[A1] = kBufA + 1; ctx.r[A2] = 4; // dst < src
  a0(0x2C);
  for (uint32_t i = 0; i < 4; ++i)
    EXPECT_EQ(mem.read8(kBufA + i), i + 2) << "byte " << i;
  EXPECT_EQ(ctx.r[V0], kBufA);
}

TEST_F(BiosSyscallTest, MemmoveCopiesOverlappingRegionsBackward) {
  writeBytes(kBufA, {1, 2, 3, 4, 5});
  ctx.r[A0] = kBufA + 1; ctx.r[A1] = kBufA; ctx.r[A2] = 4; // dst > src
  a0(0x2C);
  for (uint32_t i = 0; i < 4; ++i)
    EXPECT_EQ(mem.read8(kBufA + 1 + i), i + 1) << "byte " << i;
  EXPECT_EQ(mem.read8(kBufA), 1);
}

TEST_F(BiosSyscallTest, MemchrReturnsFirstMatchOrZero) {
  // psx-spx A(2Eh): address of the first occurrence, or 0.
  writeBytes(kBufA, {7, 8, 9, 8});
  ctx.r[A0] = kBufA; ctx.r[A1] = 0x108; ctx.r[A2] = 4; // scanbyte is 8 bits
  a0(0x2E);
  EXPECT_EQ(ctx.r[V0], kBufA + 1);
  ctx.r[A1] = 9; ctx.r[A2] = 2; // beyond len
  a0(0x2E);
  EXPECT_EQ(ctx.r[V0], 0u);
}

TEST_F(BiosSyscallTest, MemsetReturnsDstAndFillsLowByte) {
  ctx.r[A0] = kBufA; ctx.r[A1] = 0x1234; ctx.r[A2] = 2;
  a0(0x2B);
  EXPECT_EQ(mem.read8(kBufA), 0x34);
  EXPECT_EQ(mem.read8(kBufA + 1), 0x34);
  EXPECT_EQ(mem.read8(kBufA + 2), 0);
  EXPECT_EQ(ctx.r[V0], kBufA);
}

// Misc functions

TEST_F(BiosSyscallTest, RandFollowsTheDocumentedLcg) {
  // psx-spx A(2Fh): x = x*41C64E6Dh + 3039h, return (x/10000h) AND 7FFFh.
  ctx.r[A0] = 1;
  a0(0x30); // srand(1)
  a0(0x2F);
  EXPECT_EQ(ctx.r[V0], 0x41C6u);
  a0(0x2F);
  EXPECT_EQ(ctx.r[V0], 0x167Eu);
  a0(0x2F);
  EXPECT_EQ(ctx.r[V0], 0x2781u);
}

TEST_F(BiosSyscallTest, SrandReplacesTheWholeSeed) {
  ctx.r[A0] = 0x12345678;
  a0(0x30);
  a0(0x2F);
  EXPECT_EQ(ctx.r[V0], 0x0B71u);
}

TEST_F(BiosSyscallTest, SetjmpSavesTheAbiRegistersAndReturnsZero) {
  // psx-spx A(13h): 00h ra, 04h sp, 08h fp, 0Ch r16..r23, 2Ch gp.
  ctx.r[RA] = 0x80012340; ctx.r[SP] = 0x801FE000; ctx.r[FP] = 0x801FE100;
  for (int i = 0; i < 8; ++i)
    ctx.r[S0 + i] = 0x1000 + i;
  ctx.r[GP] = 0x80055555;
  ctx.r[A0] = kBufA; ctx.r[V0] = 0xDEAD;
  a0(0x13);
  EXPECT_EQ(ctx.r[V0], 0u);
  EXPECT_EQ(mem.read32(kBufA + 0x00), 0x80012340u);
  EXPECT_EQ(mem.read32(kBufA + 0x04), 0x801FE000u);
  EXPECT_EQ(mem.read32(kBufA + 0x08), 0x801FE100u);
  EXPECT_EQ(mem.read32(kBufA + 0x0C), 0x1000u);
  EXPECT_EQ(mem.read32(kBufA + 0x28), 0x1007u);
  EXPECT_EQ(mem.read32(kBufA + 0x2C), 0x80055555u);
}

TEST_F(BiosSyscallTest, LongjmpRestoresTheBufferAndReturnsParamAtRa) {
  // psx-spx A(14h): restore r16-r23, gp, sp, fp, ra and "return" to ra.
  ctx.r[RA] = 0x80012340; ctx.r[SP] = 0x801FE000; ctx.r[FP] = 0x801FE100;
  for (int i = 0; i < 8; ++i)
    ctx.r[S0 + i] = 0x2000 + i;
  ctx.r[GP] = 0x80066666;
  ctx.r[A0] = kBufA;
  a0(0x13);
  ctx.r[RA] = 0; ctx.r[SP] = 0; ctx.r[FP] = 0; ctx.r[GP] = 0;
  for (int i = 0; i < 8; ++i)
    ctx.r[S0 + i] = 0;

  ctx.r[A0] = kBufA; ctx.r[A1] = 7;
  a0(0x14);
  EXPECT_EQ(ctx.pc, 0x80012340u);
  EXPECT_EQ(ctx.r[RA], 0x80012340u);
  EXPECT_EQ(ctx.r[SP], 0x801FE000u);
  EXPECT_EQ(ctx.r[FP], 0x801FE100u);
  EXPECT_EQ(ctx.r[S0 + 7], 0x2007u);
  EXPECT_EQ(ctx.r[GP], 0x80066666u);
  EXPECT_EQ(ctx.r[V0], 7u);
}

// Heap

TEST_F(BiosSyscallTest, MallocFailsBeforeInitHeap) {
  // psx-spx A(33h): "any call to malloc will fail, unless InitHeap has been
  // used".
  ctx.r[A0] = 16;
  a0(0x33);
  EXPECT_EQ(ctx.r[V0], 0u);
}

TEST_F(BiosSyscallTest, MallocReturnsAlignedBlocksInsideTheHeap) {
  ctx.r[A0] = 0x80100000; ctx.r[A1] = 0x10000;
  a0(0x39); // InitHeap
  ctx.r[A0] = 10;
  a0(0x33);
  const uint32_t p1 = ctx.r[V0];
  ctx.r[A0] = 10;
  a0(0x33);
  const uint32_t p2 = ctx.r[V0];
  EXPECT_NE(p1, 0u);
  EXPECT_EQ(p1 % 4, 0u);
  EXPECT_GE(p1, 0x80100000u);
  EXPECT_LT(p1, 0x80110000u);
  EXPECT_GE(p2, p1 + 12) << "blocks overlap (size rounds up to 12)";
}

TEST_F(BiosSyscallTest, FreedBlockIsReused) {
  ctx.r[A0] = 0x80100000; ctx.r[A1] = 0x10000;
  a0(0x39);
  ctx.r[A0] = 64;
  a0(0x33);
  const uint32_t p = ctx.r[V0];
  ctx.r[A0] = p;
  a0(0x34); // free
  ctx.r[A0] = 64;
  a0(0x33);
  EXPECT_EQ(ctx.r[V0], p);
}

TEST_F(BiosSyscallTest, MallocLargerThanTheHeapFails) {
  ctx.r[A0] = 0x80100000; ctx.r[A1] = 0x1000;
  a0(0x39);
  ctx.r[A0] = 0x2000;
  a0(0x33);
  EXPECT_EQ(ctx.r[V0], 0u);
}

// GPU functions

class BiosGpuSyscallTest : public BiosSyscallTest {
protected:
  gpu::GPU gpu;
  void SetUp() override {
    BiosSyscallTest::SetUp();
    gpu.reset();
    bios->setGPU(&gpu);
  }
  uint16_t pixel(int x, int y) const {
    return gpu.getVRAM()[y * gpu::GPU::VRAM_WIDTH + x].raw;
  }
};

TEST_F(BiosGpuSyscallTest, GetGpuStatusReturnsGpustat) {
  // psx-spx A(4Dh): reads [1F801814h] and returns it.
  gpu.writeGP1(0x08000001); // display mode, so the value is not the reset one
  a0(0x4D);
  EXPECT_EQ(ctx.r[V0], gpu.readGPUSTAT());
}

TEST_F(BiosGpuSyscallTest, SendGp1CommandReachesGp1) {
  // psx-spx A(48h): [1F801814h]=gp1cmd.  GP1(08h) bit0 -> GPUSTAT bit17.
  ctx.r[A0] = 0x08000001;
  a0(0x48);
  EXPECT_NE(gpu.readGPUSTAT() & (1u << 17), 0u);
}

TEST_F(BiosGpuSyscallTest, GpuCwReachesGp0) {
  // psx-spx A(49h): [1F801810h]=gp0cmd.  Three calls make one 16x1 FillRect.
  for (uint32_t w : {0x0200FF00u, 0x00030000u, 0x00010010u}) {
    ctx.r[A0] = w;
    a0(0x49);
  }
  EXPECT_EQ(pixel(0, 3), 0x03E0); // green
}

TEST_F(BiosGpuSyscallTest, GpuCwpSendsNumWordsFromRam) {
  // psx-spx A(4Ah): copy num words from [src] to GP0.  A 16x1 red FillRect.
  mem.write32(kBufA + 0, 0x020000FF);
  mem.write32(kBufA + 4, 0x00000000);
  mem.write32(kBufA + 8, 0x00010010);
  ctx.r[A0] = kBufA; ctx.r[A1] = 3;
  a0(0x4A);
  EXPECT_EQ(pixel(0, 0), 0x001F);
  EXPECT_EQ(pixel(15, 0), 0x001F);
  EXPECT_EQ(pixel(0, 1), 0x0000);
}

TEST_F(BiosGpuSyscallTest, SendGpuLinkedListWalksEveryNodeUpToTheEndMarker) {
  // psx-spx A(4Bh): transfer an OT; each node is [31:24]=count,[23:0]=next,
  // and next=FFFFFFh ends the list after that node's packet.
  const uint32_t n1 = 0x80020000, n2 = 0x80020100;
  mem.write32(n1, (3u << 24) | (n2 & 0xFFFFFF));
  mem.write32(n1 + 4, 0x020000FF); // red at (0,0)
  mem.write32(n1 + 8, 0x00000000);
  mem.write32(n1 + 12, 0x00010010);
  mem.write32(n2, (3u << 24) | 0xFFFFFF);
  mem.write32(n2 + 4, 0x02FF0000); // blue at (0,2)
  mem.write32(n2 + 8, 0x00020000);
  mem.write32(n2 + 12, 0x00010010);
  ctx.r[A0] = n1;
  a0(0x4B);
  EXPECT_EQ(pixel(0, 0), 0x001F);
  EXPECT_EQ(pixel(0, 2), 0x7C00);
}

TEST_F(BiosGpuSyscallTest, GpuSyncReturnsZeroWhenIdle) {
  // psx-spx A(4Eh): returns 0 (or -1 on timeout); this GPU is never busy.
  ctx.r[V0] = 0xFFFFFFFF;
  a0(0x4E);
  EXPECT_EQ(ctx.r[V0], 0u);
}

// printf (A(3Fh))

TEST_F(BiosSyscallTest, PrintfFormatsRegisterAndStackArguments) {
  // o32: the first three varargs come in a1..a3, the rest at sp+10h onwards.
  writeString(kBufA, "%d|%u|%x|%X|%c|%s|%%|%s");
  writeString(kBufB, "hi");
  ctx.r[A0] = kBufA;
  ctx.r[A1] = static_cast<uint32_t>(-5);
  ctx.r[A2] = 7;
  ctx.r[A3] = 0xAB;
  mem.write32(ctx.r[SP] + 0x10, 0xCD);
  mem.write32(ctx.r[SP] + 0x14, 'Z');
  mem.write32(ctx.r[SP] + 0x18, kBufB);
  mem.write32(ctx.r[SP] + 0x1C, 0); // NULL %s
  testing::internal::CaptureStdout();
  a0(0x3F);
  EXPECT_EQ(testing::internal::GetCapturedStdout(), "-5|7|ab|CD|Z|hi|%|(null)");
}

TEST_F(BiosSyscallTest, PrintfAcceptsFlagsWidthPrecisionAndLengthModifiers) {
  // Values chosen so the field already fills the width: output is the same
  // with or without padding support.
  writeString(kBufA, "[%02x][%-2d][%3.1i][%ld]");
  ctx.r[A0] = kBufA;
  ctx.r[A1] = 0xAB;
  ctx.r[A2] = 42;
  ctx.r[A3] = 123;
  mem.write32(ctx.r[SP] + 0x10, 9);
  testing::internal::CaptureStdout();
  a0(0x3F);
  EXPECT_EQ(testing::internal::GetCapturedStdout(), "[ab][42][123][9]");
}

// Joypad buffers (B(12h) InitPad / B(13h) StartPad), filled each VBlank

class BiosPadTest : public BiosSyscallTest {
protected:
  input::InputController input;
  void SetUp() override {
    BiosSyscallTest::SetUp();
    bios->setInputController(&input);
  }
  void initPad(uint32_t buf1, uint32_t buf2) {
    ctx.r[A0] = buf1; ctx.r[A1] = 34; ctx.r[A2] = buf2; ctx.r[A3] = 34;
    b0(0x12);
  }
};

TEST_F(BiosPadTest, StartedPadReportsStatusIdAndActiveLowButtons) {
  // psx-spx "BIOS Joypad Functions": 00h status (00h=okay), 01h id (41h =
  // digital pad), 02h.. button data, active-low.
  initPad(kBufA, kBufB);
  b0(0x13); // StartPad
  input.press(input::BTN_CROSS, 0);
  bios->updatePadBuffers();
  EXPECT_EQ(mem.read8(kBufA + 0), 0x00);
  EXPECT_EQ(mem.read8(kBufA + 1), 0x41);
  EXPECT_EQ(mem.read8(kBufA + 2), 0xFF);
  EXPECT_EQ(mem.read8(kBufA + 3), 0xFF & ~(input::BTN_CROSS >> 8));
}

TEST_F(BiosPadTest, DisconnectedPortReportsFFStatus) {
  input.setPadType(1, input::PadType::None);
  initPad(kBufA, kBufB);
  b0(0x13);
  bios->updatePadBuffers();
  EXPECT_EQ(mem.read8(kBufB + 0), 0xFF);
}

TEST_F(BiosPadTest, PadBuffersAreNotUpdatedBeforeStartPad) {
  initPad(kBufA, kBufB);
  input.press(input::BTN_START, 0);
  bios->updatePadBuffers();
  EXPECT_EQ(mem.read8(kBufA + 2), 0xFF) << "polled before StartPad";
}

// CD-ROM interrupt hand-off

TEST_F(BiosSyscallTest, CdromEventIsMirroredIntoLegacyBssBytes) {
  // INT2 completes a command (sync byte 2), INT1 marks data ready (ready 1).
  bios->setBssMirrors(kBufA, kBufA + 4);
  bios->triggerCdromEvent(2);
  bios->triggerCdromEvent(1);
  EXPECT_EQ(mem.read8(kBufA), 2);
  EXPECT_EQ(mem.read8(kBufA + 4), 1);
}

TEST_F(BiosSyscallTest, OutOfRangeCdromInterruptIsIgnored) {
  bios->setBssMirrors(kBufA, kBufA + 4);
  mem.write8(kBufA, 0x77);
  bios->triggerCdromEvent(0);
  bios->triggerCdromEvent(6);
  EXPECT_EQ(mem.read8(kBufA), 0x77);
}

namespace {
// Serves every sector with user data 00 01 02 .. (mode-2 form-1 offset 24).
class RampFs : public cdrom::VirtualFs {
public:
  std::optional<cdrom::Sector> readSector(uint32_t) override {
    cdrom::Sector s{};
    for (uint32_t i = 0; i < cdrom::SECTOR_SIZE_RAW; ++i)
      s.raw[i] = static_cast<uint8_t>(i >= 24 ? i - 24 : 0xEE);
    return s;
  }
};

void startRead(cdrom::CdromController &cd) {
  cd.writeRegister(0x1F801800, 0x00);
  cd.writeRegister(0x1F801801, 0x06); // ReadN
  for (int i = 0; i < 40 && !cd.hasSectorReady(); ++i)
    cd.tick(100000);
}
} // namespace

TEST_F(BiosSyscallTest, DataReadyCopiesTheSectorToTheCdReadDestination) {
  // libcd CdRead without a CdReadCallback: the INT1 handler stores the sector
  // at the stashed destination and counts it off.
  RampFs disc;
  cdrom::CdromController cd;
  cd.attachVirtualFs(&disc);
  bios->setCdromController(&cd);
  startRead(cd);
  ASSERT_TRUE(cd.hasSectorReady());
  auto &st = ps1::psyq::psyq_state();
  st.cdRemaining = 2;
  st.cdDestPtr = kBufA;
  st.cdWordCount = 4;

  bios->triggerCdromEvent(1);

  EXPECT_EQ(mem.read32(kBufA), 0x03020100u);
  EXPECT_EQ(mem.read32(kBufA + 12), 0x0F0E0D0Cu);
  EXPECT_EQ(mem.read32(kBufA + 16), 0u) << "copied more than cdWordCount";
  EXPECT_EQ(st.cdDestPtr, kBufA + 16);
  EXPECT_EQ(st.cdRemaining, 1u);
  EXPECT_EQ(cd.getState(), cdrom::CdromState::ReadingData);
  bios->setCdromController(nullptr);
}

TEST_F(BiosSyscallTest, LastSectorOfACdReadStopsTheDrive) {
  RampFs disc;
  cdrom::CdromController cd;
  cd.attachVirtualFs(&disc);
  bios->setCdromController(&cd);
  startRead(cd);
  auto &st = ps1::psyq::psyq_state();
  st.cdRemaining = 1;
  st.cdDestPtr = kBufA;
  st.cdWordCount = 1;

  bios->triggerCdromEvent(1);

  EXPECT_EQ(st.cdRemaining, 0u);
  EXPECT_EQ(cd.getState(), cdrom::CdromState::Idle);
  bios->setCdromController(nullptr);
}

// SetCustomExitFromException (B(19h)) fired by a VBlank

TEST_F(BiosSyscallTest, VBlankFiresTheCustomExitAtTheNextYieldPoint) {
  // psx-spx B(19h): the hook buffer has setjmp layout; an exception "returns"
  // to its ra with r2=1.
  mem.write32(kBufA + 0x00, 0x80015000); // ra
  mem.write32(kBufA + 0x04, 0x801FD000); // sp
  ctx.r[A0] = kBufA;
  b0(0x19);

  bios->triggerVBlankEvent();
  EXPECT_NE(ctx.pc, 0x80015000u) << "must not fire on the VBlank thread";
  bios->drainPendingCallbacksSlow();

  EXPECT_EQ(ctx.pc, 0x80015000u);
  EXPECT_EQ(ctx.r[SP], 0x801FD000u);
  EXPECT_EQ(ctx.r[V0], 1u);
}

TEST_F(BiosSyscallTest, ClearedCustomExitIsNotFired) {
  mem.write32(kBufA, 0x80015000);
  ctx.r[A0] = kBufA;
  b0(0x19);
  ctx.r[A0] = 0;
  b0(0x19);
  ctx.pc = 0x80010000;
  bios->triggerVBlankEvent();
  bios->drainPendingCallbacksSlow();
  EXPECT_EQ(ctx.pc, 0x80010000u);
}

} // namespace

// Callbacks dispatched from the drain (test_stubs.cpp routes recomp_dispatch).
extern void (*g_testRecompDispatchHook)(recomp_context *, uint32_t);

namespace {
uint32_t g_lastDispatch = 0;
uint32_t g_lastA0 = 0;
int g_dispatchCount = 0;
void recordDispatch(recomp_context *c, uint32_t addr) {
  g_lastDispatch = addr;
  g_lastA0 = c->r[A0];
  ++g_dispatchCount;
}
struct ScopedHook {
  ScopedHook() {
    g_lastDispatch = 0;
    g_lastA0 = 0;
    g_dispatchCount = 0;
    g_testRecompDispatchHook = &recordDispatch;
  }
  ~ScopedHook() { g_testRecompDispatchHook = nullptr; }
};

TEST_F(BiosSyscallTest, VBlankQueuesTheDisplaySwapCallbackWithSwapDone) {
  // The PsyQ VBlank chain calls the swap callback with a0=4 ("swap done").
  ScopedHook hook;
  ps1::psyq::psyq_state().gpuSwapCb = 0x80047200;
  bios->triggerVBlankEvent();
  bios->drainPendingCallbacksSlow();
  EXPECT_EQ(g_lastDispatch, 0x80047200u);
  EXPECT_EQ(g_lastA0, 4u);
}

TEST_F(BiosSyscallTest, StreamingReadPumpsTheNextSectorAfterTheDataCallback) {
  // With sectors still pending, the drain acknowledges INT1 and advances the
  // drive so the next sector is delivered in the same drain.
  ScopedHook hook;
  RampFs disc;
  cdrom::CdromController cd;
  cd.attachVirtualFs(&disc);
  cd.setInterruptCallback([this](uint8_t t) { bios->queueCdromEvent(t); });
  bios->setCdromController(&cd);
  startRead(cd);
  auto &st = ps1::psyq::psyq_state();
  st.cdDataCb = 0x80047300;
  st.cdRemaining = 3;
  bios->queueCdromEvent(1);
  bios->drainPendingCallbacksSlow();
  bios->setCdromController(nullptr);
  EXPECT_GE(g_dispatchCount, 2) << "the drive was not pumped for more data";
  EXPECT_EQ(g_lastDispatch, 0x80047300u);
  EXPECT_EQ(g_lastA0, 1u);
}

} // namespace

namespace {

// More string functions (psx-spx "BIOS String Functions")

TEST_F(BiosSyscallTest, StrncmpOfEqualStringsShorterThanMaxlenIsEqual) {
  writeString(kBufA, "abc");
  writeString(kBufB, "abc");
  ctx.r[A0] = kBufA; ctx.r[A1] = kBufB; ctx.r[A2] = 10;
  a0(0x18);
  EXPECT_EQ(ctx.r[V0], 0u);
}

TEST_F(BiosSyscallTest, StrchrOfAnAbsentCharacterReturnsZero) {
  writeString(kBufA, "hello");
  ctx.r[A0] = kBufA; ctx.r[A1] = 'z'; ctx.r[V0] = 0xDEAD;
  a0(0x1E);
  EXPECT_EQ(ctx.r[V0], 0u);
}

// GPU status without a GPU attached

TEST_F(BiosSyscallTest, GetGpuStatusWithoutAGpuReportsTheResetState) {
  // psx-spx GPU "GP1(00h) Reset GPU": "GPUSTAT becomes 14802000h".
  a0(0x4D);
  EXPECT_EQ(ctx.r[V0], 0x14802000u);
}

// B(56h) GetC0Table / B(57h) GetB0Table

TEST_F(BiosSyscallTest, GetB0AndC0TableReturnTheirOwnJumpLists) {
  // psx-spx: "Retrieves the address of the jump lists for B(NNh) and C(NNh)
  // functions".  Each entry holds this runtime's dispatch sentinel for that
  // function number, so a game that calls through the list reaches it.
  b0(0x57);
  const uint32_t bTable = ctx.r[V0];
  b0(0x56);
  const uint32_t cTable = ctx.r[V0];

  EXPECT_NE(bTable, cTable);
  EXPECT_EQ(mem.read32(bTable + 0x3D * 4), 0xB03Du);
  EXPECT_EQ(mem.read32(cTable + 0x0A * 4), 0xC00Au);
}

// B(0Ah) WaitEvent

TEST_F(BiosSyscallTest, WaitEventReturnsOnceForADeliveredEventThenTurnsBusy) {
  // psx-spx B(0Ah): "returns 1 once when it is ready (and automatically
  // switches the event back to busy status)".
  ctx.r[A0] = 0xF0000009; ctx.r[A1] = 0x20; ctx.r[A2] = 0x2000; ctx.r[A3] = 0;
  b0(0x08);
  const uint32_t ev = ctx.r[V0];
  ctx.r[A0] = ev;
  b0(0x0C);
  ctx.r[A0] = 0xF0000009; ctx.r[A1] = 0x20;
  b0(0x07); // DeliverEvent

  ctx.r[A0] = ev;
  b0(0x0A);
  const uint32_t waited = ctx.r[V0];
  ctx.r[A0] = ev;
  b0(0x0B);

  EXPECT_EQ(waited, 1u);
  EXPECT_EQ(ctx.r[V0], 0u) << "event still ready after WaitEvent";
}

TEST_F(BiosSyscallTest, WaitEventOnADisabledEventReturnsZero) {
  // psx-spx B(0Ah): "Returns 0 if the event is disabled."
  ctx.r[A0] = 0xF0000009; ctx.r[A1] = 0x20; ctx.r[A2] = 0x2000; ctx.r[A3] = 0;
  b0(0x08);
  const uint32_t ev = ctx.r[V0];

  ctx.r[A0] = ev;
  b0(0x0A);

  EXPECT_EQ(ctx.r[V0], 0u);
}

// TTY output: B(35h) write to std_out, B(3Dh) putchar

TEST_F(BiosSyscallTest, WriteToStdoutPrintsTheBytesAndReturnsTheCount) {
  // psx-spx B(35h): "out: V0 Number of bytes written"; fd 1 is std_out.
  writeString(kBufA, "hi!");
  ctx.r[A0] = 1; ctx.r[A1] = kBufA; ctx.r[A2] = 3;
  testing::internal::CaptureStdout();
  b0(0x35);
  EXPECT_EQ(testing::internal::GetCapturedStdout(), "hi!");
  EXPECT_EQ(ctx.r[V0], 3u);
}

TEST_F(BiosSyscallTest, PutcharWritesTheCharacterToStdout) {
  ctx.r[A0] = 'Q';
  testing::internal::CaptureStdout();
  b0(0x3D);
  EXPECT_EQ(testing::internal::GetCapturedStdout(), "Q");
}

TEST_F(BiosSyscallTest, PrintfPointerConversionPrintsTheAddressInHex) {
  writeString(kBufA, "%p");
  ctx.r[A0] = kBufA; ctx.r[A1] = 0x8001ABCD;
  testing::internal::CaptureStdout();
  a0(0x3F);
  const std::string out = testing::internal::GetCapturedStdout();
  EXPECT_NE(out.find("8001abcd"), std::string::npos) << out;
}

// CD-ROM files through B(32h) open / B(33h) lseek / B(34h) read

class OneFileFs : public cdrom::VirtualFs {
public:
  std::optional<std::vector<uint8_t>>
  readFile(const std::string &path) override {
    if (path != "DATA.BIN")
      return std::nullopt;
    return std::vector<uint8_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  }
};

TEST_F(BiosSyscallTest, LseekFromStartPositionsTheNextCdromRead) {
  // psx-spx lseek: "seektype 0 = from start of file".
  OneFileFs disc;
  Bios cdBios(ctx, disc, mem);
  writeString(kBufA, "cdrom:\\DATA.BIN;1");
  ctx.r[A0] = kBufA; ctx.r[A1] = 1;
  ctx.r[T1] = 0x32; cdBios.executeB0();
  const uint32_t fd = ctx.r[V0];
  ASSERT_NE(fd, static_cast<uint32_t>(-1));

  ctx.r[A0] = fd; ctx.r[A1] = 4; ctx.r[A2] = 0;
  ctx.r[T1] = 0x33; cdBios.executeB0();
  const uint32_t pos = ctx.r[V0];
  ctx.r[A0] = fd; ctx.r[A1] = kBufB; ctx.r[A2] = 2;
  ctx.r[T1] = 0x34; cdBios.executeB0();

  EXPECT_EQ(pos, 4u);
  EXPECT_EQ(ctx.r[V0], 2u);
  EXPECT_EQ(mem.read8(kBufB), 4);
  EXPECT_EQ(mem.read8(kBufB + 1), 5);
}

// CD interrupts raised on the game thread are handled on the spot

TEST_F(BiosSyscallTest, CdromInterruptRaisedOnTheGameThreadIsHandledAtOnce) {
  bios->setGameThreadId(std::this_thread::get_id());
  bios->setBssMirrors(kBufA, kBufA + 4);

  bios->queueCdromEvent(2);

  EXPECT_EQ(mem.read8(kBufA), 2);
}

TEST_F(BiosSyscallTest, CdromInterruptFromAnotherThreadWaitsForAYieldPoint) {
  bios->setBssMirrors(kBufA, kBufA + 4);

  bios->queueCdromEvent(2); // game thread id not set: not this thread
  const uint8_t before = mem.read8(kBufA);
  bios->drainPendingCallbacksSlow();

  EXPECT_EQ(before, 0);
  EXPECT_EQ(mem.read8(kBufA), 2);
}

TEST_F(BiosSyscallTest, DataReadyCopyNeverRunsPastTheSector) {
  // A CdRead word count larger than one sector (2352 raw bytes, 2328 after
  // the 24-byte header) must stop at the end of the sector instead of
  // copying whatever follows the buffer.
  RampFs disc;
  cdrom::CdromController cd;
  cd.attachVirtualFs(&disc);
  bios->setCdromController(&cd);
  startRead(cd);
  ASSERT_TRUE(cd.hasSectorReady());
  auto &st = ps1::psyq::psyq_state();
  st.cdRemaining = 2;
  st.cdDestPtr = kBufA;
  st.cdWordCount = 0x400; // 4 KiB
  constexpr uint32_t kSectorBytes = cdrom::SECTOR_SIZE_RAW - 24;
  mem.write32(kBufA + kSectorBytes, 0xA5A5A5A5);

  bios->triggerCdromEvent(1);
  bios->setCdromController(nullptr);

  EXPECT_EQ(mem.read32(kBufA + 0x7FC), 0xFFFEFDFCu) << "sector data missing";
  EXPECT_EQ(mem.read32(kBufA + kSectorBytes), 0xA5A5A5A5u)
      << "copied past the end of the sector";
}

TEST_F(BiosSyscallTest, DataReadyRunsADmaTransferTheGameArmedBeforeTheSector) {
  // A game that programs DMA channel 3 itself (no libcd read in flight) arms
  // it before the sector arrives; the transfer waits for the drive's data
  // request and runs on INT1.
  RampFs disc;
  cdrom::CdromController cd;
  cd.attachVirtualFs(&disc);
  ps1::DMA dma;
  dma.setMemory(&mem);
  dma.setCDROM(&cd);
  bios->setCdromController(&cd);
  bios->setDma(&dma);
  dma.writeRegister(0x1F8010F0, 0x00008000);    // DPCR: enable channel 3
  dma.writeRegister(0x1F8010B0, kBufA & 0xFFFFFF); // MADR
  dma.writeRegister(0x1F8010B4, 4);             // BCR: 4 words
  dma.writeRegister(0x1F8010B8, 0x11000000);    // CHCR: start+trigger, to RAM
  ASSERT_EQ(mem.read32(kBufA), 0u) << "ran before the sector was ready";
  startRead(cd);
  ASSERT_TRUE(cd.hasSectorReady());

  bios->triggerCdromEvent(1);
  bios->setDma(nullptr);
  bios->setCdromController(nullptr);

  EXPECT_EQ(mem.read32(kBufA), 0x03020100u);
}

// Pad buffers too small for the 4-byte header

TEST_F(BiosPadTest, PadBufferSmallerThanItsHeaderIsNeverWritten) {
  for (uint32_t i = 0; i < 4; ++i)
    mem.write8(kBufA + i, 0x77);
  ctx.r[A0] = kBufA; ctx.r[A1] = 2; ctx.r[A2] = 0; ctx.r[A3] = 0;
  b0(0x12);
  b0(0x13);

  bios->updatePadBuffers();

  EXPECT_EQ(mem.read8(kBufA + 0), 0x77);
  EXPECT_EQ(mem.read8(kBufA + 1), 0x77);
}

} // namespace
