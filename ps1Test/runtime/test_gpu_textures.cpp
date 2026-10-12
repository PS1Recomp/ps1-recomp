#include "runtime/gpu/gpu.h"
#include <gtest/gtest.h>

using namespace ps1::gpu;

class GpuTexturesTest : public ::testing::Test {
protected:
  GPU gpu;

  void SetUp() override { gpu.reset(); }
};

TEST_F(GpuTexturesTest, TexturedTriangle4BitWithCLUT) {
  // Disable Dithering explicitly for reliable color hex match
  gpu.writeGP0(0xE1000000);

  // Setup CLUT at 0, 480 (CLUT x=0, y=480) -> CLUT code = (480 << 6) | (0 >> 4)
  // = 0x7800 CLUT is 16 colors. We'll set color 0 = transparent, color 1 =
  // 0xAA55
  gpu.writeGP0(0xA0000000);      // CPU to VRAM
  gpu.writeGP0((480 << 16) | 0); // Y=480, X=0
  gpu.writeGP0((1 << 16) |
               8); // H=1, W=8 (16 colors = 16 16-bit words = 8 32-bit words)
  gpu.writeGP0(0xAA550000); // P1=0xAA55 (idx 1), P0=0x0000 (idx 0)
  gpu.writeGP0(0);          // the rest 0
  gpu.writeGP0(0);
  gpu.writeGP0(0);
  gpu.writeGP0(0);
  gpu.writeGP0(0);
  gpu.writeGP0(0);
  gpu.writeGP0(0);

  // Setup Texture Page at X=64 (TPage=1, base 64, 4-bit) (TPage code 0x01)
  gpu.writeGP0(0xA0000000);     // CPU to VRAM
  gpu.writeGP0((0 << 16) | 64); // Y=0, X=64
  gpu.writeGP0((1 << 16) | 1);  // H=1, W=1 (2 words, 4 pixels)
  gpu.writeGP0(0x00000100); // Pixel 0 = idx 0, Pixel 1 = idx 1, Pixel 2 = idx
                            // 0, Pixel 3 = idx 0
  // Note: 0x0100 is 0000 0001 0000 0000 -> Wait, little endian 4-bit indices.
  // 0x0100 has 1 at the 8th bit, meaning it's the 3rd nibble (Pixel 2).
  // Wait, let's just make it 0x00000010 -> Pixel 1 = idx 1

  // Actually, write a block of entirely index 1: 0x11111111 0x11111111
  gpu.writeGP0(0xA0000000);
  gpu.writeGP0((0 << 16) | 64);
  gpu.writeGP0((10 << 16) |
               5); // 10 high by 20 pixels wide (4-bit -> 5 32-bit words wide)
  for (int i = 0; i < 50; i++)
    gpu.writeGP0(0x11111111);

  // Draw Textured Triangle (0x24)
  // V0: 10,10. UV: 0,0. CLUT: 0x7800
  // V1: 20,10. UV: 10,0. TPage: 0x0001 (4-bit, base 64,0)
  // V2: 10,20. UV: 0,10.
  gpu.writeGP0(0x24808080); // 0x80 per channel: modulation is identity
  gpu.writeGP0((10 << 16) | 10);
  gpu.writeGP0((0x7800 << 16) | (0 << 8) | 0);
  gpu.writeGP0((10 << 16) | 20);
  gpu.writeGP0((0x0001 << 16) | (0 << 8) | 10);
  gpu.writeGP0((20 << 16) | 10);
  gpu.writeGP0((10 << 8) | 0);

  const auto *vram = gpu.getVRAM();
  EXPECT_EQ(vram[10 * 1024 + 10].raw, 0xAA55);
}

// Flat textured polygons (GP0 24h-2Fh): modulation by the command colour, and
// semi-transparency (psx-spx "Semi-transparency"): only texels with bit 15 set
// blend, and the mode comes from the polygon's own Texpage attribute.  The
// rasterizer ignored both, so a semi-transparent fog layer drawn this way came
// out opaque and covered the scene.
namespace {
// 15-bit texture page at X=64 (Texpage: X base 1, depth 2), optional mode.
constexpr uint16_t tpage15(uint16_t semiMode) {
  return static_cast<uint16_t>(0x0101 | (semiMode << 5));
}

void putTexel(GPU &gpu, uint16_t texel) {
  gpu.writeGP0(0xA0000000);
  gpu.writeGP0((0 << 16) | 64);
  gpu.writeGP0((16 << 16) | 16);
  for (int i = 0; i < 16 * 16 / 2; ++i)
    gpu.writeGP0((static_cast<uint32_t>(texel) << 16) | texel);
}

void putBackground(GPU &gpu, uint16_t color) {
  gpu.writeGP0(0xA0000000);
  gpu.writeGP0((0 << 16) | 0);
  gpu.writeGP0((32 << 16) | 32);
  for (int i = 0; i < 32 * 32 / 2; ++i)
    gpu.writeGP0((static_cast<uint32_t>(color) << 16) | color);
}

// Triangle covering (2,2)-(12,2)-(2,12), sampling the uniform texel block.
void drawTri(GPU &gpu, uint32_t opcodeAndColor, uint16_t tpage) {
  gpu.writeGP0(opcodeAndColor);
  gpu.writeGP0((2 << 16) | 2);
  gpu.writeGP0(0);
  gpu.writeGP0((2 << 16) | 12);
  gpu.writeGP0((static_cast<uint32_t>(tpage) << 16) | 10);
  gpu.writeGP0((12 << 16) | 2);
  gpu.writeGP0(10 << 8);
}

uint16_t pixel(const GPU &gpu, int x, int y) { return gpu.getVRAM()[y * 1024 + x].raw; }
} // namespace

TEST_F(GpuTexturesTest, FlatTexturedTriangleModulatesByCommandColour) {
  gpu.writeGP0(0xE1000000); // no dither
  putTexel(gpu, 0x001F);    // red 31
  drawTri(gpu, 0x24000040, tpage15(0)); // red 0x40 = half brightness
  EXPECT_EQ(pixel(gpu, 4, 4), 0x000F);
}

TEST_F(GpuTexturesTest, SemiTransparentTexelBlendsWithTexpageMode) {
  gpu.writeGP0(0xE1000000); // E1 says mode 0 (average) ...
  putBackground(gpu, 0x0010);
  putTexel(gpu, 0x8010);    // red 16, semi-transparency flag set
  drawTri(gpu, 0x27000000, tpage15(1)); // ... the polygon says B+F, raw
  EXPECT_EQ(pixel(gpu, 4, 4), 0x001F) << "16 + 16 clamped to 31";
}

TEST_F(GpuTexturesTest, TexelWithoutFlagIsOpaqueInSemiTransparentCommand) {
  gpu.writeGP0(0xE1000000);
  putBackground(gpu, 0x7C00); // blue
  putTexel(gpu, 0x0010);      // red 16, no semi-transparency flag
  drawTri(gpu, 0x27000000, tpage15(1));
  EXPECT_EQ(pixel(gpu, 4, 4), 0x0010) << "written as-is";
}

// F2 (dedither) overrides the game's dither bit: a flat triangle drawn with
// dithering on comes out as one exact colour.
TEST_F(GpuTexturesTest, DeditherIgnoresTheGameDitherBit) {
  auto draw = [&] {
    gpu.reset();
    gpu.writeGP0(0xE1000200);                  // dither on
    gpu.writeGP0(0xE3000000);                  // draw area 0,0 ..
    gpu.writeGP0(0xE4000000 | (63 << 10) | 63); // .. 63,63
    gpu.writeGP0(0x20404040);                  // flat triangle, grey
    gpu.writeGP0(0x00000000);
    gpu.writeGP0(0x00000020);
    gpu.writeGP0(0x00200000);
  };
  draw();
  EXPECT_NE(pixel(gpu, 1, 1), pixel(gpu, 2, 1)) << "game dithering visible";
  gpu.setDedither(true);
  draw();
  EXPECT_EQ(pixel(gpu, 1, 1), pixel(gpu, 2, 1));
  EXPECT_EQ(pixel(gpu, 1, 1), pixel(gpu, 3, 2));
}
