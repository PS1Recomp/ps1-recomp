#include "runtime/gpu/gpu.h"
#include <cstdint>
#include <gtest/gtest.h>

using namespace ps1::gpu;

class GpuCommandTest : public ::testing::Test {
protected:
  GPU gpu;

  void SetUp() override { gpu.reset(); }
};

TEST_F(GpuCommandTest, ResetClearsVRAMAndState) {
  // Dirty VRAM and status manually (simulate some commands)
  gpu.writeGP0(0x02000000 | 0xFFFFFF); // Fill rect with white
  gpu.writeGP0((0 << 16) | 0);         // at 0,0
  gpu.writeGP0((10 << 16) | 10);       // size 10x10

  // Write some GP1 commands to dirty GPUSTAT
  gpu.writeGP1(0x04000002); // DMA Direction

  gpu.reset();

  EXPECT_EQ(gpu.readGPUSTAT(), 0x14802000);
  EXPECT_EQ(gpu.readGPUREAD(), 0);

  // Check VRAM is zeroed
  const Color16 *vram = gpu.getVRAM();
  bool allZero = true;
  for (uint32_t i = 0; i < GPU::VRAM_WIDTH * GPU::VRAM_HEIGHT; ++i) {
    if (vram[i].raw != 0) {
      allZero = false;
      break;
    }
  }
  EXPECT_TRUE(allZero);
}

TEST_F(GpuCommandTest, GP0E3E4E5Attributes) {
  // E3: Draw Area top left
  gpu.writeGP0(0xE3000000 | (100 << 10) | 50); // Y:100, X:50
  // No direct way to read internal state, but ideally these affect
  // rasterization later. We just ensure the command parser doesn't crash or get
  // stuck

  // E4: Draw Area bottom right
  gpu.writeGP0(0xE4000000 | (200 << 10) | 150); // Y:200, X:150

  // E5: Draw Offset
  gpu.writeGP0(0xE5000000 | (10 << 11) | 20); // Y:10, X:20
}

TEST_F(GpuCommandTest, GP0FillRectFillsArea) {
  // Command: Opcode 0x02, Color RGB(255, 0, 0) -> 0x020000FF
  gpu.writeGP0(0x020000FF);
  // Pos: X=10, Y=20 -> 0x0014000A
  gpu.writeGP0(0x0014000A);
  // Size: W=5, H=5 -> 0x00050005
  gpu.writeGP0(0x00050005);

  const Color16 *vram = gpu.getVRAM();

  // Convert RGB(255,0,0) to 15-bit
  uint16_t expectedRaw = (255 >> 3) | 0 | 0 | 0; // r=31, g=0, b=0, a=0

  EXPECT_EQ(vram[20 * GPU::VRAM_WIDTH + 10].raw, expectedRaw);
  EXPECT_EQ(vram[24 * GPU::VRAM_WIDTH + 14].raw, expectedRaw);
  EXPECT_EQ(vram[19 * GPU::VRAM_WIDTH + 10].raw, 0); // Outside top
  EXPECT_EQ(vram[20 * GPU::VRAM_WIDTH + 9].raw, 0);  // Outside left
}

TEST_F(GpuCommandTest, GP1DMADirectionUpdatesGPUSTAT) {
  uint32_t initialStat = gpu.readGPUSTAT();

  // Send GP1 0x04: DMA Direction (bits 0-1 set bits 29-30 in GPUSTAT)
  // Set to 2 (CPU to VRAM)
  gpu.writeGP1(0x04000002);

  uint32_t newStat = gpu.readGPUSTAT();
  EXPECT_EQ((newStat >> 29) & 3, 2);

  // Ensure rest of struct is unchanged
  EXPECT_EQ(newStat & ~(3 << 29), initialStat & ~(3 << 29));
}
