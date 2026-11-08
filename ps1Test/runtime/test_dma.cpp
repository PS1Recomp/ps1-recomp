#include "runtime/dma/dma.h"
#include "runtime/memory.h"
#include "runtime/spu/spu.h"
#include <cstring>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <vector>

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

// Regression: a game writing 0 to DPCR must not disable GPU (Ch2) or OTC (Ch6).
// The real BIOS pre-enables these during init; our HLE must preserve them
// because games assume GPU DMA is available without explicitly enabling it.
TEST_F(DmaTest, DpcrForceEnablesGpuAndOtc) {
  // Write a value that explicitly clears both GPU (bit 11) and OTC (bit 27).
  dma.writeRegister(0x1F8010F0, 0x00000000);
  uint32_t result = dma.readRegister(0x1F8010F0);
  EXPECT_TRUE(result & (1u << 11)) << "GPU DMA (Ch2, bit 11) must always be enabled";
  EXPECT_TRUE(result & (1u << 27)) << "OTC DMA (Ch6, bit 27) must always be enabled";
}

TEST_F(DmaTest, DpcrForceEnablesGpuAndOtcPreservesOtherBits) {
  // Other channel bits must not be force-set -- only GPU and OTC.
  // Write with Ch0 (MDECin, bit 3) and Ch4 (SPU, bit 19) enabled.
  uint32_t written = (1u << 3) | (1u << 19);
  dma.writeRegister(0x1F8010F0, written);
  uint32_t result = dma.readRegister(0x1F8010F0);
  // Must keep the bits the game wrote
  EXPECT_TRUE(result & (1u << 3))  << "Ch0 bit must be preserved";
  EXPECT_TRUE(result & (1u << 19)) << "Ch4 bit must be preserved";
  // Must also keep GPU and OTC forced
  EXPECT_TRUE(result & (1u << 11)) << "GPU DMA must be force-enabled";
  EXPECT_TRUE(result & (1u << 27)) << "OTC DMA must be force-enabled";
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
  // psx-spx dmachannels.md, DMA6 OTC: starting at MADR and counting down,
  // each entry points at the one below it; the last (lowest) is FFFFFFh.
  dma.writeRegister(0x1F8010F0, 0x08888888); // enable every channel
  dma.writeRegister(0x1F8010E0, 0x0000010C); // MADR: top of a 4-entry table
  dma.writeRegister(0x1F8010E4, 0x00000004); // 4 words
  dma.writeRegister(0x1F8010E8, 0x11000002); // CHCR: backward, burst, start+trigger

  EXPECT_EQ(mem.read32(0x10C), 0x00000108u);
  EXPECT_EQ(mem.read32(0x108), 0x00000104u);
  EXPECT_EQ(mem.read32(0x104), 0x00000100u);
  EXPECT_EQ(mem.read32(0x100), 0x00FFFFFFu) << "end marker";
  EXPECT_EQ(mem.read32(0x0FC), 0u) << "nothing below the table";
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

// SPU channel (Ch4) RAM -> sound RAM.
//
// Regression: the transfer used to address sound RAM by the loop index, so
// every upload restarted at sound RAM 0 and overwrote the previous one. The
// voices read from their own start addresses, far from zero, and found
// silence. The destination is the SPU's transfer address register, which the
// game programs before starting the DMA and which auto-increments.
TEST_F(DmaTest, SpuBlockTransferHonoursTheProgrammedTransferAddress) {
  spu::SPU spu;
  spu.reset();
  dma.setSPU(&spu);

  spu.writeRegister(0x1F801DA6, 0x80); // sound RAM byte address 0x400

  mem.write32(0x1000, 0x22221111);
  mem.write32(0x1004, 0x44443333);

  dma.writeRegister(0x1F8010F0, 0x08888888); // enable Ch4
  dma.writeRegister(0x1F8010C0, 0x00001000); // MADR
  dma.writeRegister(0x1F8010C4, 0x00010002); // 1 block of 2 words
  dma.writeRegister(0x1F8010C8, 0x11000001); // from RAM, burst, trigger+start

  EXPECT_EQ(spu.readSoundRam(0x400), 0x1111);
  EXPECT_EQ(spu.readSoundRam(0x402), 0x2222);
  EXPECT_EQ(spu.readSoundRam(0x404), 0x3333);
  EXPECT_EQ(spu.readSoundRam(0x406), 0x4444);
  EXPECT_EQ(spu.readSoundRam(0x000), 0x0000) << "must not restart at zero";
}

// Channel register map
//
// Contract: psx-spx dmachannels.md "DMA Register Summary" and "D#_CHCR".

namespace {
constexpr uint32_t madr(uint32_t ch) { return 0x1F801080 + ch * 0x10; }
constexpr uint32_t bcr(uint32_t ch) { return 0x1F801084 + ch * 0x10; }
constexpr uint32_t chcr(uint32_t ch) { return 0x1F801088 + ch * 0x10; }
constexpr uint32_t kDpcr = 0x1F8010F0;
constexpr uint32_t kDicr = 0x1F8010F4;
constexpr uint32_t kStartBusy = 1u << 24;
constexpr uint32_t kTrigger = 1u << 28;
constexpr uint32_t kFromRam = 1u << 0;
constexpr uint32_t kBackward = 1u << 1;
constexpr uint32_t kSlice = 1u << 9;
constexpr uint32_t kLinkedList = 2u << 9;
constexpr uint32_t kAllEnabled = 0x08888888;
} // namespace

TEST_F(DmaTest, MadrKeepsOnlyTheLow24Bits) {
  dma.writeRegister(madr(4), 0xFF123456);

  EXPECT_EQ(dma.readRegister(madr(4)), 0x00123456u);
}

TEST_F(DmaTest, ChannelControlReadsBackWhatWasWritten) {
  dma.writeRegister(chcr(0), 0x00000201); // from RAM, slice, not started

  EXPECT_EQ(dma.readRegister(chcr(0)), 0x00000201u);
}

TEST_F(DmaTest, UnusedChannelRegisterSlotReadsZero) {
  EXPECT_EQ(dma.readRegister(0x1F80108C), 0u); // D0 offset 0Ch is unused
}

TEST_F(DmaTest, RegistersPastDicrReadZero) {
  EXPECT_EQ(dma.readRegister(0x1F8010FC), 0u);
}

// Starting a transfer

class DmaSpuTest : public DmaTest {
protected:
  spu::SPU spu;
  void SetUp() override {
    DmaTest::SetUp();
    spu.reset();
    dma.setSPU(&spu);
    mem.write32(0x1000, 0x22221111);
    mem.write32(0x1004, 0x44443333);
    dma.writeRegister(madr(4), 0x1000);
    dma.writeRegister(bcr(4), 0x00010002); // 1 block of 2 words
  }
};

TEST_F(DmaSpuTest, DisabledChannelDoesNotTransfer) {
  // The reset DPCR (07654321h) leaves channel 4's enable bit (19) clear.
  dma.writeRegister(chcr(4), kFromRam | kStartBusy | kTrigger);

  EXPECT_EQ(spu.readSoundRam(0), 0u);
  EXPECT_NE(dma.readRegister(chcr(4)) & kStartBusy, 0u) << "still pending";
}

TEST_F(DmaSpuTest, BurstModeWaitsForTheTriggerBit) {
  dma.writeRegister(kDpcr, kAllEnabled);

  dma.writeRegister(chcr(4), kFromRam | kStartBusy); // sync 0, no bit 28

  EXPECT_EQ(spu.readSoundRam(0), 0u);
}

TEST_F(DmaSpuTest, CompletionClearsTheStartBit) {
  dma.writeRegister(kDpcr, kAllEnabled);

  dma.writeRegister(chcr(4), kFromRam | kStartBusy | kTrigger);

  EXPECT_EQ(dma.readRegister(chcr(4)) & (kStartBusy | kTrigger), 0u);
  EXPECT_EQ(dma.readRegister(chcr(4)) & kFromRam, kFromRam);
}

TEST_F(DmaSpuTest, SliceModeStartsWithoutTheTriggerBit) {
  dma.writeRegister(kDpcr, kAllEnabled);

  dma.writeRegister(chcr(4), kFromRam | kSlice | kStartBusy);

  EXPECT_EQ(spu.readSoundRam(0), 0x1111);
  EXPECT_EQ(spu.readSoundRam(6), 0x4444);
}

TEST_F(DmaSpuTest, BackwardStepReadsDescendingAddresses) {
  dma.writeRegister(kDpcr, kAllEnabled);
  dma.writeRegister(madr(4), 0x1004);

  dma.writeRegister(chcr(4), kFromRam | kBackward | kStartBusy | kTrigger);

  EXPECT_EQ(spu.readSoundRam(0), 0x3333) << "first word comes from MADR";
  EXPECT_EQ(spu.readSoundRam(2), 0x4444);
  EXPECT_EQ(spu.readSoundRam(4), 0x1111) << "then MADR-4";
  EXPECT_EQ(spu.readSoundRam(6), 0x2222);
}

TEST_F(DmaSpuTest, BurstModeIgnoresTheBlockCountHalf) {
  // SyncMode 0: BCR bits 16-31 are unused, only the word count matters.
  dma.writeRegister(kDpcr, kAllEnabled);
  dma.writeRegister(bcr(4), 0x00000001); // 1 word, zero blocks

  dma.writeRegister(chcr(4), kFromRam | kStartBusy | kTrigger);

  EXPECT_EQ(spu.readSoundRam(0), 0x1111);
  EXPECT_EQ(spu.readSoundRam(2), 0x2222);
  EXPECT_EQ(spu.readSoundRam(4), 0x0000) << "only one word";
}

TEST_F(DmaSpuTest, ZeroWordCountMeans10000hWords) {
  dma.writeRegister(kDpcr, kAllEnabled);
  dma.writeRegister(bcr(4), 0x00000000);

  dma.writeRegister(chcr(4), kFromRam | kStartBusy | kTrigger);

  // 10000h words = 40000h bytes of sound RAM; the address register counts
  // in 8-byte units.
  EXPECT_EQ(spu.readRegister(0x1F801DA6), 0x40000 / 8);
}

TEST_F(DmaSpuTest, PendingTransferRunsOnceTheChannelIsEnabled) {
  dma.writeRegister(chcr(4), kFromRam | kStartBusy | kTrigger); // disabled now
  dma.writeRegister(kDpcr, kAllEnabled);

  dma.checkAndRunTransfers();

  EXPECT_EQ(spu.readSoundRam(0), 0x1111);
}

TEST_F(DmaSpuTest, LinkedListModeIsOnlyForTheGpu) {
  dma.writeRegister(kDpcr, kAllEnabled);

  dma.writeRegister(chcr(4), kFromRam | kLinkedList | kStartBusy);

  EXPECT_EQ(spu.readSoundRam(0), 0u);
}

TEST_F(DmaSpuTest, NothingIsTransferredWithoutMemoryAttached) {
  DMA bare;
  bare.setSPU(&spu);
  bare.writeRegister(kDpcr, kAllEnabled);
  bare.writeRegister(madr(4), 0x1000);
  bare.writeRegister(bcr(4), 0x00010002);

  bare.writeRegister(chcr(4), kFromRam | kStartBusy | kTrigger);

  EXPECT_EQ(spu.readSoundRam(0), 0u);
}

TEST_F(DmaSpuTest, CompletedChannelsAreReportedOnce) {
  dma.writeRegister(kDpcr, kAllEnabled);
  dma.writeRegister(chcr(4), kFromRam | kStartBusy | kTrigger);

  const uint8_t first = dma.takeCompleted();
  const uint8_t second = dma.takeCompleted();

  EXPECT_EQ(first, 1u << 4);
  EXPECT_EQ(second, 0u);
}

TEST_F(DmaTest, OtcZeroCountMeans10000hEntries) {
  dma.writeRegister(kDpcr, kAllEnabled);
  dma.writeRegister(madr(6), 0x0007FFFC);
  dma.writeRegister(bcr(6), 0x00000000);

  dma.writeRegister(chcr(6), 0x11000002);

  const uint32_t lowest = 0x0007FFFC - 0xFFFF * 4;
  EXPECT_EQ(mem.read32(lowest), 0x00FFFFFFu);
  EXPECT_EQ(mem.read32(lowest + 4), lowest);
}

// DICR

TEST_F(DmaTest, CompletionRaisesTheMasterFlagWhenTheChannelIrqIsEnabled) {
  dma.writeRegister(kDpcr, kAllEnabled);
  dma.writeRegister(kDicr, (1u << 23) | (1u << (16 + 6))); // master + ch6

  dma.writeRegister(madr(6), 0x100);
  dma.writeRegister(bcr(6), 1);
  dma.writeRegister(chcr(6), 0x11000002);

  EXPECT_NE(dma.readRegister(kDicr) & (1u << (24 + 6)), 0u) << "ch6 flag";
  EXPECT_TRUE(dma.hasInterrupt());
}

TEST_F(DmaTest, WritingOneToAFlagAcknowledgesIt) {
  const uint32_t enable = (1u << 23) | (1u << (16 + 6));
  dma.writeRegister(kDpcr, kAllEnabled);
  dma.writeRegister(kDicr, enable);
  dma.writeRegister(madr(6), 0x100);
  dma.writeRegister(bcr(6), 1);
  dma.writeRegister(chcr(6), 0x11000002);

  dma.writeRegister(kDicr, enable | (1u << (24 + 6)));

  EXPECT_EQ(dma.readRegister(kDicr) & 0x7F000000, 0u);
  EXPECT_FALSE(dma.hasInterrupt());
  EXPECT_EQ(dma.readRegister(kDicr) & 0x00FF0000, enable) << "enables stay";
}

// GPU (DMA2)

class DmaGpuTest : public DmaTest {
protected:
  std::unique_ptr<gpu::GPU> gpu = std::make_unique<gpu::GPU>();
  void SetUp() override {
    DmaTest::SetUp();
    dma.setGPU(gpu.get());
    dma.writeRegister(kDpcr, kAllEnabled);
  }
  // VRAM pixel (x, 0).
  uint16_t pixel(uint32_t x) const { return gpu->getVRAM()[x].raw; }
};

TEST_F(DmaGpuTest, BlockTransferFeedsWordsToGp0) {
  // GP0(02h) fill 16x1 at (0,0) with pure red, as a 3-word block.
  mem.write32(0x2000, 0x020000FF);
  mem.write32(0x2004, 0x00000000);
  mem.write32(0x2008, 0x00010010);
  dma.writeRegister(madr(2), 0x2000);
  dma.writeRegister(bcr(2), 0x00010003);

  dma.writeRegister(chcr(2), kFromRam | kSlice | kStartBusy);

  EXPECT_EQ(pixel(0), 0x001F);
  EXPECT_EQ(pixel(15), 0x001F);
}

TEST_F(DmaGpuTest, LinkedListTransferWalksThePacketChain) {
  // Two nodes: the first carries the fill command, the second (end marker
  // FFFFFFh in its header) carries nothing.  Header: [31:24] word count,
  // [23:0] next node.  psx-spx dmachannels.md "DMA2 linked list".
  mem.write32(0x3000, 0x03003100);
  mem.write32(0x3004, 0x02FF0000); // blue
  mem.write32(0x3008, 0x00000000);
  mem.write32(0x300C, 0x00010010);
  mem.write32(0x3100, 0x00FFFFFF);
  dma.writeRegister(madr(2), 0x3000);

  dma.writeRegister(chcr(2), kFromRam | kLinkedList | kStartBusy);

  EXPECT_EQ(pixel(0), 0x7C00);
}

// CDROM (DMA3)

namespace {
class FilledFs : public cdrom::VirtualFs {
public:
  std::optional<cdrom::Sector> readSector(uint32_t) override {
    cdrom::Sector s{};
    std::memset(s.raw, 0x5C, cdrom::SECTOR_SIZE_RAW);
    return s;
  }
};

void bufferOneSector(cdrom::CdromController &cd) {
  cd.writeRegister(0x1F801800, 0x00);
  cd.writeRegister(0x1F801802, 0x00); // minute
  cd.writeRegister(0x1F801802, 0x02); // second
  cd.writeRegister(0x1F801802, 0x00); // sector
  cd.writeRegister(0x1F801801, 0x02); // SetLoc
  cd.tick(100000);
  cd.writeRegister(0x1F801801, 0x06); // ReadN
  for (int i = 0; i < 40 && !cd.hasSectorReady(); ++i)
    cd.tick(100000);
}
} // namespace

class DmaCdromTest : public DmaTest {
protected:
  cdrom::CdromController cd;
  FilledFs fs;
  void SetUp() override {
    DmaTest::SetUp();
    cd.reset();
    cd.attachVirtualFs(&fs);
    dma.writeRegister(kDpcr, kAllEnabled);
    dma.writeRegister(madr(3), 0x4000);
    dma.writeRegister(bcr(3), 0x00010200); // one 2048-byte sector
    for (uint32_t a = 0x4000; a < 0x4800; a += 4)
      mem.write32(a, 0xEEEEEEEE);
  }
};

TEST_F(DmaCdromTest, WaitsForASectorBeforeTransferring) {
  dma.setCDROM(&cd);

  dma.writeRegister(chcr(3), kStartBusy | kTrigger);

  EXPECT_NE(dma.readRegister(chcr(3)) & kStartBusy, 0u) << "still waiting";
  EXPECT_EQ(mem.read32(0x4000), 0xEEEEEEEEu);
}

TEST_F(DmaCdromTest, WaitsWhenNoDriveIsAttached) {
  dma.writeRegister(chcr(3), kStartBusy | kTrigger);

  EXPECT_NE(dma.readRegister(chcr(3)) & kStartBusy, 0u);
}

TEST_F(DmaCdromTest, CopiesTheBufferedSectorIntoRam) {
  dma.setCDROM(&cd);
  bufferOneSector(cd);
  ASSERT_TRUE(cd.hasSectorReady());

  dma.writeRegister(chcr(3), kStartBusy | kTrigger);

  EXPECT_EQ(mem.read32(0x4000), 0x5C5C5C5Cu);
  EXPECT_EQ(mem.read32(0x47FC), 0x5C5C5C5Cu);
  EXPECT_FALSE(cd.hasSectorReady()) << "the sector is consumed";
}

// MDEC (DMA0 / DMA1)

TEST_F(DmaTest, MdecInFeedsCommandWords) {
  mdec::MDEC mdec;
  dma.setMDEC(&mdec);
  dma.writeRegister(kDpcr, kAllEnabled);
  mem.write32(0x5000, 0x40000000); // MDEC(2): quant table, 16 words follow
  dma.writeRegister(madr(0), 0x5000);
  dma.writeRegister(bcr(0), 0x00010001);

  dma.writeRegister(chcr(0), kFromRam | kSlice | kStartBusy);

  EXPECT_NE(mdec.readStatus() & (1u << 29), 0u) << "busy receiving the table";
}

TEST_F(DmaTest, MdecOutDeliversTheSameWordsAsTheDataPort) {
  // Decode one 15-bit macroblock of six flat blocks (DC only, then EOB FE00h)
  // in two MDECs; one is drained through DMA1, the other through 1F801820h.
  auto feed = [](mdec::MDEC &m) {
    m.writeCommand(0x38000006); // MDEC(1), 15-bit output, 6 words follow
    for (int b = 0; b < 6; ++b)
      m.writeCommand(0xFE000040); // DC=40h (q-scale 0), then end of block
  };
  mdec::MDEC viaDma, viaPort;
  feed(viaDma);
  feed(viaPort);
  std::vector<uint32_t> expected;
  while (viaPort.dmaOutReady())
    expected.push_back(viaPort.readData());
  ASSERT_FALSE(expected.empty()) << "the reference decode produced nothing";
  dma.setMDEC(&viaDma);
  dma.writeRegister(kDpcr, kAllEnabled);
  dma.writeRegister(madr(1), 0x6000);
  dma.writeRegister(bcr(1), 0x00010000 | static_cast<uint32_t>(expected.size()));

  dma.writeRegister(chcr(1), kSlice | kStartBusy); // MDEC -> RAM

  for (size_t i = 0; i < expected.size(); ++i)
    ASSERT_EQ(mem.read32(0x6000 + 4 * static_cast<uint32_t>(i)), expected[i]) << i;
  EXPECT_FALSE(viaDma.dmaOutReady()) << "the FIFO is drained";
}
