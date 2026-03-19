#include "runtime/gpu/gpu.h"
#include <gtest/gtest.h>

using namespace ps1::gpu;

class GpuDmaTest : public ::testing::Test {
protected:
  GPU gpu;
  uint8_t ram[1024 * 1024 * 2]; // 2MB dummy RAM

  void SetUp() override {
    gpu.reset();
    std::fill(std::begin(ram), std::end(ram), 0);
  }

  void writeWord(uint32_t addr, uint32_t val) {
    uint32_t offset = addr & 0x1FFFFC;
    ram[offset] = val & 0xFF;
    ram[offset + 1] = (val >> 8) & 0xFF;
    ram[offset + 2] = (val >> 16) & 0xFF;
    ram[offset + 3] = (val >> 24) & 0xFF;
  }
};

TEST_F(GpuDmaTest, ProcessLinkedListParsesOT) {
  // Construct a small OT with 2 nodes.

  // Node 1 at 0x1000 sends a Monochrome Rectangle
  // Header: 2 words payload, points to 0x2000
  writeWord(0x1000, (2 << 24) | 0x2000);
  // Word 1: 0x68 (1x1 Rect), Color Red
  writeWord(0x1004, 0x680000FF);
  // Word 2: X=10, Y=10
  writeWord(0x1008, (10 << 16) | 10);

  // Node 2 at 0x2000 sends a Fill Rect
  // Header: 3 words payload, points to 0xFFFFFF (End of Table)
  writeWord(0x2000, (3 << 24) | 0xFFFFFF);
  // Word 1: 0x02 (Fill Rect), Color Green
  writeWord(0x2004, 0x0200FF00);
  // Word 2: X=20, Y=20
  writeWord(0x2008, (20 << 16) | 20);
  // Word 3: W=5, H=5
  writeWord(0x200C, (5 << 16) | 5);

  gpu.processLinkedList(0x1000, ram);

  const auto *vram = gpu.getVRAM();

  // Verify 1x1 Rect from Node 1
  uint16_t red = 31 | (0 << 5) | (0 << 10);
  EXPECT_EQ(vram[10 * 1024 + 10].raw, red);

  // Verify Fill Rect from Node 2
  uint16_t green = 0 | (31 << 5) | (0 << 10);
  EXPECT_EQ(vram[20 * 1024 + 20].raw, green);
  EXPECT_EQ(vram[24 * 1024 + 24].raw, green);
}
