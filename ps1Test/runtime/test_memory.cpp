// Tests for ps1Runtime -- Memory Subsystem
// Validates RAM, scratchpad, BIOS, address mirroring, and byte order

#include <gtest/gtest.h>
#include <runtime/memory.h>

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
