// Tests: GTE command semantics against psx-spx "Geometry Transformation
// Engine (GTE)". Expected values are derived from the formulas in that page
// (and duckstation's gte.cpp for FLAG details), not from the implementation.
// FLAG bits are written as literal bit numbers from psx-spx "GTE Saturation".

#include <algorithm>
#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <runtime/gte.h>

using namespace ps1;

namespace {

constexpr uint32_t bit(int n) { return 1u << n; }

uint32_t pack(int32_t lo, int32_t hi) {
  return static_cast<uint32_t>(static_cast<uint16_t>(lo)) |
         (static_cast<uint32_t>(static_cast<uint16_t>(hi)) << 16);
}

// psx-spx "GTE Division Inaccuracy": UNR division with the table generated
// by unr_table[i] = max(0, (40000h/(i+100h)+1)/2 - 101h).
uint32_t unrDivide(uint32_t h, uint32_t sz3) {
  if (h >= sz3 * 2)
    return 0x1FFFF;
  const int z = __builtin_clz(sz3) - 16;
  const uint64_t n = static_cast<uint64_t>(h) << z;
  uint64_t d = static_cast<uint64_t>(sz3) << z;
  const uint32_t idx = static_cast<uint32_t>((d - 0x7FC0) >> 7);
  const int32_t t = std::max<int32_t>(
      0, (0x40000 / static_cast<int32_t>(idx + 0x100) + 1) / 2 - 0x101);
  const uint64_t u = static_cast<uint64_t>(t) + 0x101;
  d = (0x2000080 - d * u) >> 8;
  d = (0x0000080 + d * u) >> 8;
  return static_cast<uint32_t>(std::min<uint64_t>((n * d + 0x8000) >> 16, 0x1FFFF));
}

} // namespace

class GteOpsTest : public ::testing::Test {
protected:
  CPUContext ctx;
  void SetUp() override { ctx.reset(); }

  int32_t s(uint8_t dataReg) const {
    return static_cast<int32_t>(ctx.cop2d[dataReg]);
  }
  uint32_t flag() const { return ctx.cop2c[GTE_FLAG]; }
  int16_t sx(uint8_t reg) const { return static_cast<int16_t>(ctx.cop2d[reg] & 0xFFFF); }
  int16_t sy(uint8_t reg) const { return static_cast<int16_t>(ctx.cop2d[reg] >> 16); }

  // base: 0 = rotation (RT), 8 = light (LLM), 16 = light color (LCM)
  void setMatrix(uint8_t base, const int16_t m[3][3]) {
    ctx.cop2c[base + 0] = pack(m[0][0], m[0][1]);
    ctx.cop2c[base + 1] = pack(m[0][2], m[1][0]);
    ctx.cop2c[base + 2] = pack(m[1][1], m[1][2]);
    ctx.cop2c[base + 3] = pack(m[2][0], m[2][1]);
    ctx.cop2c[base + 4] = static_cast<uint16_t>(m[2][2]);
  }
  void setDiag(uint8_t base, int16_t a, int16_t b, int16_t c) {
    const int16_t m[3][3] = {{a, 0, 0}, {0, b, 0}, {0, 0, c}};
    setMatrix(base, m);
  }
  void setVec(int idx, int16_t x, int16_t y, int16_t z) {
    ctx.cop2d[idx * 2] = pack(x, y);
    ctx.cop2d[idx * 2 + 1] = static_cast<uint32_t>(static_cast<int32_t>(z));
  }
  void setIR(int32_t a, int32_t b, int32_t c) {
    ctx.cop2d[GTE_IR1] = static_cast<uint32_t>(a);
    ctx.cop2d[GTE_IR2] = static_cast<uint32_t>(b);
    ctx.cop2d[GTE_IR3] = static_cast<uint32_t>(c);
  }
  void setTriple(uint8_t first, int32_t a, int32_t b, int32_t c) {
    ctx.cop2c[first] = static_cast<uint32_t>(a);
    ctx.cop2c[first + 1] = static_cast<uint32_t>(b);
    ctx.cop2c[first + 2] = static_cast<uint32_t>(c);
  }
  void expectMAC(int32_t a, int32_t b, int32_t c) {
    EXPECT_EQ(s(GTE_MAC1), a);
    EXPECT_EQ(s(GTE_MAC2), b);
    EXPECT_EQ(s(GTE_MAC3), c);
  }
  void expectIR(int32_t a, int32_t b, int32_t c) {
    EXPECT_EQ(s(GTE_IR1), a);
    EXPECT_EQ(s(GTE_IR2), b);
    EXPECT_EQ(s(GTE_IR3), c);
  }
  static uint32_t rgb(uint32_t r, uint32_t g, uint32_t b, uint32_t code) {
    return r | (g << 8) | (b << 16) | (code << 24);
  }
  // RTPS setup that exposes the division result: MAC0 = DQB + DQA*n = n.
  void setupQuotientProbe(uint16_t h, int32_t sz3) {
    setDiag(0, 4096, 4096, 4096);
    setTriple(GTE_TRX, 0, 0, sz3);
    ctx.cop2c[GTE_H] = h;
    ctx.cop2c[GTE_DQA] = 1;
    ctx.cop2c[GTE_DQB] = 0;
  }
};

// Register access (psx-spx "GTE Registers")

TEST_F(GteOpsTest, IrgbWriteExpandsEachChannelTimes80h) {
  GTE::writeData(&ctx, GTE_IRGB, 1 | (2 << 5) | (3 << 10));
  expectIR(0x80, 0x100, 0x180);
}

TEST_F(GteOpsTest, OrgbCollapsesInRangeIrToFiveBitsPerChannel) {
  setIR(3 * 0x80, 10 * 0x80 + 0x7F, 31 * 0x80); // fractions are truncated
  const uint32_t expected = 3 | (10 << 5) | (31 << 10);
  EXPECT_EQ(GTE::readData(&ctx, GTE_ORGB), expected);
  EXPECT_EQ(GTE::readData(&ctx, GTE_IRGB), expected) << "IRGB mirrors ORGB";
}

TEST_F(GteOpsTest, LzcrCountsLeadingZeroesOfPositiveSource) {
  GTE::writeData(&ctx, GTE_LZCS, 0x40000000);
  EXPECT_EQ(GTE::readData(&ctx, GTE_LZCR), 1u);
  GTE::writeData(&ctx, GTE_LZCS, 0);
  EXPECT_EQ(GTE::readData(&ctx, GTE_LZCR), 32u);
}

TEST_F(GteOpsTest, LzcrCountsLeadingOnesOfNegativeSource) {
  GTE::writeData(&ctx, GTE_LZCS, 0xFFF00000);
  EXPECT_EQ(GTE::readData(&ctx, GTE_LZCR), 12u);
  GTE::writeData(&ctx, GTE_LZCS, 0x80000000);
  EXPECT_EQ(GTE::readData(&ctx, GTE_LZCR), 1u);
  GTE::writeData(&ctx, GTE_LZCS, 0xFFFFFFFF);
  EXPECT_EQ(GTE::readData(&ctx, GTE_LZCR), 32u);
}

TEST_F(GteOpsTest, LzcsReadsBackAndLzcrIsReadOnly) {
  GTE::writeData(&ctx, GTE_LZCS, 0x00FF0000);
  GTE::writeData(&ctx, GTE_LZCR, 5);
  EXPECT_EQ(GTE::readData(&ctx, GTE_LZCS), 0x00FF0000u);
  EXPECT_EQ(GTE::readData(&ctx, GTE_LZCR), 8u);
}

TEST_F(GteOpsTest, SxypWriteMovesTheScreenXyFifo) {
  GTE::writeData(&ctx, GTE_SXY0, 0x11);
  GTE::writeData(&ctx, GTE_SXY1, 0x22);
  GTE::writeData(&ctx, GTE_SXY2, 0x33);
  GTE::writeData(&ctx, GTE_SXYP, 0x44);
  EXPECT_EQ(ctx.cop2d[GTE_SXY0], 0x22u);
  EXPECT_EQ(ctx.cop2d[GTE_SXY1], 0x33u);
  EXPECT_EQ(ctx.cop2d[GTE_SXY2], 0x44u);
}

TEST_F(GteOpsTest, Sxy2WriteDoesNotMoveTheFifo) {
  GTE::writeData(&ctx, GTE_SXY0, 0x11);
  GTE::writeData(&ctx, GTE_SXY1, 0x22);
  GTE::writeData(&ctx, GTE_SXY2, 0x33);
  EXPECT_EQ(GTE::readData(&ctx, GTE_SXY0), 0x11u);
  EXPECT_EQ(GTE::readData(&ctx, GTE_SXY1), 0x22u);
}

TEST_F(GteOpsTest, ControlRegistersRoundTrip32BitTranslation) {
  GTE::writeControl(&ctx, GTE_TRX, 0x80001234);
  EXPECT_EQ(GTE::readControl(&ctx, GTE_TRX), 0x80001234u);
  GTE::writeControl(&ctx, GTE_OFX, 0x00A00000);
  EXPECT_EQ(GTE::readControl(&ctx, GTE_OFX), 0x00A00000u);
}

// RTPS / RTPT

// Exact UNR quotient for divisors whose normalised value indexes the first
// 47 table entries (indices where the table is verified against psx-spx).
TEST_F(GteOpsTest, RtpsQuotientMatchesUnrDivision) {
  const int32_t divisors[] = {0x8000, 0x8800, 0x9000, 0x9700,
                              0x4400, 0x2300, 0x1100, 0x0480};
  const uint16_t heights[] = {0x0155, 0x03E8, 0x0400};
  for (int32_t sz3 : divisors) {
    for (uint16_t h : heights) {
      setupQuotientProbe(h, sz3);
      GTE::RTPS(&ctx, true, false);
      EXPECT_EQ(ctx.cop2d[GTE_SZ3], static_cast<uint32_t>(sz3));
      EXPECT_EQ(ctx.cop2d[GTE_MAC0], unrDivide(h, sz3))
          << "H=" << h << " SZ3=" << sz3;
      // IR3 = MAC3 = SZ3 saturates once SZ3 exceeds 7FFFh; FLAG.22 is not
      // part of the FLAG.31 summary (bits 30..23 and 18..13 only).
      EXPECT_EQ(flag(), sz3 > 0x7FFF ? bit(22) : 0u);
    }
  }
}

TEST_F(GteOpsTest, RtpsScreenXyIsIrTimesQuotientPlusOffset) {
  setDiag(0, 4096, 4096, 4096);
  setVec(0, 100, -50, 0);
  setTriple(GTE_TRX, 0, 0, 0x8000);
  ctx.cop2c[GTE_H] = 0x4000; // quotient = 8000h (0.5)
  ctx.cop2c[GTE_OFX] = 160u << 16;
  ctx.cop2c[GTE_OFY] = 120u << 16;
  GTE::RTPS(&ctx, true, false);
  EXPECT_EQ(sx(GTE_SXY2), 160 + 50);
  EXPECT_EQ(sy(GTE_SXY2), 120 - 25);
  expectIR(100, -50, 0x7FFF);
}

TEST_F(GteOpsTest, RtpsSaturatesScreenXyAndRaisesErrorFlag) {
  setDiag(0, 4096, 4096, 4096);
  setVec(0, 2000, -2000, 0);
  setTriple(GTE_TRX, 0, 0, 1000);
  ctx.cop2c[GTE_H] = 1000; // quotient = 10000h (1.0)
  GTE::RTPS(&ctx, true, false);
  EXPECT_EQ(sx(GTE_SXY2), 0x3FF);
  EXPECT_EQ(sy(GTE_SXY2), -0x400);
  EXPECT_EQ(flag(), bit(31) | bit(14) | bit(13));

  setVec(0, -2000, 2000, 0);
  GTE::RTPS(&ctx, true, false);
  EXPECT_EQ(sx(GTE_SXY2), -0x400);
  EXPECT_EQ(sy(GTE_SXY2), 0x3FF);
  EXPECT_EQ(flag(), bit(31) | bit(14) | bit(13));
}

TEST_F(GteOpsTest, RtpsNegativeDepthSaturatesSz3AndOverflowsDivide) {
  setupQuotientProbe(0x100, -1000);
  GTE::RTPS(&ctx, true, false);
  EXPECT_EQ(ctx.cop2d[GTE_SZ3], 0u);
  EXPECT_EQ(s(GTE_IR3), -1000);
  EXPECT_EQ(ctx.cop2d[GTE_MAC0], 0x1FFFFu) << "divide saturates to 1FFFFh";
  EXPECT_EQ(flag(), bit(31) | bit(18) | bit(17));
}

TEST_F(GteOpsTest, RtpsFarDepthSaturatesSz3AndIr3) {
  setupQuotientProbe(0x100, 0x12345);
  GTE::RTPS(&ctx, true, false);
  EXPECT_EQ(ctx.cop2d[GTE_SZ3], 0xFFFFu);
  EXPECT_EQ(s(GTE_IR3), 0x7FFF);
  EXPECT_EQ(s(GTE_MAC3), 0x12345);
  EXPECT_EQ(flag(), bit(31) | bit(22) | bit(18));
}

// SZ3 range is 0..FFFFh inclusive: exactly FFFFh is not a saturation.
TEST_F(GteOpsTest, RtpsSz3OfFFFFIsNotSaturated) {
  setupQuotientProbe(0x100, 0xFFFF);
  GTE::RTPS(&ctx, true, false);
  EXPECT_EQ(ctx.cop2d[GTE_SZ3], 0xFFFFu);
  EXPECT_EQ(flag() & bit(18), 0u);
}

TEST_F(GteOpsTest, RtpsDepthCueSaturatesIr0WithoutErrorFlag) {
  setupQuotientProbe(0x100, 0x4000);
  ctx.cop2c[GTE_DQA] = 0;
  ctx.cop2c[GTE_DQB] = 0x2000000; // IR0 = 2000h -> saturated to 1000h
  GTE::RTPS(&ctx, true, false);
  EXPECT_EQ(s(GTE_MAC0), 0x2000000);
  EXPECT_EQ(s(GTE_IR0), 0x1000);
  EXPECT_EQ(flag(), bit(12)) << "FLAG.12 is not part of the error summary";

  ctx.cop2c[GTE_DQB] = static_cast<uint32_t>(-0x1000000);
  GTE::RTPS(&ctx, true, false);
  EXPECT_EQ(s(GTE_IR0), 0);
  EXPECT_EQ(flag(), bit(12));
}

TEST_F(GteOpsTest, RtpsWithSf0KeepsMacUnshiftedAndShiftsSz3) {
  setDiag(0, 4096, 4096, 4096);
  setVec(0, 10, 20, 30);
  ctx.cop2c[GTE_H] = 0x10;
  GTE::RTPS(&ctx, false, false);
  expectMAC(40960, 81920, 122880);
  EXPECT_EQ(ctx.cop2d[GTE_SZ3], 30u) << "SZ3 = MAC3 SAR 12 when sf=0";
  EXPECT_EQ(s(GTE_IR1), 0x7FFF);
  EXPECT_NE(flag() & bit(24), 0u);
}

TEST_F(GteOpsTest, RtpsLmClampsIr1ButLeavesMac1Signed) {
  setDiag(0, 4096, 4096, 4096);
  setVec(0, -100, 0, 0);
  setTriple(GTE_TRX, 0, 0, 0x1000);
  ctx.cop2c[GTE_H] = 0x100;
  GTE::RTPS(&ctx, true, true);
  EXPECT_EQ(s(GTE_MAC1), -100);
  EXPECT_EQ(s(GTE_IR1), 0);
  EXPECT_NE(flag() & bit(24), 0u);
  EXPECT_NE(flag() & bit(31), 0u);
}

TEST_F(GteOpsTest, RtptProjectsThreeVerticesThroughBothFifos) {
  setDiag(0, 4096, 4096, 4096);
  setVec(0, 0x100, 0, 0x1000);
  setVec(1, 0x100, 0, 0x2000);
  setVec(2, 0x100, 0, 0x4000);
  ctx.cop2c[GTE_H] = 0x800;
  ctx.cop2c[GTE_DQA] = 1;
  ctx.cop2d[GTE_SZ3] = 0x777; // oldest stage must survive as SZ0
  GTE::RTPT(&ctx, true, false);
  EXPECT_EQ(ctx.cop2d[GTE_SZ0], 0x777u);
  EXPECT_EQ(ctx.cop2d[GTE_SZ1], 0x1000u);
  EXPECT_EQ(ctx.cop2d[GTE_SZ2], 0x2000u);
  EXPECT_EQ(ctx.cop2d[GTE_SZ3], 0x4000u);
  EXPECT_EQ(sx(GTE_SXY0), 0x80); // 100h * 0.5
  EXPECT_EQ(sx(GTE_SXY1), 0x40); // 100h * 0.25
  EXPECT_EQ(sx(GTE_SXY2), 0x20); // 100h * 0.125
  EXPECT_EQ(ctx.cop2d[GTE_MAC0], unrDivide(0x800, 0x4000))
      << "depth cue uses the last vertex";
}

// NCLIP

TEST_F(GteOpsTest, NclipReturnsTwiceTheSignedArea) {
  ctx.cop2d[GTE_SXY0] = pack(0, 0);
  ctx.cop2d[GTE_SXY1] = pack(100, 0);
  ctx.cop2d[GTE_SXY2] = pack(0, 100);
  GTE::NCLIP(&ctx, false, false);
  EXPECT_EQ(s(GTE_MAC0), 10000);
  EXPECT_EQ(flag(), 0u);
}

TEST_F(GteOpsTest, NclipFlagsMac0PositiveOverflow) {
  ctx.cop2d[GTE_SXY0] = pack(0x7FFF, 0);
  ctx.cop2d[GTE_SXY1] = pack(-0x8000, 0x7FFF);
  ctx.cop2d[GTE_SXY2] = pack(0, -0x8000);
  GTE::NCLIP(&ctx, false, false); // 3221127169 > 2^31-1
  EXPECT_EQ(s(GTE_MAC0), static_cast<int32_t>(3221127169u));
  EXPECT_EQ(flag(), bit(31) | bit(16));
}

TEST_F(GteOpsTest, NclipFlagsMac0NegativeOverflow) {
  ctx.cop2d[GTE_SXY0] = pack(0x7FFF, 0);
  ctx.cop2d[GTE_SXY1] = pack(0, -0x8000);
  ctx.cop2d[GTE_SXY2] = pack(-0x8000, 0x7FFF);
  GTE::NCLIP(&ctx, false, false); // -3221127169 < -2^31
  EXPECT_EQ(s(GTE_MAC0), 1073840127);
  EXPECT_EQ(flag(), bit(31) | bit(15));
}

// AVSZ3 / AVSZ4

TEST_F(GteOpsTest, Avsz3NegativeScaleSaturatesOtzToZero) {
  ctx.cop2c[GTE_ZSF3] = 0xFFFF; // -1
  ctx.cop2d[GTE_SZ1] = 1;
  ctx.cop2d[GTE_SZ2] = 1;
  ctx.cop2d[GTE_SZ3] = 1;
  GTE::AVSZ3(&ctx, false, false);
  EXPECT_EQ(s(GTE_MAC0), -3);
  EXPECT_EQ(ctx.cop2d[GTE_OTZ], 0u);
  EXPECT_EQ(flag(), bit(31) | bit(18));
}

TEST_F(GteOpsTest, Avsz3LargeSumOverflowsMac0AndSaturatesOtz) {
  ctx.cop2c[GTE_ZSF3] = 0x7FFF;
  ctx.cop2d[GTE_SZ1] = 0xFFFF;
  ctx.cop2d[GTE_SZ2] = 0xFFFF;
  ctx.cop2d[GTE_SZ3] = 0xFFFF;
  GTE::AVSZ3(&ctx, false, false);
  EXPECT_EQ(ctx.cop2d[GTE_OTZ], 0xFFFFu);
  EXPECT_EQ(flag(), bit(31) | bit(18) | bit(16));
}

TEST_F(GteOpsTest, Avsz4UsesAllFourStagesAndSaturates) {
  ctx.cop2c[GTE_ZSF4] = static_cast<uint16_t>(-0x100);
  for (uint8_t r = GTE_SZ0; r <= GTE_SZ3; r++)
    ctx.cop2d[r] = 0x10;
  GTE::AVSZ4(&ctx, false, false);
  EXPECT_EQ(s(GTE_MAC0), -0x100 * 0x40);
  EXPECT_EQ(ctx.cop2d[GTE_OTZ], 0u);
  EXPECT_EQ(flag(), bit(31) | bit(18));

  ctx.cop2c[GTE_ZSF4] = 0x7FFF;
  for (uint8_t r = GTE_SZ0; r <= GTE_SZ3; r++)
    ctx.cop2d[r] = 0x8000;
  GTE::AVSZ4(&ctx, false, false);
  EXPECT_EQ(ctx.cop2d[GTE_OTZ], 0xFFFFu);
  EXPECT_EQ(flag(), bit(31) | bit(18) | bit(16));
}

// OP / SQR

TEST_F(GteOpsTest, OpComputesCrossProductWithRotationDiagonal) {
  setDiag(0, 1, 2, 3);
  setIR(4, 5, 6);
  GTE::OP(&ctx, false, false);
  // [IR3*D2-IR2*D3, IR1*D3-IR3*D1, IR2*D1-IR1*D2]
  expectMAC(-3, 6, -3);
  expectIR(-3, 6, -3);
  EXPECT_EQ(flag(), 0u);
}

TEST_F(GteOpsTest, OpShiftsBySfAndClampsNegativesWithLm) {
  setDiag(0, 0x1000, 0x2000, 0x3000);
  setIR(4, 5, 6);
  GTE::OP(&ctx, true, true);
  expectMAC(-3, 6, -3);
  expectIR(0, 6, 0);
  EXPECT_EQ(flag(), bit(31) | bit(24) | bit(22));
}

TEST_F(GteOpsTest, SqrShiftsBySf) {
  setIR(0x1000, -0x2000, 0x800);
  GTE::SQR(&ctx, true, false);
  expectMAC(0x1000, 0x4000, 0x400);
  expectIR(0x1000, 0x4000, 0x400);
}

TEST_F(GteOpsTest, SqrSaturatesIrToMax7FFF) {
  setIR(0x200, 3, -4);
  GTE::SQR(&ctx, false, true);
  expectMAC(0x40000, 9, 16);
  expectIR(0x7FFF, 9, 16);
  EXPECT_EQ(flag(), bit(31) | bit(24));
}

// MVMVA operand selection

TEST_F(GteOpsTest, MvmvaLightMatrixTimesV1PlusBackground) {
  setDiag(8, 4096, 8192, 2048);
  setVec(1, 10, 20, 30);
  setTriple(GTE_RBK, 1, 2, 3);
  GTE::MVMVA(&ctx, 1, 1, 1, true, false);
  expectMAC(11, 42, 18);
  expectIR(11, 42, 18);
}

TEST_F(GteOpsTest, MvmvaColorMatrixTimesV2WithoutTranslation) {
  const int16_t lcm[3][3] = {{4096, 4096, 0}, {0, 4096, 4096}, {4096, 0, 4096}};
  setMatrix(16, lcm);
  setVec(2, 1, 2, 3);
  setTriple(GTE_TRX, 100, 100, 100);
  setTriple(GTE_RBK, 100, 100, 100);
  GTE::MVMVA(&ctx, 2, 2, 3, true, false);
  expectMAC(3, 5, 4);
}

TEST_F(GteOpsTest, MvmvaRotationTimesIrVectorPlusTranslation) {
  setDiag(0, 4096, 4096, 4096);
  setIR(5, 6, 7);
  setTriple(GTE_TRX, 100, 0, 0);
  GTE::MVMVA(&ctx, 0, 3, 0, true, false);
  expectMAC(105, 6, 7);
}

TEST_F(GteOpsTest, MvmvaLmClampsNegativeIrToZero) {
  setDiag(0, 4096, 4096, 4096);
  setVec(0, -5, 6, -7);
  GTE::MVMVA(&ctx, 0, 0, 3, true, true);
  expectMAC(-5, 6, -7);
  expectIR(0, 6, 0);
  EXPECT_EQ(flag(), bit(31) | bit(24) | bit(22));
}

TEST_F(GteOpsTest, MvmvaSaturatesIrToSigned16) {
  setTriple(GTE_TRX, 0x8000, -0x9000, 0);
  GTE::MVMVA(&ctx, 0, 0, 0, true, false);
  expectMAC(0x8000, -0x9000, 0);
  expectIR(0x7FFF, -0x8000, 0);
  EXPECT_EQ(flag(), bit(31) | bit(24) | bit(23));
}

// FLAG.30: MAC1 larger than 43 bits and positive.
TEST_F(GteOpsTest, GplFlagsMac1BeyondFortyThreeBits) {
  ctx.cop2d[GTE_MAC1] = 0x7FFFFFFF;
  ctx.cop2d[GTE_IR0] = 0x7FFF;
  setIR(0x7FFF, 0, 0);
  GTE::GPL(&ctx, true, false); // (7FFFFFFFh SHL 12) + 7FFFh*7FFFh > 2^43-1
  EXPECT_NE(flag() & bit(30), 0u);
  EXPECT_NE(flag() & bit(31), 0u);
}

// GPF / GPL and the color FIFO

TEST_F(GteOpsTest, GpfPushesColorFifoAndSaturatesChannels) {
  ctx.cop2d[GTE_IR0] = 0x1000;
  setIR(0x800, 0x1800, -0x100);
  ctx.cop2d[GTE_RGBC] = rgb(0, 0, 0, 0x2C);
  ctx.cop2d[GTE_RGB1] = 0x11111111;
  ctx.cop2d[GTE_RGB2] = 0x22222222;
  GTE::GPF(&ctx, true, false);
  expectMAC(0x800, 0x1800, -0x100);
  EXPECT_EQ(ctx.cop2d[GTE_RGB0], 0x11111111u);
  EXPECT_EQ(ctx.cop2d[GTE_RGB1], 0x22222222u);
  EXPECT_EQ(ctx.cop2d[GTE_RGB2], rgb(0x80, 0xFF, 0x00, 0x2C));
  // Color saturation (FLAG.20/19) is not part of the FLAG.31 summary.
  EXPECT_EQ(flag(), bit(20) | bit(19));
}

TEST_F(GteOpsTest, GplAddsToExistingMacScaledBySf) {
  ctx.cop2d[GTE_MAC1] = 100;
  ctx.cop2d[GTE_MAC2] = static_cast<uint32_t>(-50);
  ctx.cop2d[GTE_MAC3] = 0;
  ctx.cop2d[GTE_IR0] = 0x1000;
  setIR(50, 50, 0x10);
  GTE::GPL(&ctx, true, false);
  expectMAC(150, 0, 16);
  EXPECT_EQ(ctx.cop2d[GTE_RGB2], rgb(150 / 16, 0, 1, 0));
}

TEST_F(GteOpsTest, GplWithSf0AccumulatesUnscaled) {
  ctx.cop2d[GTE_MAC1] = 100;
  ctx.cop2d[GTE_IR0] = 2;
  setIR(3, 0, 0);
  GTE::GPL(&ctx, false, false);
  EXPECT_EQ(s(GTE_MAC1), 106);
}

// Depth cueing family: MAC + (FC - MAC) * IR0

TEST_F(GteOpsTest, IntplInterpolatesIrTowardFarColor) {
  ctx.cop2d[GTE_IR0] = 0x800; // 0.5
  setIR(0x100, 0x200, 0x300);
  setTriple(GTE_RFC, 0x300, 0x200, 0x100);
  ctx.cop2d[GTE_RGBC] = rgb(0, 0, 0, 0x30);
  GTE::INTPL(&ctx, true, false);
  expectMAC(0x200, 0x200, 0x200);
  EXPECT_EQ(ctx.cop2d[GTE_RGB2], rgb(0x20, 0x20, 0x20, 0x30));
}

TEST_F(GteOpsTest, DpcsWithIr0ZeroKeepsPrimaryColor) {
  ctx.cop2d[GTE_RGBC] = rgb(0x40, 0x80, 0xC0, 0x38);
  setTriple(GTE_RFC, 0x800, 0x800, 0x800);
  ctx.cop2d[GTE_IR0] = 0;
  GTE::DPCS(&ctx, true, false);
  expectMAC(0x400, 0x800, 0xC00);
  EXPECT_EQ(ctx.cop2d[GTE_RGB2], rgb(0x40, 0x80, 0xC0, 0x38));
}

TEST_F(GteOpsTest, DpcsWithIr0OneReachesFarColor) {
  ctx.cop2d[GTE_RGBC] = rgb(0x40, 0x80, 0xC0, 0x38);
  setTriple(GTE_RFC, 0x800, 0x800, 0x800);
  ctx.cop2d[GTE_IR0] = 0x1000;
  GTE::DPCS(&ctx, true, false);
  expectMAC(0x800, 0x800, 0x800);
  EXPECT_EQ(ctx.cop2d[GTE_RGB2], rgb(0x80, 0x80, 0x80, 0x38));
}

// DPCT reads RGB0 three times; with IR0=0 the FIFO comes back in order with
// CODE taken from RGBC.
TEST_F(GteOpsTest, DpctCyclesTheColorFifoFromItsBottom) {
  ctx.cop2d[GTE_RGB0] = rgb(0x30, 0x20, 0x10, 0);
  ctx.cop2d[GTE_RGB1] = rgb(0x60, 0x50, 0x40, 0);
  ctx.cop2d[GTE_RGB2] = rgb(0x90, 0x80, 0x70, 0);
  ctx.cop2d[GTE_RGBC] = rgb(0, 0, 0, 0x3C);
  ctx.cop2d[GTE_IR0] = 0;
  GTE::DPCT(&ctx, true, false);
  EXPECT_EQ(ctx.cop2d[GTE_RGB0], rgb(0x30, 0x20, 0x10, 0x3C));
  EXPECT_EQ(ctx.cop2d[GTE_RGB1], rgb(0x60, 0x50, 0x40, 0x3C));
  EXPECT_EQ(ctx.cop2d[GTE_RGB2], rgb(0x90, 0x80, 0x70, 0x3C));
}

TEST_F(GteOpsTest, DcplScalesPrimaryColorByIrThenDepthCues) {
  ctx.cop2d[GTE_RGBC] = rgb(0x80, 0x40, 0x20, 0x3A);
  setIR(0x1000, 0x1000, 0x800);
  ctx.cop2d[GTE_IR0] = 0x800; // halfway to black far color
  GTE::DCPL(&ctx, true, false);
  expectMAC(0x400, 0x200, 0x80);
  EXPECT_EQ(ctx.cop2d[GTE_RGB2], rgb(0x40, 0x20, 0x08, 0x3A));
}

TEST_F(GteOpsTest, CdpWithZeroInputFadesToFarColorByIr0) {
  setDiag(16, 4096, 4096, 4096);
  setTriple(GTE_RFC, 0x800, 0x400, 0xC00);
  ctx.cop2d[GTE_IR0] = 0x800;
  ctx.cop2d[GTE_RGBC] = rgb(0xFF, 0xFF, 0xFF, 0x36);
  GTE::CDP(&ctx, true, true);
  expectMAC(0x400, 0x200, 0x600);
  EXPECT_EQ(ctx.cop2d[GTE_RGB2], rgb(0x40, 0x20, 0x60, 0x36));
}

// Lighting: LLM*V0, then BK + LCM*IR, then optional RGBC / depth cue.
// BK is zero where the light-matrix step would otherwise observe it.

TEST_F(GteOpsTest, NcsLightsNormalThroughBothMatrices) {
  setDiag(8, 4096, 4096, 4096);
  setDiag(16, 4096, 4096, 4096);
  setVec(0, 0x800, 0x400, 0x200);
  ctx.cop2d[GTE_RGBC] = rgb(0, 0, 0, 0x30);
  GTE::NCS(&ctx, true, true);
  expectMAC(0x800, 0x400, 0x200);
  EXPECT_EQ(ctx.cop2d[GTE_RGB2], rgb(0x80, 0x40, 0x20, 0x30));
  EXPECT_EQ(flag(), 0u);
}

TEST_F(GteOpsTest, NcsBackfacingNormalWithLmIsBlack) {
  setDiag(8, 4096, 4096, 4096);
  setDiag(16, 4096, 4096, 4096);
  setVec(0, -0x800, 0, 0);
  GTE::NCS(&ctx, true, true);
  EXPECT_EQ(s(GTE_IR1), 0);
  EXPECT_EQ(ctx.cop2d[GTE_RGB2] & 0xFFFFFF, 0u);
  EXPECT_NE(flag() & bit(24), 0u);
}

TEST_F(GteOpsTest, NctLightsThreeNormalsInFifoOrder) {
  setDiag(8, 4096, 4096, 4096);
  setDiag(16, 4096, 4096, 4096);
  setVec(0, 0x800, 0, 0);
  setVec(1, 0, 0x800, 0);
  setVec(2, 0, 0, 0x800);
  GTE::NCT(&ctx, true, true);
  EXPECT_EQ(ctx.cop2d[GTE_RGB0], rgb(0x80, 0, 0, 0));
  EXPECT_EQ(ctx.cop2d[GTE_RGB1], rgb(0, 0x80, 0, 0));
  EXPECT_EQ(ctx.cop2d[GTE_RGB2], rgb(0, 0, 0x80, 0));
}

TEST_F(GteOpsTest, NccsMultipliesLightByPrimaryColor) {
  setDiag(8, 4096, 4096, 4096);
  setDiag(16, 4096, 4096, 4096);
  setVec(0, 0x1000, 0x800, 0x1000);
  ctx.cop2d[GTE_RGBC] = rgb(0x80, 0x40, 0xFF, 0x34);
  GTE::NCCS(&ctx, true, true);
  expectMAC(0x800, 0x200, 0xFF0);
  EXPECT_EQ(ctx.cop2d[GTE_RGB2], rgb(0x80, 0x20, 0xFF, 0x34));
}

TEST_F(GteOpsTest, NcctColorsThreeNormals) {
  setDiag(8, 4096, 4096, 4096);
  setDiag(16, 4096, 4096, 4096);
  setVec(0, 0x1000, 0, 0);
  setVec(1, 0, 0x1000, 0);
  setVec(2, 0, 0, 0x1000);
  ctx.cop2d[GTE_RGBC] = rgb(0x80, 0x40, 0x20, 0);
  GTE::NCCT(&ctx, true, true);
  EXPECT_EQ(ctx.cop2d[GTE_RGB0], rgb(0x80, 0, 0, 0));
  EXPECT_EQ(ctx.cop2d[GTE_RGB1], rgb(0, 0x40, 0, 0));
  EXPECT_EQ(ctx.cop2d[GTE_RGB2], rgb(0, 0, 0x20, 0));
}

TEST_F(GteOpsTest, CcAddsBackgroundThenMultipliesByPrimaryColor) {
  setDiag(16, 4096, 4096, 4096);
  setIR(0x1000, 0x1000, 0x1000);
  setTriple(GTE_RBK, 0x100, 0, 0);
  ctx.cop2d[GTE_RGBC] = rgb(0x80, 0x40, 0x20, 0x32);
  GTE::CC(&ctx, true, true);
  expectMAC(0x880, 0x400, 0x200);
  EXPECT_EQ(ctx.cop2d[GTE_RGB2], rgb(0x88, 0x40, 0x20, 0x32));
}

// Unlit normal (zero light matrix): NCDS reduces to FC * IR0.
TEST_F(GteOpsTest, NcdsUnlitNormalFadesFullyToFarColor) {
  setDiag(16, 4096, 4096, 4096);
  setVec(0, 0x1000, 0x1000, 0x1000);
  setTriple(GTE_RFC, 0x800, 0x400, 0xC00);
  ctx.cop2d[GTE_IR0] = 0x1000;
  ctx.cop2d[GTE_RGBC] = rgb(0xFF, 0xFF, 0xFF, 0x30);
  GTE::NCDS(&ctx, true, true);
  expectMAC(0x800, 0x400, 0xC00);
  EXPECT_EQ(ctx.cop2d[GTE_RGB2], rgb(0x80, 0x40, 0xC0, 0x30));
}

TEST_F(GteOpsTest, NcdtUnlitNormalsFadeToFarColor) {
  setDiag(16, 4096, 4096, 4096);
  setTriple(GTE_RFC, 0x100, 0x200, 0x300);
  ctx.cop2d[GTE_IR0] = 0x1000;
  ctx.cop2d[GTE_RGBC] = rgb(0xFF, 0xFF, 0xFF, 0x38);
  GTE::NCDT(&ctx, true, true);
  expectMAC(0x100, 0x200, 0x300);
  EXPECT_EQ(ctx.cop2d[GTE_RGB2], rgb(0x10, 0x20, 0x30, 0x38));
}

// psx-spx "GTE Saturation": FLAG bit 31 is the OR of bits 30..23 and 18..13,
// and FLAG is reset at the start of every command.  Every command must raise
// it when a stage saturates IRn, and leave it clear on in-range input.
struct GteCommandCase {
  const char *name;
  void (*run)(CPUContext *, bool, bool);
  bool sf;
};

class GteErrorFlagTest : public GteOpsTest,
                         public ::testing::WithParamInterface<GteCommandCase> {
protected:
  // Far colour and background 10000h: one step that reaches them gives
  // IR = 10000h > 7FFFh.  Projection: SX = 2000 * (H/SZ=1.0) > 3FFh.
  void setupSaturatingInputs() {
    setTriple(GTE_RFC, 0x10000, 0x10000, 0x10000);
    setTriple(GTE_RBK, 0x10000, 0x10000, 0x10000);
    ctx.cop2d[GTE_IR0] = 0x1000;
    setIR(0x7FFF, 0x7FFF, 0x7FFF);
    ctx.cop2d[GTE_RGBC] = rgb(0xFF, 0xFF, 0xFF, 0x30);
    for (int i = 0; i < 3; ++i)
      ctx.cop2d[GTE_RGB0 + i] = rgb(0xFF, 0xFF, 0xFF, 0x30);
    setDiag(0, 4096, 4096, 4096);
    setDiag(8, 4096, 4096, 4096);
    setDiag(16, 4096, 4096, 4096);
    for (int v = 0; v < 3; ++v)
      setVec(v, 2000, 2000, 0);
    setTriple(GTE_TRX, 0, 0, 1000);
    ctx.cop2c[GTE_H] = 1000;
  }
};

TEST_P(GteErrorFlagTest, SaturatingStageRaisesTheErrorFlag) {
  setupSaturatingInputs();

  GetParam().run(&ctx, GetParam().sf, /*lm=*/false);

  EXPECT_NE(flag() & 0x7F87E000u, 0u);
  EXPECT_NE(flag() & bit(31), 0u);
}

TEST_P(GteErrorFlagTest, InRangeInputLeavesTheErrorFlagClear) {
  ctx.cop2c[GTE_FLAG] = bit(31) | bit(24); // stale flags from a prior op
  setTriple(GTE_TRX, 0, 0, 1000);           // RTPT: SZ 1000, no divide overflow
  ctx.cop2c[GTE_H] = 1000;

  GetParam().run(&ctx, GetParam().sf, /*lm=*/false); // otherwise all zero

  EXPECT_EQ(flag() & bit(31), 0u);
}

INSTANTIATE_TEST_SUITE_P(
    AllCommands, GteErrorFlagTest,
    ::testing::Values(GteCommandCase{"RTPT", &GTE::RTPT, true},
                      GteCommandCase{"DPCS", &GTE::DPCS, true},
                      GteCommandCase{"DPCT", &GTE::DPCT, true},
                      GteCommandCase{"INTPL", &GTE::INTPL, true},
                      GteCommandCase{"NCDS", &GTE::NCDS, true},
                      GteCommandCase{"NCDT", &GTE::NCDT, true},
                      GteCommandCase{"CDP", &GTE::CDP, true},
                      GteCommandCase{"NCCS", &GTE::NCCS, true},
                      GteCommandCase{"NCCT", &GTE::NCCT, true},
                      GteCommandCase{"CC", &GTE::CC, true},
                      GteCommandCase{"NCT", &GTE::NCT, true},
                      GteCommandCase{"DCPL", &GTE::DCPL, true},
                      GteCommandCase{"GPF", &GTE::GPF, false}),
    [](const ::testing::TestParamInfo<GteCommandCase> &info) {
      return std::string(info.param.name);
    });
