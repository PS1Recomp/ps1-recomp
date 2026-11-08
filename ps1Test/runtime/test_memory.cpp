// Tests for ps1Runtime -- Memory Subsystem
// Validates RAM, scratchpad, BIOS, address mirroring, and byte order

#include <gtest/gtest.h>
#include <runtime/memory.h>
#include <memory>
#include <vector>

using namespace ps1;

// Basic Read/Write

TEST(Memory, ReadWrite8) {
  Memory mem;
  mem.write8(0x00000000, 0x42);
  EXPECT_EQ(mem.read8(0x00000000), 0x42u);

  mem.write8(0x001FFFFF, 0xFF);
  EXPECT_EQ(mem.read8(0x001FFFFF), 0xFFu);
}

TEST(Memory, ReadWrite16LittleEndian) {
  Memory mem;
  mem.write16(0x00001000, 0xBEEF);
  EXPECT_EQ(mem.read16(0x00001000), 0xBEEFu);

  // Verify byte order (little-endian)
  EXPECT_EQ(mem.read8(0x00001000), 0xEF); // low byte
  EXPECT_EQ(mem.read8(0x00001001), 0xBE); // high byte
}

TEST(Memory, ReadWrite32LittleEndian) {
  Memory mem;
  mem.write32(0x00002000, 0xDEADBEEF);
  EXPECT_EQ(mem.read32(0x00002000), 0xDEADBEEFu);

  // Verify byte order (LE: 0xDEADBEEF -> EF BE AD DE)
  EXPECT_EQ(mem.read8(0x00002000), 0xEF);
  EXPECT_EQ(mem.read8(0x00002001), 0xBE);
  EXPECT_EQ(mem.read8(0x00002002), 0xAD);
  EXPECT_EQ(mem.read8(0x00002003), 0xDE);
}

// Address Mirroring (KSEG0 / KSEG1)

TEST(Memory, KSEG0Mirror) {
  Memory mem;
  // Write via KUSEG (physical)
  mem.write32(0x00010000, 0xCAFEBABE);

  // Read via KSEG0 (0x80000000 mirror)
  EXPECT_EQ(mem.read32(0x80010000), 0xCAFEBABEu);
}

TEST(Memory, KSEG1Mirror) {
  Memory mem;
  // Write via KSEG1 (0xA0000000 mirror)
  mem.write32(0xA0020000, 0x12345678);

  // Read via KUSEG and KSEG0
  EXPECT_EQ(mem.read32(0x00020000), 0x12345678u);
  EXPECT_EQ(mem.read32(0x80020000), 0x12345678u);
}

TEST(Memory, CrossMirrorWrite) {
  Memory mem;
  // Write via KSEG0, read via KSEG1
  mem.write32(0x80030000, 0xAAAABBBB);
  EXPECT_EQ(mem.read32(0xA0030000), 0xAAAABBBBu);
}

// Scratchpad (Data Cache)

TEST(Memory, ScratchpadReadWrite) {
  Memory mem;
  mem.write32(0x1F800000, 0x11223344);
  EXPECT_EQ(mem.read32(0x1F800000), 0x11223344u);

  mem.write8(0x1F8003FF, 0xAB);
  EXPECT_EQ(mem.read8(0x1F8003FF), 0xABu);
}

TEST(Memory, ScratchpadIsolated) {
  Memory mem;
  // Scratchpad and RAM are separate
  mem.write32(0x1F800000, 0xAAAAAAAA);
  mem.write32(0x00000000, 0xBBBBBBBB);

  EXPECT_EQ(mem.read32(0x1F800000), 0xAAAAAAAAu);
  EXPECT_EQ(mem.read32(0x00000000), 0xBBBBBBBBu);
}

// BIOS ROM

TEST(Memory, BiosReadOnly) {
  Memory mem;
  // BIOS initializes as 0xFF
  EXPECT_EQ(mem.read8(0x1FC00000), 0xFFu);

  // Writes to BIOS are ignored
  mem.write8(0x1FC00000, 0x42);
  EXPECT_EQ(mem.read8(0x1FC00000), 0xFFu);
}

TEST(Memory, BiosLoadViaDirect) {
  Memory mem;
  // Load BIOS data directly
  uint8_t *bios = mem.biosPtr();
  bios[0] = 0x50; // 'P'
  bios[1] = 0x53; // 'S'

  EXPECT_EQ(mem.read8(0x1FC00000), 0x50u);
  EXPECT_EQ(mem.read8(0x1FC00001), 0x53u);
}

// Reset

TEST(Memory, Reset) {
  Memory mem;
  mem.write32(0x00010000, 0xDEADBEEF);
  mem.write32(0x1F800000, 0xCAFEBABE);

  mem.reset();

  EXPECT_EQ(mem.read32(0x00010000), 0u);
  EXPECT_EQ(mem.read32(0x1F800000), 0u);
  EXPECT_EQ(mem.read8(0x1FC00000), 0xFFu); // BIOS stays 0xFF
}

// Unmapped Regions

TEST(Memory, UnmappedReturnsZero) {
  Memory mem;
  // I/O port range -- returns 0 for now
  EXPECT_EQ(mem.read32(0x1F801000), 0u);
}

// Direct Pointer Access

TEST(Memory, DirectRamAccess) {
  Memory mem;
  uint8_t *ram = mem.ramPtr();
  ram[0x100] = 0xAA;
  ram[0x101] = 0xBB;

  EXPECT_EQ(mem.read16(0x00000100), 0xBBAAu);
  EXPECT_EQ(mem.read16(0x80000100), 0xBBAAu); // KSEG0 mirror
}

// I/O Routing
//
// Contract: psx-spx "Memory Map" and "I/O Map" (memorymap.md, iomap.md).
// Every hardware register is reached through Memory, at its physical address
// or through the KSEG0/KSEG1 mirrors, so these tests observe each device
// through the bus and check that what was written lands in that device.

namespace {

struct AttachedDevices {
  std::unique_ptr<Memory> mem = std::make_unique<Memory>();
  std::unique_ptr<gpu::GPU> gpu = std::make_unique<gpu::GPU>();
  spu::SPU spu;
  DMA dma;
  cdrom::CdromController cdrom;
  input::InputController input;
  mdec::MDEC mdec;
  Timers timers;
  InterruptController irq;

  AttachedDevices() {
    mem->setGPU(gpu.get());
    mem->setSPU(&spu);
    mem->setDMA(&dma);
    mem->setCDROM(&cdrom);
    mem->setInput(&input);
    mem->setMDEC(&mdec);
    mem->setTimers(&timers);
    mem->setInterruptController(&irq);
  }
};

} // namespace

TEST(MemoryIo, SpuRegistersAreReachedWith16BitAccesses) {
  AttachedDevices d;

  d.mem->write16(0x1F801D80, 0x3FFF); // main volume left

  EXPECT_EQ(d.spu.readRegister(0x1F801D80), 0x3FFF);
  EXPECT_EQ(d.mem->read16(0xBF801D80), 0x3FFF) << "KSEG1 mirror of the same register";
}

TEST(MemoryIo, A32BitSpuWriteStoresTheLowHalfFirst) {
  AttachedDevices d;

  d.mem->write32(0x1F801D80, 0x22221111); // main volume left, then right

  EXPECT_EQ(d.spu.readRegister(0x1F801D80), 0x1111);
  EXPECT_EQ(d.spu.readRegister(0x1F801D82), 0x2222);
  EXPECT_EQ(d.mem->read32(0x1F801D80), 0x22221111u);
}

TEST(MemoryIo, TimerRegistersAreReachedWith16BitAccesses) {
  AttachedDevices d;

  d.mem->write16(0x1F801118, 1234); // timer 1 target

  EXPECT_EQ(d.timers.readRegister(0x1F801118), 1234);
  EXPECT_EQ(d.mem->read16(0x1F801118), 1234);
}

TEST(MemoryIo, ControllerRegistersAreReachedWith16And32BitAccesses) {
  AttachedDevices d;

  d.mem->write16(0x1F80104E, 0x0088); // JOY_BAUD
  d.mem->write32(0x1F801040, 0x01);   // JOY_DATA: start a pad exchange

  EXPECT_EQ(d.mem->read16(0x1F80104E), 0x0088);
  EXPECT_EQ(d.mem->read32(0x1F801040), 0xFFu) << "the pad answers 01h with Hi-Z";
  EXPECT_EQ(d.mem->read32(0x1F801044), d.input.readRegister(0x1F801044));
}

TEST(MemoryIo, InterruptMaskIsReachedWith16BitAccesses) {
  AttachedDevices d;

  d.mem->write16(0x1F801074, IRQ_VBLANK | IRQ_DMA);

  EXPECT_EQ(d.irq.readIMask(), IRQ_VBLANK | IRQ_DMA);
  EXPECT_EQ(d.mem->read16(0x1F801074), IRQ_VBLANK | IRQ_DMA);
}

TEST(MemoryIo, InterruptStatusIsAcknowledgedByWritingZeroBits16Bit) {
  AttachedDevices d;
  d.irq.raiseInterrupt(IRQ_VBLANK | IRQ_CDROM);

  d.mem->write16(0x1F801070, static_cast<uint16_t>(~IRQ_VBLANK));

  EXPECT_EQ(d.mem->read16(0x1F801070), IRQ_CDROM);
}

TEST(MemoryIo, InterruptRegistersAreReachedWith32BitAccesses) {
  AttachedDevices d;
  d.irq.raiseInterrupt(IRQ_TMR0 | IRQ_SPU);

  d.mem->write32(0x1F801074, IRQ_SPU);
  d.mem->write32(0x1F801070, ~IRQ_TMR0);

  EXPECT_EQ(d.mem->read32(0x1F801074), IRQ_SPU);
  EXPECT_EQ(d.mem->read32(0x1F801070), IRQ_SPU);
}

TEST(MemoryIo, DmaRegistersAreReachedWith32BitAccesses) {
  AttachedDevices d;

  d.mem->write32(0x1F8010C0, 0x00123450); // DMA4 MADR

  EXPECT_EQ(d.dma.readRegister(0x1F8010C0), 0x00123450u);
  EXPECT_EQ(d.mem->read32(0x9F8010C0), 0x00123450u) << "KSEG0 mirror";
}

TEST(MemoryIo, Gp1WritesReachTheGpuAndGpustatReadsBack) {
  AttachedDevices d;

  d.mem->write32(0x1F801814, 0x04000002); // GP1(04h): DMA direction = CPU->GPU

  EXPECT_EQ((d.mem->read32(0x1F801814) >> 29) & 3, 2u) << "GPUSTAT.29-30";
}

TEST(MemoryIo, Gp0WritesReachTheGpuAndGpureadReturnsVram) {
  AttachedDevices d;
  // GP0(02h) fill 16x1 at (0,0) with pure red (15-bit 001Fh).
  d.mem->write32(0x1F801810, 0x020000FF);
  d.mem->write32(0x1F801810, 0x00000000);
  d.mem->write32(0x1F801810, 0x00010010);
  // GP0(C0h) copy a 2x1 rectangle from VRAM to the CPU.
  d.mem->write32(0x1F801810, 0xC0000000);
  d.mem->write32(0x1F801810, 0x00000000);
  d.mem->write32(0x1F801810, 0x00010002);

  const uint32_t pixels = d.mem->read32(0x1F801810);

  EXPECT_EQ(pixels, 0x001F001Fu);
}

TEST(MemoryIo, MdecRegistersAreReachedWith32BitAccesses) {
  AttachedDevices d;

  d.mem->write32(0x1F801820, 0x40000000); // MDEC(2): set quant table, 16 words follow

  EXPECT_NE(d.mem->read32(0x1F801824) & (1u << 29), 0u) << "busy receiving the table";
  d.mem->write32(0x1F801824, 0x80000000); // MDEC1 reset
  EXPECT_EQ(d.mem->read32(0x1F801824) & (1u << 29), 0u);
  EXPECT_EQ(d.mem->read32(0x1F801820), 0u) << "no decoded data to read";
}

TEST(MemoryIo, CdromIndexRegisterIsReachedWith8And32BitAccesses) {
  AttachedDevices d;

  d.mem->write8(0x1F801800, 0x02);
  const uint8_t afterByteWrite = d.mem->read8(0x1F801800) & 3;
  d.mem->write32(0x1F801800, 0x01);
  const uint32_t afterWordWrite = d.mem->read32(0x1F801800) & 3;

  EXPECT_EQ(afterByteWrite, 2u);
  EXPECT_EQ(afterWordWrite, 1u);
}

TEST(MemoryIo, UnattachedDevicesReadAsZeroAndIgnoreWrites) {
  auto mem = std::make_unique<Memory>();
  const uint32_t regs32[] = {0x1F801810, 0x1F801814, 0x1F8010F0, 0x1F801070,
                             0x1F801074, 0x1F801820, 0x1F801824, 0x1F801040,
                             0x1F801800, 0x1F801D80};
  const uint32_t regs16[] = {0x1F801D80, 0x1F801100, 0x1F801044, 0x1F801070,
                             0x1F801074};

  for (uint32_t a : regs32)
    mem->write32(a, 0xFFFFFFFF);
  for (uint32_t a : regs16)
    mem->write16(a, 0xFFFF);
  mem->write8(0x1F801800, 0xFF);

  for (uint32_t a : regs32)
    EXPECT_EQ(mem->read32(a), 0u) << std::hex << a;
  for (uint32_t a : regs16)
    EXPECT_EQ(mem->read16(a), 0u) << std::hex << a;
  EXPECT_EQ(mem->read8(0x1F801800), 0u);
}

TEST(MemoryIo, RamSizeRegisterReadsTheBootDefault) {
  auto mem = std::make_unique<Memory>();

  mem->write32(0x1F801060, 0); // writes are ignored

  // psx-spx memorycontrol.md: RAM_SIZE (1F801060h) is usually 00000B88h.
  EXPECT_EQ(mem->read32(0x1F801060), 0x00000B88u);
}

TEST(MemoryIo, MemoryControlRegistersIgnoreWrites) {
  auto mem = std::make_unique<Memory>();

  mem->write32(0x1F801000, 0x1F000000); // Expansion 1 base

  EXPECT_EQ(mem->read32(0x1F801000), 0u);
}

TEST(MemoryIo, CacheControlRegisterIgnoresWrites) {
  auto mem = std::make_unique<Memory>();

  mem->write32(0xFFFE0130, 0x0001E988);

  EXPECT_EQ(mem->read32(0xFFFE0130), 0u);
}

TEST(MemoryIo, UnhandledIoByteWritesAreDropped) {
  auto mem = std::make_unique<Memory>();

  mem->write32(0x1F801050, 0xAABBCCDD); // SIO, not modelled

  EXPECT_EQ(mem->read32(0x1F801050), 0u);
}

// Address Mirroring (RAM in the first 8 MB)

TEST(Memory, RamIsMirroredFourTimesInTheFirst8MB) {
  auto mem = std::make_unique<Memory>();

  mem->write32(0x00600010, 0x01020304); // fourth mirror

  EXPECT_EQ(mem->read32(0x00000010), 0x01020304u);
  EXPECT_EQ(mem->read32(0x80200010), 0x01020304u);
}

TEST(Memory, ScratchpadEndsAfter1KB) {
  auto mem = std::make_unique<Memory>();

  mem->write8(0x1F800400, 0x77); // first byte past the scratchpad

  EXPECT_EQ(mem->read8(0x1F800400), 0u);
  EXPECT_EQ(mem->read8(0x1F800000), 0u) << "must not wrap into the scratchpad";
}

TEST(Memory, BiosIsReadableThroughKseg1) {
  auto mem = std::make_unique<Memory>();
  mem->biosPtr()[4] = 0x5A;

  EXPECT_EQ(mem->read8(0xBFC00004), 0x5Au);
}

// Write Watchpoint

TEST(MemoryWatchpoint, FiresWithTheValueWrittenThroughAnyMirror) {
  auto mem = std::make_unique<Memory>();
  std::vector<uint32_t> seen;
  mem->setWriteWatchpoint(0x80001000, [&](uint32_t v) { seen.push_back(v); });

  mem->write32(0x00001000, 0x11);
  mem->write32(0xA0001000, 0x22);

  EXPECT_EQ(seen, (std::vector<uint32_t>{0x11, 0x22}));
}

TEST(MemoryWatchpoint, IgnoresWritesToOtherAddresses) {
  auto mem = std::make_unique<Memory>();
  int calls = 0;
  mem->setWriteWatchpoint(0x80001000, [&](uint32_t) { ++calls; });

  mem->write32(0x80001004, 0x11);

  EXPECT_EQ(calls, 0);
}

TEST(MemoryWatchpoint, StopsFiringOnceCleared) {
  auto mem = std::make_unique<Memory>();
  int calls = 0;
  mem->setWriteWatchpoint(0x80001000, [&](uint32_t) { ++calls; });

  mem->clearWriteWatchpoint();
  mem->write32(0x80001000, 0x11);

  EXPECT_EQ(calls, 0);
  EXPECT_EQ(mem->read32(0x80001000), 0x11u) << "the write itself still lands";
}
