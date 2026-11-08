// Tests: GP0(E2h) texture window (psx-spx "GP0(E2h) - Texture Window
// setting"): texcoord = (texcoord AND NOT(Mask*8)) OR ((Offset AND Mask)*8).
#include "runtime/gpu/gpu.h"
#include <gtest/gtest.h>

using namespace ps1::gpu;

class GpuTextureWindowTest : public ::testing::Test {
protected:
  GPU gpu;

  void SetUp() override {
    gpu.reset();
    gpu.writeGP0(0xE1000000); // no dither
  }

  // 15-bit texels: block u<8 red / u>=8 green for v<8; blue for v>=8.
  void uploadQuadrants() {
    fill(64, 0, 8, 8, 0x001F);
    fill(72, 0, 8, 8, 0x03E0);
    fill(64, 8, 16, 8, 0x7C00);
  }
  void fill(int x, int y, int w, int h, uint16_t c) {
    gpu.writeGP0(0xA0000000);
    gpu.writeGP0((y << 16) | x);
    gpu.writeGP0((h << 16) | w);
    for (int i = 0; i < (w * h + 1) / 2; ++i)
      gpu.writeGP0((static_cast<uint32_t>(c) << 16) | c);
  }
  // Raw flat-textured triangle (2,2)-(18,2)-(2,18) over uv (0,0)-(16,16) of
  // the 15-bit page at X=64.
  void drawTriangle() {
    const uint32_t tpage = 1 | (2 << 7);
    gpu.writeGP0(0x25000000);
    gpu.writeGP0((2 << 16) | 2);
    gpu.writeGP0(0);
    gpu.writeGP0((2 << 16) | 18);
    gpu.writeGP0((tpage << 16) | 16);
    gpu.writeGP0((18 << 16) | 2);
    gpu.writeGP0(16 << 8);
  }
  uint16_t px(int x, int y) const {
    return gpu.getVRAM()[y * GPU::VRAM_WIDTH + x].raw;
  }
};

TEST_F(GpuTextureWindowTest, NoWindowSamplesTexcoordsUnchanged) {
  uploadQuadrants();

  drawTriangle();

  EXPECT_EQ(px(3, 3), 0x001F); // u~1, v~1: red block
}

TEST_F(GpuTextureWindowTest, HorizontalOffsetForcesUIntoTheWindowBlock) {
  uploadQuadrants();
  gpu.writeGP0(0xE2000000 | 1 | (1 << 10)); // mask X 1, offset X 1

  drawTriangle();

  EXPECT_EQ(px(3, 3), 0x03E0); // u~1 -> u|8: green block
}

TEST_F(GpuTextureWindowTest, VerticalOffsetForcesVIntoTheWindowBlock) {
  uploadQuadrants();
  gpu.writeGP0(0xE2000000 | (1 << 5) | (1 << 15)); // mask Y 1, offset Y 1

  drawTriangle();

  EXPECT_EQ(px(3, 3), 0x7C00); // v~1 -> v|8: blue block
}
