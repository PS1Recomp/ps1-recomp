#include "runtime/gpu/gpu.h"
#include <gtest/gtest.h>

using namespace ps1::gpu;

class GpuRasterizerTest : public ::testing::Test {
protected:
  GPU gpu;

  void SetUp() override { gpu.reset(); }
};

TEST_F(GpuRasterizerTest, MonochromeTriangleDrawsPixels) {
  // Command: Opcode 0x20
  // C: RGB(0, 255, 0)
  gpu.writeGP0(0x2000FF00); // 0x20 | B=0, G=FF, R=00

  // V0: X=10, Y=10
  gpu.writeGP0((10 << 16) | 10);

  // V1: X=20, Y=10
  gpu.writeGP0((10 << 16) | 20);

  // V2: X=10, Y=20
  gpu.writeGP0((20 << 16) | 10);

  // This should rasterize a right triangle at the top-left (10,10) to (20,10)
  // and (10,20)

  const Color16 *vram = gpu.getVRAM();

  // Green is g5 = 255>>3 = 31 -> packed as 31 << 5
  uint16_t expectedRaw = 31 << 5;

  // Top left vertex should be drawn
  EXPECT_EQ(vram[11 * GPU::VRAM_WIDTH + 11].raw, expectedRaw);

  // Outside bounding box should be 0
  EXPECT_EQ(vram[9 * GPU::VRAM_WIDTH + 10].raw, 0);
  EXPECT_EQ(vram[10 * GPU::VRAM_WIDTH + 9].raw, 0);
  EXPECT_EQ(vram[21 * GPU::VRAM_WIDTH + 10].raw, 0);
}

TEST_F(GpuRasterizerTest, MonochromeQuadDrawsTwoTriangles) {
  // Command: Opcode 0x28
  // C: RGB(0, 0, 255)
  gpu.writeGP0(0x28FF0000); // 0x28 | B=FF, G=00, R=00

  // V0: 10, 10
  gpu.writeGP0((10 << 16) | 10);
  // V1: 20, 10
  gpu.writeGP0((10 << 16) | 20);
  // V2: 10, 20
  gpu.writeGP0((20 << 16) | 10);
  // V3: 20, 20
  gpu.writeGP0((20 << 16) | 20);

  const Color16 *vram = gpu.getVRAM();

  // Blue is b5 = 255>>3 = 31 -> packed as 31 << 10
  uint16_t expectedRaw = 31 << 10;

  // Check typical internal points
  EXPECT_EQ(vram[15 * GPU::VRAM_WIDTH + 15].raw, expectedRaw);

  // Outside
  EXPECT_EQ(vram[9 * GPU::VRAM_WIDTH + 10].raw, 0);
}
