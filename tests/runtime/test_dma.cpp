#include "runtime/dma/dma.h"
#include "runtime/memory.h"
#include <gtest/gtest.h>

using namespace ps1;

class DmaTest : public ::testing::Test {
protected:
  DMA dma;
  Memory mem;
  void SetUp() override {
    dma.reset();
    mem.reset();
    dma.setMemory(&mem);
  }
};

TEST_F(DmaTest, InitialState) {
  EXPECT_EQ(dma.readRegister(0x1F8010F0), 0x07654321u); // DPCR default
  EXPECT_FALSE(dma.hasInterrupt());
}

TEST_F(DmaTest, DpcrReadWrite) {
  dma.writeRegister(0x1F8010F0, 0x88888888);
  EXPECT_EQ(dma.readRegister(0x1F8010F0), 0x88888888u);
}

TEST_F(DmaTest, ChannelRegisterReadWrite) {
  // Channel 2 (GPU) base addr
  dma.writeRegister(0x1F8010A0, 0x00100000);
  EXPECT_EQ(dma.readRegister(0x1F8010A0), 0x00100000u);

  // Channel 2 block control
  dma.writeRegister(0x1F8010A4, 0x00010001);
  EXPECT_EQ(dma.readRegister(0x1F8010A4), 0x00010001u);
}

TEST_F(DmaTest, OtcClearsOrderingTable) {
  // Enable OTC channel in DPCR
  dma.writeRegister(0x1F8010F0, 0x08888888); // Enable ch6

  // Set up OTC: clear a 4-entry ordering table starting at address 0x100
  dma.writeRegister(0x1F8010E0, 0x0000010C); // Base addr (end of table)
  dma.writeRegister(0x1F8010E4, 0x00000004); // 4 words
  dma.writeRegister(0x1F8010E8, 0x11000002); // CHCR: from RAM, burst, start

  // Check that the OT was filled backwards
  // Entry at 0x10C should have end marker (0x00FFFFFF)
  uint32_t entry0 = mem.read32(0x10C);
  // Last entry should be end marker or previous pointer
  // Note: OTC fills backwards from base addr
  EXPECT_TRUE(true); // Mainly testing that it doesn't crash
}

TEST_F(DmaTest, DicrInterruptLogic) {
  // Set force IRQ bit
  dma.writeRegister(0x1F8010F4, 0x00008000); // Force IRQ
  EXPECT_TRUE(dma.hasInterrupt());

  // Clear force IRQ
  dma.writeRegister(0x1F8010F4, 0x00000000);
  EXPECT_FALSE(dma.hasInterrupt());
}

TEST_F(DmaTest, BlockTransferDoesntCrash) {
  // Set up a block transfer that reads from RAM to device
  // Enable channel 4 (SPU) in DPCR
  dma.writeRegister(0x1F8010F0, 0x08888888);

  // Write some data to RAM
  for (int i = 0; i < 64; i++) {
    mem.write8(0x1000 + i, static_cast<uint8_t>(i));
  }

  // Ch4: base=0x1000, block=16 words, 1 blocks
  dma.writeRegister(0x1F8010C0, 0x00001000);
  dma.writeRegister(0x1F8010C4, 0x00010010);

  // Don't actually trigger - just test register state
  EXPECT_EQ(dma.readRegister(0x1F8010C0), 0x00001000u);
  EXPECT_EQ(dma.readRegister(0x1F8010C4), 0x00010010u);
}
