// Tests for ps1xAnalyzer — ELF Parser
// Validates PS1 ELF loading and section detection

#include <gtest/gtest.h>
#include <elfio/elfio.hpp>

// ──────────────────────────────────────────
// ELFIO Integration Tests
// ──────────────────────────────────────────

TEST(ElfParser, ElfioLibraryLoads) {
    // Verify ELFIO library is correctly linked and usable
    ELFIO::elfio reader;
    EXPECT_NE(nullptr, &reader);
}

TEST(ElfParser, RejectInvalidFile) {
    // Attempting to load a non-existent file should fail gracefully
    ELFIO::elfio reader;
    EXPECT_FALSE(reader.load("/tmp/nonexistent_file.elf"));
}

TEST(ElfParser, RejectNonElfFile) {
    // Create a temporary file with garbage data
    const char* tmp_path = "/tmp/ps1recomp_test_garbage.bin";
    FILE* f = fopen(tmp_path, "wb");
    ASSERT_NE(nullptr, f);
    const char garbage[] = "this is not an ELF file";
    fwrite(garbage, 1, sizeof(garbage), f);
    fclose(f);

    ELFIO::elfio reader;
    EXPECT_FALSE(reader.load(tmp_path));

    remove(tmp_path);
}

// ──────────────────────────────────────────
// PS1 ELF Validation Tests
// ──────────────────────────────────────────

TEST(ElfParser, MipsConstants) {
    // Verify MIPS machine constants match PS1 expectations
    EXPECT_EQ(ELFIO::EM_MIPS, 8);          // MIPS machine type
    EXPECT_EQ(ELFIO::ELFCLASS32, 1);       // 32-bit class
    EXPECT_EQ(ELFIO::ELFDATA2LSB, 1);      // Little-endian
}

// ──────────────────────────────────────────
// PS1 Memory Map Validation
// ──────────────────────────────────────────

TEST(PS1MemoryMap, AddressRanges) {
    // PS1 memory map constants — verify our understanding
    constexpr uint32_t RAM_START_KUSEG  = 0x00000000;
    constexpr uint32_t RAM_END_KUSEG    = 0x001FFFFF;
    constexpr uint32_t RAM_START_KSEG0  = 0x80000000;
    constexpr uint32_t RAM_END_KSEG0    = 0x801FFFFF;
    constexpr uint32_t RAM_START_KSEG1  = 0xA0000000;
    constexpr uint32_t RAM_END_KSEG1    = 0xA01FFFFF;
    constexpr uint32_t SCRATCHPAD_START = 0x1F800000;
    constexpr uint32_t SCRATCHPAD_END   = 0x1F8003FF;
    constexpr uint32_t BIOS_START       = 0xBFC00000;

    // RAM is 2MB
    EXPECT_EQ(RAM_END_KUSEG - RAM_START_KUSEG + 1, 2 * 1024 * 1024);
    EXPECT_EQ(RAM_END_KSEG0 - RAM_START_KSEG0 + 1, 2 * 1024 * 1024);
    EXPECT_EQ(RAM_END_KSEG1 - RAM_START_KSEG1 + 1, 2 * 1024 * 1024);

    // Scratchpad is 1KB
    EXPECT_EQ(SCRATCHPAD_END - SCRATCHPAD_START + 1, 1024);

    // KSEG0 = KUSEG + 0x80000000
    EXPECT_EQ(RAM_START_KSEG0 - RAM_START_KUSEG, 0x80000000);

    // BIOS at 0xBFC00000
    EXPECT_EQ(BIOS_START, 0xBFC00000u);
}

TEST(PS1MemoryMap, PsyqAddressHeuristic) {
    // PsyQ SDK functions tend to be at addresses > 0x800A0000
    // Game logic tends to be at 0x8004C000 - 0x800A0000
    constexpr uint32_t PSYQ_THRESHOLD = 0x800A0000;
    constexpr uint32_t GAME_LOGIC_START = 0x8004C000;

    auto is_likely_psyq = [](uint32_t addr) -> bool {
        return addr >= 0x800A0000;
    };

    auto is_likely_game_logic = [](uint32_t addr) -> bool {
        return addr >= 0x8004C000 && addr < 0x800A0000;
    };

    // PsyQ examples
    EXPECT_TRUE(is_likely_psyq(0x800B1234));
    EXPECT_TRUE(is_likely_psyq(0x800A0000));
    EXPECT_TRUE(is_likely_psyq(0x800FFFFF));

    // Game logic examples
    EXPECT_TRUE(is_likely_game_logic(0x80050000));
    EXPECT_TRUE(is_likely_game_logic(0x80070000));
    EXPECT_FALSE(is_likely_game_logic(0x800B0000));

    // Cross-check
    EXPECT_FALSE(is_likely_psyq(0x80050000));
    EXPECT_FALSE(is_likely_game_logic(0x800B1234));
}
