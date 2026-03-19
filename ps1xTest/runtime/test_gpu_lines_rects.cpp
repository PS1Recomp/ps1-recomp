#include "runtime/gpu/gpu.h"
#include <gtest/gtest.h>

using namespace ps1::gpu;

class GpuLinesRectsTest : public ::testing::Test {
protected:
  GPU gpu;

  void SetUp() override { gpu.reset(); }
};

TEST_F(GpuLinesRectsTest, ExecuteMonochromeLine) {
  // Command 0x40 (Mono Line)
  // X0,Y0 = 10,10. X1,Y1 = 15,10
  // Color: Red (255, 0, 0)
  gpu.writeGP0(0x400000FF);
  gpu.writeGP0((10 << 16) | 10);
  gpu.writeGP0((10 << 16) | 15);

  const auto *vram = gpu.getVRAM();
  uint16_t red = 31 | (0 << 5) | (0 << 10);

  EXPECT_EQ(vram[10 * 1024 + 10].raw, red);
  EXPECT_EQ(vram[10 * 1024 + 12].raw, red);
  EXPECT_EQ(vram[10 * 1024 + 15].raw, red);
  EXPECT_EQ(vram[10 * 1024 + 16].raw, 0); // Past line
}

TEST_F(GpuLinesRectsTest, Execute16x16Rect) {
  // Command 0x78 (16x16 Rect)
  // X0,Y0 = 20, 30
  // Color: Green (0, 255, 0)
  gpu.writeGP0(0x7800FF00);
  gpu.writeGP0((30 << 16) | 20);

  const auto *vram = gpu.getVRAM();
  uint16_t green = 0 | (31 << 5) | (0 << 10);

  EXPECT_EQ(vram[30 * 1024 + 20].raw, green);
  EXPECT_EQ(vram[45 * 1024 + 35].raw, green);
  EXPECT_EQ(vram[46 * 1024 + 20].raw, 0); // Past rect height
  EXPECT_EQ(vram[30 * 1024 + 36].raw, 0); // Past rect width
}
