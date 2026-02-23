#include "runtime/gpu/gpu.h"
#include <gtest/gtest.h>

using namespace ps1::gpu;

class GpuTexturesTest : public ::testing::Test {
protected:
  GPU gpu;

  void SetUp() override { gpu.reset(); }
};

TEST_F(GpuTexturesTest, TexturedTriangle4BitWithCLUT) {
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
  gpu.writeGP0(0x24000000);
  gpu.writeGP0((10 << 16) | 10);
  gpu.writeGP0((0x7800 << 16) | (0 << 8) | 0);
  gpu.writeGP0((10 << 16) | 20);
  gpu.writeGP0((0x0001 << 16) | (0 << 8) | 10);
  gpu.writeGP0((20 << 16) | 10);
  gpu.writeGP0((10 << 8) | 0);

  const auto *vram = gpu.getVRAM();
  EXPECT_EQ(vram[10 * 1024 + 10].raw, 0xAA55);
}
