#include "runtime/gpu/gpu.h"
#include <gtest/gtest.h>

using namespace ps1::gpu;

class GpuVramTest : public ::testing::Test {
protected:
  GPU gpu;

  void SetUp() override { gpu.reset(); }
};

TEST_F(GpuVramTest, CPUToVRAMTransfer) {
  // Command 0xA0: Copy Rectangle (CPU to VRAM)
  // X=10, Y=20, Width=4, Height=2 -> 8 pixels -> 4 words
  gpu.writeGP0(0xA0000000);      // Command
  gpu.writeGP0((20 << 16) | 10); // Y=20, X=10
  gpu.writeGP0((2 << 16) | 4);   // H=2, W=4

  // Now send the data words
  gpu.writeGP0(0x11112222); // Pixel 1: 2222, Pixel 2: 1111
  gpu.writeGP0(0x33334444); // Pixel 3: 4444, Pixel 4: 3333
  gpu.writeGP0(0x55556666); // Pixel 5: 6666, Pixel 6: 5555
  gpu.writeGP0(0x77778888); // Pixel 7: 8888, Pixel 8: 7777

  const auto *vram = gpu.getVRAM();
  EXPECT_EQ(vram[20 * 1024 + 10].raw, 0x2222);
  EXPECT_EQ(vram[20 * 1024 + 11].raw, 0x1111);
  EXPECT_EQ(vram[20 * 1024 + 12].raw, 0x4444);
  EXPECT_EQ(vram[20 * 1024 + 13].raw, 0x3333);

  EXPECT_EQ(vram[21 * 1024 + 10].raw, 0x6666);
  EXPECT_EQ(vram[21 * 1024 + 11].raw, 0x5555);
  EXPECT_EQ(vram[21 * 1024 + 12].raw, 0x8888);
  EXPECT_EQ(vram[21 * 1024 + 13].raw, 0x7777);
}

TEST_F(GpuVramTest, VRAMToVRAMTransfer) {
  // First, manually fill some pixels in VRAM
  // We'll write to rect at X=0, Y=0 (4x2)
  gpu.writeGP0(0xA0000000);    // CPU to VRAM
  gpu.writeGP0(0);             // 0,0
  gpu.writeGP0((2 << 16) | 4); // H=2, W=4
  gpu.writeGP0(0xAAAAA000);
  gpu.writeGP0(0xCCCCC000);
  gpu.writeGP0(0xEEEEEE00);
  gpu.writeGP0(0xFFFFFF00);

  // Now, VRAM to VRAM copy!
  // Move 4x2 rect from 0,0 to 100,50
  gpu.writeGP0(0x80000000);       // Copy Rectangle
  gpu.writeGP0((0 << 16) | 0);    // Src Y=0, X=0
  gpu.writeGP0((50 << 16) | 100); // Dest Y=50, X=100
  gpu.writeGP0((2 << 16) | 4);    // H=2, W=4

  const auto *vram = gpu.getVRAM();
  EXPECT_EQ(vram[50 * 1024 + 100].raw, 0xA000);
  EXPECT_EQ(vram[50 * 1024 + 101].raw, 0xAAAA);
  EXPECT_EQ(vram[50 * 1024 + 102].raw, 0xC000);
  EXPECT_EQ(vram[50 * 1024 + 103].raw, 0xCCCC);

  EXPECT_EQ(vram[51 * 1024 + 100].raw, 0xEE00);
  EXPECT_EQ(vram[51 * 1024 + 101].raw, 0xEEEE);
  EXPECT_EQ(vram[51 * 1024 + 102].raw, 0xFF00);
  EXPECT_EQ(vram[51 * 1024 + 103].raw, 0xFFFF);
}

TEST_F(GpuVramTest, VRAMToCPUTransfer) {
  // Setup VRAM: write to 0,0 a 2x1 block = 2 pixels = 1 word
  gpu.writeGP0(0xA0000000);    // CPU to VRAM
  gpu.writeGP0(0);             // 0,0
  gpu.writeGP0((1 << 16) | 2); // H=1, W=2
  gpu.writeGP0(0xBEEFBABE);    // pixel1=BABE, pixel2=BEEF

  // Now request it back
  gpu.writeGP0(0xC0000000);    // VRAM to CPU
  gpu.writeGP0(0);             // 0,0
  gpu.writeGP0((1 << 16) | 2); // H=1, W=2

  uint32_t val = gpu.readGPUREAD();
  EXPECT_EQ(val, 0xBEEFBABE);
}
