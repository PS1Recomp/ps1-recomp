// Tests: GPU command processing -- render primitives, VRAM transfers, GP1
// display control and GPUREAD, checked against psx-spx "Graphics Processing
// Unit (GPU)" by reading exact VRAM pixels and status bits.
#include "runtime/gpu/gpu.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <unistd.h>
#include <vector>

using namespace ps1::gpu;

namespace {

constexpr uint16_t rgb15(int r, int g, int b) {
  return static_cast<uint16_t>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
}
constexpr uint32_t xy(int x, int y) {
  return (static_cast<uint32_t>(y & 0xFFFF) << 16) | (x & 0xFFFF);
}
int r5(uint16_t c) { return c & 0x1F; }
int g5(uint16_t c) { return (c >> 5) & 0x1F; }
int b5(uint16_t c) { return (c >> 10) & 0x1F; }

} // namespace

class GpuPrimitivesTest : public ::testing::Test {
protected:
  GPU gpu;

  void SetUp() override {
    gpu.reset();
    gpu.writeGP0(0xE1000000); // texpage 0, blend mode 0, dither off
  }

  uint16_t px(int x, int y) const {
    return gpu.getVRAM()[y * GPU::VRAM_WIDTH + x].raw;
  }

  // GP0(A0h) CPU-to-VRAM: fill a w*h rectangle with one halfword value.
  void upload(int x, int y, int w, int h, uint16_t value) {
    gpu.writeGP0(0xA0000000);
    gpu.writeGP0(xy(x, y));
    gpu.writeGP0(xy(w, h));
    const int words = (w * h + 1) / 2;
    for (int i = 0; i < words; ++i)
      gpu.writeGP0(value | (static_cast<uint32_t>(value) << 16));
  }

  uint32_t readRegister(uint32_t index) {
    gpu.writeGP1(0x10000000 | index);
    return gpu.readGPUREAD();
  }
};

// ---------------------------------------------------------------------------
// Gouraud polygons (psx-spx "GPU Render Polygon Commands")
// ---------------------------------------------------------------------------

TEST_F(GpuPrimitivesTest, GouraudTriangleTakesEachVertexColourAtItsVertex) {
  gpu.writeGP0(0x300000FF); // c0 red + cmd
  gpu.writeGP0(xy(0, 0));
  gpu.writeGP0(0x0000FF00); // c1 green
  gpu.writeGP0(xy(32, 0));
  gpu.writeGP0(0x00FF0000); // c2 blue
  gpu.writeGP0(xy(0, 32));

  EXPECT_EQ(px(0, 0), rgb15(255, 0, 0));
  // Halfway along the red->green edge both channels sit near the middle.
  const uint16_t mid = px(16, 0);
  EXPECT_NEAR(r5(mid), 16, 1);
  EXPECT_NEAR(g5(mid), 16, 1);
  EXPECT_EQ(b5(mid), 0);
  // Close to the blue vertex, blue dominates.
  const uint16_t nearBlue = px(1, 30);
  EXPECT_GT(b5(nearBlue), 25);
  EXPECT_LT(r5(nearBlue), 6);
  EXPECT_EQ(px(40, 0), 0); // outside the triangle
}

TEST_F(GpuPrimitivesTest, GouraudQuadUsesFourthVertexColourForSecondTriangle) {
  // psx-spx: quads are vertices 1,2,3 then 2,3,4.
  gpu.writeGP0(0x380000FF);
  gpu.writeGP0(xy(100, 100));
  gpu.writeGP0(0x000000FF);
  gpu.writeGP0(xy(132, 100));
  gpu.writeGP0(0x000000FF);
  gpu.writeGP0(xy(100, 132));
  gpu.writeGP0(0x00FF0000); // c3 blue
  gpu.writeGP0(xy(132, 132));

  EXPECT_EQ(px(101, 101), rgb15(255, 0, 0)); // first triangle is all red
  const uint16_t nearV3 = px(130, 130);
  EXPECT_GT(b5(nearV3), r5(nearV3));
  EXPECT_EQ(g5(nearV3), 0);
}

TEST_F(GpuPrimitivesTest, SemiTransparentGouraudTriangleBlendsWithBackground) {
  gpu.writeGP0(0xE1000000 | (1 << 5)); // mode 1: B+F
  gpu.writeGP0(0x02000080);           // background r=128
  gpu.writeGP0(xy(0, 0));
  gpu.writeGP0(xy(64, 64));

  gpu.writeGP0(0x32000040); // shaded, semi-transparent, c0 r=64
  gpu.writeGP0(xy(0, 0));
  gpu.writeGP0(0x00000040);
  gpu.writeGP0(xy(40, 0));
  gpu.writeGP0(0x00000040);
  gpu.writeGP0(xy(0, 40));

  EXPECT_EQ(px(5, 5), rgb15(192, 0, 0));
  EXPECT_EQ(px(50, 50), rgb15(128, 0, 0)); // background outside untouched
}

TEST_F(GpuPrimitivesTest, ClockwiseAndCounterClockwiseTrianglesDrawTheSame) {
  // psx-spx: "the ordering of the vertices doesn't matter on the GPU side".
  gpu.writeGP0(0x200000FF);
  gpu.writeGP0(xy(0, 0));
  gpu.writeGP0(xy(20, 0));
  gpu.writeGP0(xy(0, 20));
  gpu.writeGP0(0x2000FF00);
  gpu.writeGP0(xy(100, 0));
  gpu.writeGP0(xy(100, 20));
  gpu.writeGP0(xy(120, 0));

  EXPECT_EQ(px(5, 5), rgb15(255, 0, 0));
  EXPECT_EQ(px(105, 5), rgb15(0, 255, 0));
}

TEST_F(GpuPrimitivesTest, DegenerateTriangleDrawsNothing) {
  gpu.writeGP0(0x200000FF);
  gpu.writeGP0(xy(0, 0));
  gpu.writeGP0(xy(10, 10));
  gpu.writeGP0(xy(20, 20));
  EXPECT_EQ(px(10, 10), 0);
  EXPECT_EQ(px(0, 0), 0);
}

TEST_F(GpuPrimitivesTest, PolygonIsClippedToDrawingArea) {
  gpu.writeGP0(0xE3000000 | (10 << 10) | 10); // top-left (10,10)
  gpu.writeGP0(0xE4000000 | (20 << 10) | 20); // bottom-right (20,20)
  gpu.writeGP0(0x280000FF);
  gpu.writeGP0(xy(0, 0));
  gpu.writeGP0(xy(40, 0));
  gpu.writeGP0(xy(0, 40));
  gpu.writeGP0(xy(40, 40));

  EXPECT_EQ(px(15, 15), rgb15(255, 0, 0));
  EXPECT_EQ(px(20, 20), rgb15(255, 0, 0)); // E4 corner is inclusive
  EXPECT_EQ(px(9, 15), 0);
  EXPECT_EQ(px(21, 15), 0);
  EXPECT_EQ(px(15, 21), 0);
}

TEST_F(GpuPrimitivesTest, DrawingOffsetIsSignExtendedAndAddedToVertices) {
  // GP0(E5h): 11-bit signed X/Y. -8 in X, +16 in Y.
  gpu.writeGP0(0xE5000000 | (16 << 11) | (0x800 - 8));
  gpu.writeGP0(0x680000FF); // 1x1 dot
  gpu.writeGP0(xy(108, 4));
  EXPECT_EQ(px(100, 20), rgb15(255, 0, 0));
  EXPECT_EQ(readRegister(5), (16u << 11) | (0x800 - 8));
}

// ---------------------------------------------------------------------------
// Dithering (psx-spx "Dither enable": gouraud polygons only, 4x4 matrix)
// ---------------------------------------------------------------------------

TEST_F(GpuPrimitivesTest, DitheredGouraudTriangleAppliesFourByFourMatrix) {
  gpu.writeGP0(0xE1000000 | (1 << 9));
  // 104 is exactly 13<<3: matrix offset -4 drops to 12, +1/+3 stays 13.
  const uint32_t grey = 0x00686868;
  gpu.writeGP0(0x30000000 | grey);
  gpu.writeGP0(xy(0, 0));
  gpu.writeGP0(grey);
  gpu.writeGP0(xy(64, 0));
  gpu.writeGP0(grey);
  gpu.writeGP0(xy(0, 64));

  EXPECT_EQ(r5(px(4, 4)), 12); // matrix[0][0] = -4
  EXPECT_EQ(r5(px(7, 4)), 13); // matrix[0][3] = +1
  EXPECT_EQ(r5(px(5, 5)), 12); // matrix[1][1] = -2 -> 102 -> 12
  EXPECT_EQ(r5(px(6, 5)), 13); // matrix[1][2] = +3
}

TEST_F(GpuPrimitivesTest, DitherClampsNegativeChannelsToZero) {
  gpu.writeGP0(0xE1000000 | (1 << 9));
  gpu.writeGP0(0x30000000); // black
  gpu.writeGP0(xy(0, 0));
  gpu.writeGP0(0);
  gpu.writeGP0(xy(16, 0));
  gpu.writeGP0(0);
  gpu.writeGP0(xy(0, 16));
  EXPECT_EQ(px(4, 4), 0); // 0 - 4 clamps to 0, not to 0x1F
}

// ---------------------------------------------------------------------------
// Textured polygons, 8-bit and 15-bit pages, flat and gouraud
// ---------------------------------------------------------------------------

namespace {
constexpr uint16_t kTpage15 = 1 | (2 << 7); // X base 64, 15-bit direct
constexpr uint16_t kTpage8 = 2 | (1 << 7);  // X base 128, 8-bit CLUT
constexpr uint16_t kClut = (500 << 6) | 0;  // CLUT at (0,500)
} // namespace

TEST_F(GpuPrimitivesTest, RawTexturedQuadCopies15BitTexels) {
  upload(64, 0, 32, 32, rgb15(0, 248, 0));
  gpu.writeGP0(0x2D808080); // textured quad, raw
  gpu.writeGP0(xy(200, 200));
  gpu.writeGP0(0);                                   // uv0, clut
  gpu.writeGP0(xy(216, 200));
  gpu.writeGP0((static_cast<uint32_t>(kTpage15) << 16) | 16); // uv1, tpage
  gpu.writeGP0(xy(200, 216));
  gpu.writeGP0(16 << 8);
  gpu.writeGP0(xy(216, 216));
  gpu.writeGP0((16 << 8) | 16);

  EXPECT_EQ(px(205, 205), rgb15(0, 248, 0));
  EXPECT_EQ(px(214, 214), rgb15(0, 248, 0)); // second triangle
  EXPECT_EQ(px(220, 220), 0);
}

TEST_F(GpuPrimitivesTest, TexturedTriangleSamples8BitClutTexels) {
  upload(0, 500, 16, 1, 0);
  gpu.writeGP0(0xA0000000); // CLUT entry 5 = blue
  gpu.writeGP0(xy(5, 500));
  gpu.writeGP0(xy(1, 1));
  gpu.writeGP0(rgb15(0, 0, 248));
  upload(128, 0, 16, 32, 0x0505); // every 8-bit index is 5

  gpu.writeGP0(0x25808080); // textured triangle, raw
  gpu.writeGP0(xy(300, 10));
  gpu.writeGP0(static_cast<uint32_t>(kClut) << 16);
  gpu.writeGP0(xy(330, 10));
  gpu.writeGP0((static_cast<uint32_t>(kTpage8) << 16) | 30);
  gpu.writeGP0(xy(300, 40));
  gpu.writeGP0(30 << 8);

  EXPECT_EQ(px(305, 15), rgb15(0, 0, 248));
}

TEST_F(GpuPrimitivesTest, GouraudTexturedTriangleModulatesTexelByVertexColour) {
  upload(64, 0, 32, 32, rgb15(248, 248, 248));
  const uint32_t half = 0x00404040; // 0x40/0x80 = half intensity
  gpu.writeGP0(0x34000000 | half);
  gpu.writeGP0(xy(0, 100));
  gpu.writeGP0(0);
  gpu.writeGP0(half);
  gpu.writeGP0(xy(30, 100));
  gpu.writeGP0((static_cast<uint32_t>(kTpage15) << 16) | 30);
  gpu.writeGP0(half);
  gpu.writeGP0(xy(0, 130));
  gpu.writeGP0(30 << 8);

  EXPECT_EQ(px(5, 105), rgb15(124, 124, 124));
}

TEST_F(GpuPrimitivesTest, RawGouraudTexturedQuadIgnoresVertexColours) {
  upload(64, 0, 32, 32, rgb15(248, 0, 248));
  gpu.writeGP0(0x3D000000); // raw: vertex colours (black) are not applied
  gpu.writeGP0(xy(0, 200));
  gpu.writeGP0(0);
  gpu.writeGP0(0);
  gpu.writeGP0(xy(16, 200));
  gpu.writeGP0((static_cast<uint32_t>(kTpage15) << 16) | 16);
  gpu.writeGP0(0);
  gpu.writeGP0(xy(0, 216));
  gpu.writeGP0(16 << 8);
  gpu.writeGP0(0);
  gpu.writeGP0(xy(16, 216));
  gpu.writeGP0((16 << 8) | 16);

  EXPECT_EQ(px(3, 203), rgb15(248, 0, 248));
  EXPECT_EQ(px(14, 214), rgb15(248, 0, 248));
}

TEST_F(GpuPrimitivesTest, GouraudTexturedQuadSamples8BitAnd4BitPages) {
  // 8-bit page: index 5 -> blue.
  upload(0, 500, 16, 1, 0);
  gpu.writeGP0(0xA0000000);
  gpu.writeGP0(xy(5, 500));
  gpu.writeGP0(xy(1, 1));
  gpu.writeGP0(rgb15(0, 0, 248));
  upload(128, 0, 16, 32, 0x0505);
  const uint32_t white = 0x00808080;
  auto quad = [&](int x0, uint16_t tpage) {
    gpu.writeGP0(0x3C000000 | white);
    gpu.writeGP0(xy(x0, 300));
    gpu.writeGP0(static_cast<uint32_t>(kClut) << 16);
    gpu.writeGP0(white);
    gpu.writeGP0(xy(x0 + 16, 300));
    gpu.writeGP0((static_cast<uint32_t>(tpage) << 16) | 16);
    gpu.writeGP0(white);
    gpu.writeGP0(xy(x0, 316));
    gpu.writeGP0(16 << 8);
    gpu.writeGP0(white);
    gpu.writeGP0(xy(x0 + 16, 316));
    gpu.writeGP0((16 << 8) | 16);
  };
  quad(0, kTpage8);
  // 4-bit page at X base 192: every nibble is index 5 as well.
  upload(192, 0, 8, 32, 0x5555);
  quad(100, 3); // tpage 3 = X base 192, 4-bit

  EXPECT_EQ(px(4, 304), rgb15(0, 0, 248));
  EXPECT_EQ(px(104, 304), rgb15(0, 0, 248));
}

TEST_F(GpuPrimitivesTest, SemiTransparentGouraudTexturedBlendsOnlyFlaggedTexels) {
  upload(64, 0, 32, 32, 0x8000 | rgb15(128, 0, 0));
  gpu.writeGP0(0x02000080); // background r=128
  gpu.writeGP0(xy(0, 400));
  gpu.writeGP0(xy(32, 32));
  const uint16_t tpageAdd = kTpage15 | (1 << 5); // mode 1: B+F
  const uint32_t white = 0x00808080;
  gpu.writeGP0(0x36000000 | white);
  gpu.writeGP0(xy(0, 400));
  gpu.writeGP0(0);
  gpu.writeGP0(white);
  gpu.writeGP0(xy(30, 400));
  gpu.writeGP0((static_cast<uint32_t>(tpageAdd) << 16) | 30);
  gpu.writeGP0(white);
  gpu.writeGP0(xy(0, 430));
  gpu.writeGP0(30 << 8);

  EXPECT_EQ(px(4, 404) & 0x7FFF, rgb15(255, 0, 0)); // 128 + 128 saturates
}

TEST_F(GpuPrimitivesTest, TextureWindowRepeatsTexelsInsideWindow) {
  // GP0(E2h) mask 1 (8 px), offset 0: u=8..15 wraps back onto u=0..7.
  upload(64, 0, 8, 1, rgb15(248, 0, 0));
  upload(72, 0, 8, 1, rgb15(0, 248, 0));
  gpu.writeGP0(0xE2000000 | 1 | (1 << 5));
  gpu.writeGP0(0xE1000000 | kTpage15);
  gpu.writeGP0(0x65000000); // variable textured rect, raw
  gpu.writeGP0(xy(500, 0));
  gpu.writeGP0(0); // uv base (0,0)
  gpu.writeGP0(xy(16, 1));

  EXPECT_EQ(px(500, 0), rgb15(248, 0, 0));
  EXPECT_EQ(px(510, 0), rgb15(248, 0, 0)); // u=10 masked to 2
}

// ---------------------------------------------------------------------------
// Rectangles (psx-spx "GPU Render Rectangle Commands")
// ---------------------------------------------------------------------------

TEST_F(GpuPrimitivesTest, VariableRectangleCoversExactlyWidthByHeight) {
  gpu.writeGP0(0x6000FF00);
  gpu.writeGP0(xy(10, 10));
  gpu.writeGP0(xy(3, 2));
  EXPECT_EQ(px(10, 10), rgb15(0, 255, 0));
  EXPECT_EQ(px(12, 11), rgb15(0, 255, 0));
  EXPECT_EQ(px(13, 10), 0);
  EXPECT_EQ(px(10, 12), 0);
}

TEST_F(GpuPrimitivesTest, EightByEightRectangleCoversSixtyFourPixels) {
  gpu.writeGP0(0x70FF0000);
  gpu.writeGP0(xy(40, 40));
  EXPECT_EQ(px(40, 40), rgb15(0, 0, 255));
  EXPECT_EQ(px(47, 47), rgb15(0, 0, 255));
  EXPECT_EQ(px(48, 47), 0);
  EXPECT_EQ(px(47, 48), 0);
}

TEST_F(GpuPrimitivesTest, SemiTransparentRectangleSubtractsInModeTwo) {
  gpu.writeGP0(0xE1000000 | (2 << 5)); // B-F
  gpu.writeGP0(0x020000C0);
  gpu.writeGP0(xy(0, 0));
  gpu.writeGP0(xy(16, 16));
  gpu.writeGP0(0x62000040); // variable rect, semi-transparent, r=64
  gpu.writeGP0(xy(0, 0));
  gpu.writeGP0(xy(4, 4));
  EXPECT_EQ(px(1, 1), rgb15(128, 0, 0));
  EXPECT_EQ(px(8, 8), rgb15(192, 0, 0));
}

TEST_F(GpuPrimitivesTest, RectangleIsClippedToDrawingArea) {
  gpu.writeGP0(0xE3000000 | (0 << 10) | 100);
  gpu.writeGP0(0xE4000000 | (511 << 10) | 103);
  gpu.writeGP0(0x780000FF); // 16x16 straddling x=100..103
  gpu.writeGP0(xy(96, 0));
  EXPECT_EQ(px(99, 0), 0);
  EXPECT_EQ(px(100, 0), rgb15(255, 0, 0));
  EXPECT_EQ(px(103, 15), rgb15(255, 0, 0));
  EXPECT_EQ(px(104, 0), 0);
}

TEST_F(GpuPrimitivesTest, RectangleEntirelyOutsideDrawingAreaDrawsNothing) {
  gpu.writeGP0(0xE3000000 | (0 << 10) | 0);
  gpu.writeGP0(0xE4000000 | (50 << 10) | 50);
  gpu.writeGP0(0x700000FF);
  gpu.writeGP0(xy(200, 200));
  EXPECT_EQ(px(200, 200), 0);
}

TEST_F(GpuPrimitivesTest, TexturedSpritesSampleCurrentTexpageAtEachSize) {
  upload(64, 0, 32, 32, rgb15(0, 248, 248));
  gpu.writeGP0(0xE1000000 | kTpage15);
  gpu.writeGP0(0x6D808080); // 1x1 textured, raw
  gpu.writeGP0(xy(600, 0));
  gpu.writeGP0(0);
  gpu.writeGP0(0x75808080); // 8x8 textured, raw
  gpu.writeGP0(xy(600, 10));
  gpu.writeGP0(0);
  gpu.writeGP0(0x7D808080); // 16x16 textured, raw
  gpu.writeGP0(xy(600, 30));
  gpu.writeGP0(0);

  const uint16_t cyan = rgb15(0, 248, 248);
  EXPECT_EQ(px(600, 0), cyan);
  EXPECT_EQ(px(601, 0), 0);
  EXPECT_EQ(px(607, 17), cyan);
  EXPECT_EQ(px(608, 17), 0);
  EXPECT_EQ(px(615, 45), cyan);
  EXPECT_EQ(px(616, 45), 0);
}

TEST_F(GpuPrimitivesTest, TexturedSpriteSkipsTransparentTexelsAndBlendsFlagged) {
  upload(64, 0, 2, 1, 0); // texel u=0,1 = 0000h (transparent)
  upload(66, 0, 2, 1, 0x8000 | rgb15(64, 0, 0));
  gpu.writeGP0(0xE1000000 | kTpage15 | (1 << 5)); // B+F
  gpu.writeGP0(0x02000040);
  gpu.writeGP0(xy(700, 0));
  gpu.writeGP0(xy(16, 1));
  gpu.writeGP0(0x67808080); // variable textured, semi-transparent, raw
  gpu.writeGP0(xy(700, 0));
  gpu.writeGP0(0);
  gpu.writeGP0(xy(4, 1));
  EXPECT_EQ(px(700, 0), rgb15(64, 0, 0)); // transparent texel: background kept
  EXPECT_EQ(px(702, 0) & 0x7FFF, rgb15(128, 0, 0));
}

// ---------------------------------------------------------------------------
// Lines (psx-spx "GPU Render Line Commands")
// ---------------------------------------------------------------------------

TEST_F(GpuPrimitivesTest, LineIncludesBothEndpointsInEitherDirection) {
  gpu.writeGP0(0x400000FF);
  gpu.writeGP0(xy(20, 50));
  gpu.writeGP0(xy(10, 40)); // right-to-left, bottom-to-top diagonal
  for (int i = 0; i <= 10; ++i)
    EXPECT_EQ(px(10 + i, 40 + i), rgb15(255, 0, 0)) << "i=" << i;
  EXPECT_EQ(px(9, 39), 0);
  EXPECT_EQ(px(21, 51), 0);
}

TEST_F(GpuPrimitivesTest, SteepLineStepsOnePixelPerRow) {
  gpu.writeGP0(0x4000FF00);
  gpu.writeGP0(xy(5, 0));
  gpu.writeGP0(xy(7, 8));
  for (int y = 0; y <= 8; ++y) {
    int hits = 0;
    for (int x = 0; x < 12; ++x)
      hits += px(x, y) == rgb15(0, 255, 0);
    EXPECT_EQ(hits, 1) << "y=" << y;
  }
}

TEST_F(GpuPrimitivesTest, GouraudLineStartsWithFirstVertexColour) {
  gpu.writeGP0(0x500000FF); // gouraud line, c0 red
  gpu.writeGP0(xy(0, 60));
  gpu.writeGP0(0x0000FF00); // c1
  gpu.writeGP0(xy(8, 60));
  EXPECT_EQ(px(0, 60), rgb15(255, 0, 0));
  EXPECT_NE(px(8, 60), 0);
  EXPECT_EQ(px(9, 60), 0);
}

TEST_F(GpuPrimitivesTest, SemiTransparentLineBlendsAndIsClipped) {
  gpu.writeGP0(0xE1000000 | (3 << 5)); // B + F/4
  gpu.writeGP0(0xE4000000 | (511 << 10) | 15);
  gpu.writeGP0(0x02000080);
  gpu.writeGP0(xy(0, 70));
  gpu.writeGP0(xy(32, 1));
  gpu.writeGP0(0x42000080); // semi-transparent line, r=128
  gpu.writeGP0(xy(0, 70));
  gpu.writeGP0(xy(30, 70));
  EXPECT_EQ(px(5, 70), rgb15(160, 0, 0));  // 128 + 128/4
  EXPECT_EQ(px(20, 70), rgb15(128, 0, 0)); // beyond X2=15: untouched
}

// ---------------------------------------------------------------------------
// VRAM transfers (psx-spx "GPU Memory Transfer Commands")
// ---------------------------------------------------------------------------

TEST_F(GpuPrimitivesTest, CpuToVramWrapsHorizontallyAt1024) {
  gpu.writeGP0(0xA0000000);
  gpu.writeGP0(xy(1022, 5));
  gpu.writeGP0(xy(4, 1));
  gpu.writeGP0(0x22221111);
  gpu.writeGP0(0x44443333);
  EXPECT_EQ(px(1022, 5), 0x1111);
  EXPECT_EQ(px(1023, 5), 0x2222);
  EXPECT_EQ(px(0, 5), 0x3333);
  EXPECT_EQ(px(1, 5), 0x4444);
}

TEST_F(GpuPrimitivesTest, CpuToVramZeroSizeMeansFullDimension) {
  // psx-spx: Xsiz=((Xsiz-1) AND 3FFh)+1, Ysiz=((Ysiz-1) AND 1FFh)+1.
  gpu.writeGP0(0xA0000000);
  gpu.writeGP0(xy(0, 0));
  gpu.writeGP0(0); // 1024 x 512
  gpu.writeGP0(0xBBBBAAAA);
  for (int i = 0; i < 511; ++i) // rest of row 0
    gpu.writeGP0(0x12341234);
  gpu.writeGP0(0x56785678); // row 1 starts here only if width was 1024
  EXPECT_EQ(px(0, 0), 0xAAAA);
  EXPECT_EQ(px(1023, 0), 0x1234);
  EXPECT_EQ(px(0, 1), 0x5678);
}

TEST_F(GpuPrimitivesTest, VramToCpuReturnsTwoPixelsPerWordThenLatch) {
  gpu.writeGP0(0xA0000000);
  gpu.writeGP0(xy(50, 50));
  gpu.writeGP0(xy(2, 2));
  gpu.writeGP0(0x22221111);
  gpu.writeGP0(0x44443333);

  gpu.writeGP1(0x10000007); // latch GPU version into GPUREAD
  const uint32_t latched = gpu.readGPUREAD();
  gpu.writeGP0(0xC0000000);
  gpu.writeGP0(xy(50, 50));
  gpu.writeGP0(xy(2, 2));
  EXPECT_EQ(gpu.readGPUREAD(), 0x22221111u);
  EXPECT_EQ(gpu.readGPUREAD(), 0x44443333u);
  EXPECT_EQ(gpu.readGPUREAD(), latched); // transfer done: register latch again
}

TEST_F(GpuPrimitivesTest, VramToVramZeroHeightCopiesFull512Rows) {
  upload(0, 511, 1, 1, 0x2222); // odd-sized upload: written before (0,0)
  upload(0, 0, 1, 1, 0x1111);
  gpu.writeGP0(0x80000000);
  gpu.writeGP0(xy(0, 0));
  gpu.writeGP0(xy(5, 0));
  gpu.writeGP0(xy(1, 0)); // width 1, height 0 -> 512
  EXPECT_EQ(px(5, 0), 0x1111);
  EXPECT_EQ(px(5, 511), 0x2222);
  EXPECT_EQ(px(6, 0), 0);
}

TEST_F(GpuPrimitivesTest, VramToVramZeroWidthCopiesFull1024Columns) {
  upload(1023, 7, 1, 1, 0x3333);
  gpu.writeGP0(0x80000000);
  gpu.writeGP0(xy(0, 7));
  gpu.writeGP0(xy(0, 9));
  gpu.writeGP0(xy(0, 1)); // width 0 -> 1024, height 1
  EXPECT_EQ(px(1023, 9), 0x3333);
}

// ---------------------------------------------------------------------------
// Command FIFO behaviour
// ---------------------------------------------------------------------------

TEST_F(GpuPrimitivesTest, UnknownGp0OpcodeConsumesOneWord) {
  gpu.writeGP0(0x03000000); // unassigned misc command
  gpu.writeGP0(0x020000FF);
  gpu.writeGP0(xy(3, 3));
  gpu.writeGP0(xy(1, 1));
  EXPECT_EQ(px(3, 3), rgb15(255, 0, 0));
}

TEST_F(GpuPrimitivesTest, NopAndClearCacheDoNotDisturbFollowingCommand) {
  gpu.writeGP0(0x00000000);
  gpu.writeGP0(0x01000000);
  gpu.writeGP0(0xE6000000); // mask setting off/off: plain drawing
  gpu.writeGP0(0x680000FF);
  gpu.writeGP0(xy(9, 9));
  EXPECT_EQ(px(9, 9), rgb15(255, 0, 0));
}

// ---------------------------------------------------------------------------
// GP1 display control and GP1(10h) register reads
// ---------------------------------------------------------------------------

TEST_F(GpuPrimitivesTest, Gp1ResetCommandBufferAbortsHalfSentPolygon) {
  gpu.writeGP0(0x280000FF); // quad header + one vertex, then abort
  gpu.writeGP0(xy(0, 0));
  gpu.writeGP1(0x01000000);
  gpu.writeGP0(0x0200FF00);
  gpu.writeGP0(xy(30, 30));
  gpu.writeGP0(xy(1, 1));
  EXPECT_EQ(px(30, 30), rgb15(0, 255, 0));
}

TEST_F(GpuPrimitivesTest, Gp1ResetCommandBufferAbortsCpuToVramTransfer) {
  gpu.writeGP0(0xA0000000);
  gpu.writeGP0(xy(0, 0));
  gpu.writeGP0(xy(16, 16));
  gpu.writeGP1(0x01000000);
  gpu.writeGP0(0x0200FF00); // must be decoded as a command, not pixel data
  gpu.writeGP0(xy(40, 40));
  gpu.writeGP0(xy(1, 1));
  EXPECT_EQ(px(0, 0), 0);
  EXPECT_EQ(px(40, 40), rgb15(0, 255, 0));
}

TEST_F(GpuPrimitivesTest, Gp1ResetClearsRenderingAttributesAndDmaDirection) {
  gpu.writeGP0(0xE3000000 | (5 << 10) | 5);
  gpu.writeGP0(0xE5000000 | (3 << 11) | 3);
  gpu.writeGP1(0x04000002);
  gpu.writeGP1(0x00000000);
  EXPECT_EQ(readRegister(3), 0u);
  EXPECT_EQ(readRegister(5), 0u);
  EXPECT_EQ((gpu.readGPUSTAT() >> 29) & 3, 0u);
}

TEST_F(GpuPrimitivesTest, Gp1GetInfoReturnsDrawingAreaCorners) {
  gpu.writeGP0(0xE3000000 | (17 << 10) | 33);
  gpu.writeGP0(0xE4000000 | (239 << 10) | 319);
  EXPECT_EQ(readRegister(3), (17u << 10) | 33u);
  EXPECT_EQ(readRegister(4), (239u << 10) | 319u);
  EXPECT_EQ(readRegister(0x13), (17u << 10) | 33u); // 10h..1Fh mirror 0..Fh
}

TEST_F(GpuPrimitivesTest, Gp1GetInfoVersionIndexReportsV2Gpu) {
  EXPECT_EQ(readRegister(7), 2u);
}

TEST_F(GpuPrimitivesTest, UnknownGp1CommandLeavesStatusUnchanged) {
  const uint32_t before = gpu.readGPUSTAT();
  gpu.writeGP1(0x0B000000);
  EXPECT_EQ(gpu.readGPUSTAT(), before);
}

TEST_F(GpuPrimitivesTest, DisplayModeMarksModeSetAndAreaMarksAreaSet) {
  EXPECT_FALSE(gpu.isDisplayModeSet());
  EXPECT_FALSE(gpu.isDisplayAreaSet());
  gpu.writeGP1(0x08000001);
  gpu.writeGP1(0x05000000 | (240 << 10) | 320);
  EXPECT_TRUE(gpu.isDisplayModeSet());
  EXPECT_TRUE(gpu.isDisplayAreaSet());
  uint32_t x = 0, y = 0;
  gpu.getDisplayArea(x, y);
  EXPECT_EQ(x, 320u);
  EXPECT_EQ(y, 240u);
}

TEST_F(GpuPrimitivesTest, DisplayEnableClearsAndSetsBit23) {
  gpu.writeGP1(0x03000000);
  EXPECT_TRUE(gpu.isDisplayEnabled());
  gpu.writeGP1(0x03000001);
  EXPECT_FALSE(gpu.isDisplayEnabled());
}

// ---------------------------------------------------------------------------
// Host-side buffers: save-state load and VBlank snapshot
// ---------------------------------------------------------------------------

TEST_F(GpuPrimitivesTest, LoadVramReplacesEveryPixel) {
  std::vector<uint16_t> image(GPU::VRAM_WIDTH * GPU::VRAM_HEIGHT);
  for (size_t i = 0; i < image.size(); ++i)
    image[i] = static_cast<uint16_t>(i * 7);
  gpu.loadVram(reinterpret_cast<const uint8_t *>(image.data()));
  EXPECT_EQ(px(0, 0), image[0]);
  EXPECT_EQ(px(1023, 511), image.back());
  EXPECT_EQ(px(300, 200), image[200 * 1024 + 300]);
}

TEST_F(GpuPrimitivesTest, SnapshotCopiesLiveVramIntoDisplayBuffer) {
  gpu.writeGP0(0x680000FF);
  gpu.writeGP0(xy(12, 34));
  EXPECT_EQ(gpu.getDisplayVRAM()[34 * 1024 + 12].raw, 0); // not yet snapshotted
  gpu.snapshotDisplayBuffer();
  EXPECT_EQ(gpu.getDisplayVRAM()[34 * 1024 + 12].raw, rgb15(255, 0, 0));
  gpu.writeGP0(0x6800FF00); // later draws do not leak into the snapshot
  gpu.writeGP0(xy(12, 34));
  EXPECT_EQ(gpu.getDisplayVRAM()[34 * 1024 + 12].raw, rgb15(255, 0, 0));
}

// ---------------------------------------------------------------------------
// Primitive census (PS1_CENSUS diagnostic dump)
// ---------------------------------------------------------------------------

TEST_F(GpuPrimitivesTest, CensusDumpCountsDrawnDegenerateAndClipped) {
  gpu.censusReset();
  gpu.writeGP0(0xE4000000 | (100 << 10) | 100);
  gpu.writeGP0(0x200000FF); // drawn triangle
  gpu.writeGP0(xy(0, 0));
  gpu.writeGP0(xy(20, 0));
  gpu.writeGP0(xy(0, 20));
  gpu.writeGP0(0x200000FF); // degenerate
  gpu.writeGP0(xy(0, 0));
  gpu.writeGP0(xy(1, 1));
  gpu.writeGP0(xy(2, 2));
  gpu.writeGP0(0x200000FF); // clipped triangle
  gpu.writeGP0(xy(300, 300));
  gpu.writeGP0(xy(320, 300));
  gpu.writeGP0(xy(300, 320));
  gpu.writeGP0(0x700000FF); // clipped rect
  gpu.writeGP0(xy(400, 400));
  gpu.writeGP0(0x700000FF); // drawn rect
  gpu.writeGP0(xy(50, 50));
  gpu.writeGP0(0x60000000); // zero-size rect: degenerate
  gpu.writeGP0(xy(10, 10));
  gpu.writeGP0(0);

  const auto path = std::filesystem::temp_directory_path() /
                    ("ps1_gpu_census_" + std::to_string(::getpid()) + ".txt");
  gpu.censusDump(path.c_str());
  std::ifstream in(path);
  std::string header;
  std::getline(in, header);
  std::string second;
  std::getline(in, second);
  in.close();
  std::filesystem::remove(path);

  EXPECT_EQ(header, "drawn 2 degenerate 2 clipped 2 tri 1");
  EXPECT_EQ(second, "gt4_start 0 gt4_exec 0 clipped_tri 1");
}

// GP1(00h) reset clears the GP0(E2h) texture window (psx-spx "GPU Display
// Control Commands"); a stale window made texture tests depend on run order.
TEST_F(GpuPrimitivesTest, ResetClearsTextureWindow) {
  gpu.writeGP0(0xE2000000 | 1 | (1 << 5)); // 8-px window
  gpu.writeGP1(0x00000000);
  gpu.writeGP0(0xE1000000 | (1 | (2 << 7))); // 15-bit page at X=64
  gpu.writeGP0(0xA0000000);
  gpu.writeGP0(10 | (0 << 16));
  gpu.writeGP0(16 | (1 << 16));
  for (int i = 0; i < 8; ++i)
    gpu.writeGP0(0x03E003E0); // green everywhere u=0..15
  // u=10 must sample texel 10; under the stale window it samples texel 2.
  gpu.writeGP0(0xA0000000);
  gpu.writeGP0(64 | (0 << 16));
  gpu.writeGP0(16 | (1 << 16));
  for (int i = 0; i < 8; ++i)
    gpu.writeGP0(i == 5 ? 0x001F001F : 0x03E003E0); // u=10,11 red
  gpu.writeGP0(0x65000000);
  gpu.writeGP0(500);
  gpu.writeGP0(0);
  gpu.writeGP0(16 | (1 << 16));
  EXPECT_EQ(px(510, 0), 0x001F);
}
