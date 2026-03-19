#include "runtime/gpu/gpu.h"
#include <gtest/gtest.h>

using namespace ps1::gpu;

class GpuDisplayTest : public ::testing::Test {
protected:
  GPU gpu;

  void SetUp() override {
    gpu.writeGP1(0x00000000); // Reset GPU
  }
};

TEST_F(GpuDisplayTest, DisplayEnableTogglesBit23) {
  // Initial reset state for bit 23 is 1 (Display Off)
  uint32_t stat = gpu.readGPUSTAT();
  EXPECT_NE(stat & (1 << 23), 0);

  // Turn Display On (val=0) means bit 23 goes 0.
  gpu.writeGP1(0x03000000);
  stat = gpu.readGPUSTAT();
  EXPECT_EQ(stat & (1 << 23), 0);

  // Turn Display Off (val=1) means bit 23 goes 1.
  gpu.writeGP1(0x03000001);
  stat = gpu.readGPUSTAT();
  EXPECT_NE(stat & (1 << 23), 0);
}

TEST_F(GpuDisplayTest, DisplayVRAMStartOffset) {
  gpu.writeGP1(0x05000000 | (123) | (456 << 10));

  uint32_t xStart, yStart;
  gpu.getDisplayArea(xStart, yStart);

  EXPECT_EQ(xStart, 123);
  EXPECT_EQ(yStart, 456);
}

TEST_F(GpuDisplayTest, DisplayRangeHorizontal) {
  gpu.writeGP1(0x06000000 | 500 | (3000 << 12));

  uint32_t x1, x2, y1, y2;
  gpu.getDisplayRange(x1, x2, y1, y2);

  EXPECT_EQ(x1, 500);
  EXPECT_EQ(x2, 3000);
}

TEST_F(GpuDisplayTest, DisplayRangeVertical) {
  gpu.writeGP1(0x07000000 | 20 | (260 << 10));

  uint32_t x1, x2, y1, y2;
  gpu.getDisplayRange(x1, x2, y1, y2);

  EXPECT_EQ(y1, 20);
  EXPECT_EQ(y2, 260);
}

TEST_F(GpuDisplayTest, DisplayModeSetsGpuStatCorrectly) {
  // 0x08 - Display Mode
  // Bit 0: Hres1=1
  // Bit 1: Hres2=0 (together 1 means 320)
  // Bit 2: Vres=1 (480)
  // Bit 3: Video Mode=1 (PAL)
  // Bit 4: Depth=1 (24-bit)
  // Bit 5: Interlace=1
  uint32_t mode = (1) | (0 << 1) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5);
  gpu.writeGP1(0x08000000 | mode);

  uint32_t stat = gpu.readGPUSTAT();

  // GP1(08) bit 0-5 map directly to GPUSTAT bits 17-22
  EXPECT_NE(stat & (1 << 17), 0);
  EXPECT_EQ(stat & (1 << 18), 0);
  EXPECT_NE(stat & (1 << 19), 0); // Vres
  EXPECT_NE(stat & (1 << 20), 0); // Video Mode
  EXPECT_NE(stat & (1 << 21), 0); // Depth
  EXPECT_NE(stat & (1 << 22), 0); // Interlace
}
