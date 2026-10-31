// Tests for ps1Analyzer -- ELF Parser
// Tests ElfParser class: loading, validation, section/symbol extraction

#include <gtest/gtest.h>
#include <unistd.h>
#include <ps1recomp/elf_parser.h>
#include <elfio/elfio.hpp>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <filesystem>

using namespace ps1recomp;

// Helper: Create a minimal valid PS1 ELF

static std::string createMinimalPS1Elf(const std::string& path,
                                        uint32_t entryPoint = 0x80010000,
                                        bool addSymbols = false) {
    ELFIO::elfio writer;
    writer.create(ELFIO::ELFCLASS32, ELFIO::ELFDATA2LSB);
    writer.set_os_abi(ELFIO::ELFOSABI_NONE);
    writer.set_type(ELFIO::ET_EXEC);
    writer.set_machine(ELFIO::EM_MIPS);
    writer.set_entry(entryPoint);

    // Add .text section with some MIPS NOP instructions
    auto* text_sec = writer.sections.add(".text");
    text_sec->set_type(ELFIO::SHT_PROGBITS);
    text_sec->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_EXECINSTR);
    text_sec->set_addr_align(4);
    text_sec->set_address(entryPoint);

    // 16 NOP instructions (64 bytes)
    std::vector<uint8_t> text_data(64, 0x00);
    text_sec->set_data(reinterpret_cast<const char*>(text_data.data()), text_data.size());

    // Add .data section
    auto* data_sec = writer.sections.add(".data");
    data_sec->set_type(ELFIO::SHT_PROGBITS);
    data_sec->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_WRITE);
    data_sec->set_addr_align(4);
    data_sec->set_address(entryPoint + 0x1000);

    std::vector<uint8_t> data_bytes = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    data_sec->set_data(reinterpret_cast<const char*>(data_bytes.data()), data_bytes.size());

    // Add .bss section
    auto* bss_sec = writer.sections.add(".bss");
    bss_sec->set_type(ELFIO::SHT_NOBITS);
    bss_sec->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_WRITE);
    bss_sec->set_addr_align(4);
    bss_sec->set_address(entryPoint + 0x2000);
    bss_sec->set_size(256);

    // Add .rodata section
    auto* rodata_sec = writer.sections.add(".rodata");
    rodata_sec->set_type(ELFIO::SHT_PROGBITS);
    rodata_sec->set_flags(ELFIO::SHF_ALLOC);
    rodata_sec->set_addr_align(4);
    rodata_sec->set_address(entryPoint + 0x3000);
    std::vector<uint8_t> rodata = {0xAA, 0xBB, 0xCC, 0xDD};
    rodata_sec->set_data(reinterpret_cast<const char*>(rodata.data()), rodata.size());

    // Add symbols if requested
    if (addSymbols) {
        auto* strtab = writer.sections.add(".strtab");
        strtab->set_type(ELFIO::SHT_STRTAB);
        ELFIO::string_section_accessor stra(strtab);

        auto* symtab = writer.sections.add(".symtab");
        symtab->set_type(ELFIO::SHT_SYMTAB);
        symtab->set_info(2);
        symtab->set_addr_align(4);
        symtab->set_entry_size(writer.get_default_entry_size(ELFIO::SHT_SYMTAB));
        symtab->set_link(strtab->get_index());

        ELFIO::symbol_section_accessor syma(writer, symtab);

        // Function symbol
        syma.add_symbol(stra, "main", entryPoint, 32,
                        ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0,
                        text_sec->get_index());

        // Another function
        syma.add_symbol(stra, "game_loop", entryPoint + 0x20, 16,
                        ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0,
                        text_sec->get_index());

        // Data symbol
        syma.add_symbol(stra, "player_hp", entryPoint + 0x1000, 4,
                        ELFIO::STB_GLOBAL, ELFIO::STT_OBJECT, 0,
                        data_sec->get_index());

        // PsyQ-range function
        syma.add_symbol(stra, "GsInitGraph", 0x800B5000, 64,
                        ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0,
                        text_sec->get_index());
    }

    // Add a LOAD segment
    auto* segment = writer.segments.add();
    segment->set_type(ELFIO::PT_LOAD);
    segment->set_virtual_address(entryPoint);
    segment->set_physical_address(entryPoint);
    segment->set_flags(ELFIO::PF_X | ELFIO::PF_R);
    segment->set_align(0x1000);
    segment->add_section_index(text_sec->get_index(), text_sec->get_addr_align());

    writer.save(path);
    return path;
}

static void cleanupFile(const std::string& path) {
    std::remove(path.c_str());
}

// Basic Loading Tests

TEST(ElfParser, LoadValidPS1Elf) {
    const std::string path = "/tmp/ps1recomp_test_valid.elf";
    createMinimalPS1Elf(path);

    ElfParser parser;
    EXPECT_TRUE(parser.load(path));
    EXPECT_TRUE(parser.isLoaded());
    EXPECT_TRUE(parser.getError().empty());

    cleanupFile(path);
}

TEST(ElfParser, RejectNonExistentFile) {
    ElfParser parser;
    EXPECT_FALSE(parser.load("/tmp/does_not_exist_at_all.elf"));
    EXPECT_FALSE(parser.isLoaded());
    EXPECT_FALSE(parser.getError().empty());
}

TEST(ElfParser, RejectGarbageFile) {
    const std::string path = "/tmp/ps1recomp_test_garbage.bin";
    {
        std::ofstream f(path, std::ios::binary);
        const char garbage[] = "this is definitely not an ELF file";
        f.write(garbage, sizeof(garbage));
    }

    ElfParser parser;
    EXPECT_FALSE(parser.load(path));
    EXPECT_FALSE(parser.isLoaded());

    cleanupFile(path);
}

TEST(ElfParser, RejectEmptyFile) {
    const std::string path = "/tmp/ps1recomp_test_empty.bin";
    { std::ofstream f(path, std::ios::binary); }

    ElfParser parser;
    EXPECT_FALSE(parser.load(path));

    cleanupFile(path);
}

// Entry Point Tests

TEST(ElfParser, EntryPointKSEG0) {
    const std::string path = "/tmp/ps1recomp_test_kseg0.elf";
    createMinimalPS1Elf(path, 0x80010000);

    ElfParser parser;
    ASSERT_TRUE(parser.load(path));
    EXPECT_EQ(parser.getEntryPoint(), 0x80010000u);

    cleanupFile(path);
}

TEST(ElfParser, EntryPointKUSEG) {
    const std::string path = "/tmp/ps1recomp_test_kuseg.elf";
    createMinimalPS1Elf(path, 0x00010000);

    ElfParser parser;
    ASSERT_TRUE(parser.load(path));
    EXPECT_EQ(parser.getEntryPoint(), 0x00010000u);

    cleanupFile(path);
}

TEST(ElfParser, RejectBadEntryPoint) {
    // Entry point outside RAM range
    const std::string path = "/tmp/ps1recomp_test_bad_entry.elf";
    createMinimalPS1Elf(path, 0xDEADBEEF);

    ElfParser parser;
    EXPECT_FALSE(parser.load(path));
    EXPECT_FALSE(parser.getError().empty());

    cleanupFile(path);
}

// Section Extraction Tests

TEST(ElfParser, ExtractSections) {
    const std::string path = "/tmp/ps1recomp_test_sections.elf";
    createMinimalPS1Elf(path);

    ElfParser parser;
    ASSERT_TRUE(parser.load(path));

    // Should have at least .text, .data, .bss, .rodata
    EXPECT_GE(parser.getSections().size(), 4u);

    // Find sections by name
    auto* text = parser.findSection(".text");
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->type, SectionType::Text);
    EXPECT_EQ(text->size, 64u);
    EXPECT_NE(text->data, nullptr);

    auto* data = parser.findSection(".data");
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->type, SectionType::Data);
    EXPECT_EQ(data->size, 8u);

    auto* bss = parser.findSection(".bss");
    ASSERT_NE(bss, nullptr);
    EXPECT_EQ(bss->type, SectionType::Bss);
    EXPECT_EQ(bss->size, 256u);
    EXPECT_EQ(bss->data, nullptr); // BSS has no data

    auto* rodata = parser.findSection(".rodata");
    ASSERT_NE(rodata, nullptr);
    EXPECT_EQ(rodata->type, SectionType::Data); // rodata classified as Data

    cleanupFile(path);
}

TEST(ElfParser, GetTextSection) {
    const std::string path = "/tmp/ps1recomp_test_textsec.elf";
    createMinimalPS1Elf(path);

    ElfParser parser;
    ASSERT_TRUE(parser.load(path));

    auto* text = parser.getTextSection();
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->name, ".text");
    EXPECT_EQ(text->vaddr, 0x80010000u);

    cleanupFile(path);
}

TEST(ElfParser, FindSectionByAddress) {
    const std::string path = "/tmp/ps1recomp_test_findaddr.elf";
    createMinimalPS1Elf(path, 0x80010000);

    ElfParser parser;
    ASSERT_TRUE(parser.load(path));

    // Address in .text (0x80010000 + 0x20 = 0x80010020)
    auto* sec = parser.findSectionByAddress(0x80010020);
    ASSERT_NE(sec, nullptr);
    EXPECT_EQ(sec->name, ".text");

    // Address in .data (0x80010000 + 0x1000 = 0x80011000)
    sec = parser.findSectionByAddress(0x80011004);
    ASSERT_NE(sec, nullptr);
    EXPECT_EQ(sec->name, ".data");

    // Address nowhere
    sec = parser.findSectionByAddress(0xFFFFFFFF);
    EXPECT_EQ(sec, nullptr);

    cleanupFile(path);
}

TEST(ElfParser, SectionContainsAddress) {
    Section sec;
    sec.vaddr = 0x80010000;
    sec.size = 0x100;

    EXPECT_TRUE(sec.containsAddress(0x80010000));
    EXPECT_TRUE(sec.containsAddress(0x800100FF));
    EXPECT_FALSE(sec.containsAddress(0x80010100));
    EXPECT_FALSE(sec.containsAddress(0x8000FFFF));
}

// Symbol Extraction Tests

TEST(ElfParser, ExtractSymbols) {
    const std::string path = "/tmp/ps1recomp_test_symbols.elf";
    createMinimalPS1Elf(path, 0x80010000, true);

    ElfParser parser;
    ASSERT_TRUE(parser.load(path));

    // Should have loaded symbols
    EXPECT_GE(parser.getSymbols().size(), 3u);

    // Check function symbols
    auto funcs = parser.getFunctionSymbols();
    EXPECT_GE(funcs.size(), 2u); // main, game_loop, GsInitGraph

    // Verify we can find specific symbols
    bool found_main = false;
    bool found_game_loop = false;
    for (const auto& sym : parser.getSymbols()) {
        if (sym.name == "main") {
            found_main = true;
            EXPECT_EQ(sym.address, 0x80010000u);
            EXPECT_EQ(sym.size, 32u);
            EXPECT_TRUE(sym.isFunction());
        }
        if (sym.name == "game_loop") {
            found_game_loop = true;
            EXPECT_EQ(sym.address, 0x80010020u);
            EXPECT_TRUE(sym.isFunction());
        }
    }
    EXPECT_TRUE(found_main);
    EXPECT_TRUE(found_game_loop);

    cleanupFile(path);
}

TEST(ElfParser, NoSymbolsGraceful) {
    const std::string path = "/tmp/ps1recomp_test_nosyms.elf";
    createMinimalPS1Elf(path, 0x80010000, false);

    ElfParser parser;
    ASSERT_TRUE(parser.load(path));

    // No symbols, but should still load fine
    EXPECT_EQ(parser.getSymbols().size(), 0u);
    EXPECT_EQ(parser.getFunctionSymbols().size(), 0u);

    cleanupFile(path);
}

// Size Calculation Tests

TEST(ElfParser, TotalCodeSize) {
    const std::string path = "/tmp/ps1recomp_test_codesize.elf";
    createMinimalPS1Elf(path);

    ElfParser parser;
    ASSERT_TRUE(parser.load(path));

    EXPECT_EQ(parser.getTotalCodeSize(), 64u); // .text = 64 bytes

    cleanupFile(path);
}

TEST(ElfParser, TotalDataSize) {
    const std::string path = "/tmp/ps1recomp_test_datasize.elf";
    createMinimalPS1Elf(path);

    ElfParser parser;
    ASSERT_TRUE(parser.load(path));

    // .data(8) + .bss(256) + .rodata(4) = 268
    EXPECT_EQ(parser.getTotalDataSize(), 268u);

    cleanupFile(path);
}

// Static Helper Tests

TEST(ElfParser, VAddrToPhysical) {
    // KUSEG
    auto phys = ElfParser::vAddrToPhysical(0x00010000);
    ASSERT_TRUE(phys.has_value());
    EXPECT_EQ(*phys, 0x00010000u);

    // KSEG0
    phys = ElfParser::vAddrToPhysical(0x80010000);
    ASSERT_TRUE(phys.has_value());
    EXPECT_EQ(*phys, 0x00010000u);

    // KSEG1
    phys = ElfParser::vAddrToPhysical(0xA0010000);
    ASSERT_TRUE(phys.has_value());
    EXPECT_EQ(*phys, 0x00010000u);

    // Out of range
    phys = ElfParser::vAddrToPhysical(0xBFC00000);
    EXPECT_FALSE(phys.has_value());

    // Out of RAM (within KSEG0 but > 2MB)
    phys = ElfParser::vAddrToPhysical(0x80300000);
    EXPECT_FALSE(phys.has_value());
}

TEST(ElfParser, IsInRAM) {
    EXPECT_TRUE(ElfParser::isInRAM(0x80010000));
    EXPECT_TRUE(ElfParser::isInRAM(0x00100000));
    EXPECT_TRUE(ElfParser::isInRAM(0xA0100000));
    EXPECT_FALSE(ElfParser::isInRAM(0xBFC00000));
    EXPECT_FALSE(ElfParser::isInRAM(0x80300000));
}

TEST(ElfParser, IsLikelyPsyQ) {
    EXPECT_TRUE(ElfParser::isLikelyPsyQ(0x800B5000));
    EXPECT_TRUE(ElfParser::isLikelyPsyQ(0x800A0000));
    EXPECT_FALSE(ElfParser::isLikelyPsyQ(0x80050000));
    EXPECT_FALSE(ElfParser::isLikelyPsyQ(0x80010000));
}

// PS1 Memory Map Constants

TEST(PS1Constants, RAMSize) {
    EXPECT_EQ(PS1_RAM_SIZE, 2u * 1024 * 1024);
    EXPECT_EQ(PS1_RAM_MASK, 0x1FFFFFu);
}

TEST(PS1Constants, SegmentRanges) {
    EXPECT_EQ(PS1_KSEG0_END - PS1_KSEG0_START + 1, PS1_RAM_SIZE);
    EXPECT_EQ(PS1_KSEG1_END - PS1_KSEG1_START + 1, PS1_RAM_SIZE);
    EXPECT_EQ(PS1_KUSEG_END - PS1_KUSEG_START + 1, PS1_RAM_SIZE);
}

TEST(PS1Constants, Scratchpad) {
    EXPECT_EQ(PS1_SCRATCHPAD_SIZE, 1024u);
}

TEST(PS1Constants, IOBase) {
    EXPECT_EQ(PS1_IO_BASE, 0x1F801000u);
    EXPECT_EQ(PS1_BIOS_START, 0xBFC00000u);
}

// Unique temp path per test (parallel ctest runs share /tmp).
static std::string uniqueTempPath(const char* ext) {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    return (std::filesystem::temp_directory_path() /
            (std::string("ps1recomp_elf_") + info->name() + "_" +
             std::to_string(getpid()) + ext)).string();
}

// PS-X EXE (psx-spx "CDROM File Playstation EXE and SYSTEM.CNF"):
// 800h-byte header, pc0 at 10h, t_addr at 18h, t_size at 1Ch, then payload.
static std::vector<uint8_t> makePsxExeFile(uint32_t pc, uint32_t tAddr,
                                           uint32_t tSize, size_t payload) {
    std::vector<uint8_t> f(0x800 + payload, 0);
    std::memcpy(f.data(), "PS-X EXE", 8);
    std::memcpy(f.data() + 0x10, &pc, 4);
    std::memcpy(f.data() + 0x18, &tAddr, 4);
    std::memcpy(f.data() + 0x1C, &tSize, 4);
    for (size_t i = 0; i < payload; ++i)
        f[0x800 + i] = static_cast<uint8_t>(i + 1);
    return f;
}

static void writeBytes(const std::string& path, const std::vector<uint8_t>& b) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(b.data()),
              static_cast<std::streamsize>(b.size()));
}

TEST(ElfParser, LoadsPsxExeAsSingleTextSection) {
    const std::string path = uniqueTempPath(".exe");
    writeBytes(path, makePsxExeFile(0x80010008, 0x80010000, 0x100, 0x100));

    ElfParser parser;
    ASSERT_TRUE(parser.load(path)) << parser.getError();
    EXPECT_EQ(parser.getEntryPoint(), 0x80010008u);
    ASSERT_EQ(parser.getSections().size(), 1u);
    const Section* text = parser.getTextSection();
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->vaddr, 0x80010000u);
    EXPECT_EQ(text->size, 0x100u);
    EXPECT_EQ(text->type, SectionType::Text);
    ASSERT_NE(text->data, nullptr);
    EXPECT_EQ(text->data[0], 1);      // payload starts right after the header
    EXPECT_EQ(text->data[0xFF], 0x00); // (0xFF + 1) & 0xFF
    EXPECT_TRUE(parser.getSymbols().empty());
    EXPECT_EQ(parser.getTotalCodeSize(), 0x100u);

    cleanupFile(path);
}

TEST(ElfParser, PsxExeSizeLargerThanFileIsClampedToPayload) {
    const std::string path = uniqueTempPath(".exe");
    writeBytes(path, makePsxExeFile(0x80010000, 0x80010000, 0x10000, 0x40));
    ElfParser parser;
    ASSERT_TRUE(parser.load(path));
    EXPECT_EQ(parser.getTextSection()->size, 0x40u);
    cleanupFile(path);
}

TEST(ElfParser, PsxExeZeroSizeUsesWholePayload) {
    const std::string path = uniqueTempPath(".exe");
    writeBytes(path, makePsxExeFile(0x80010000, 0x80010000, 0, 0x80));
    ElfParser parser;
    ASSERT_TRUE(parser.load(path));
    EXPECT_EQ(parser.getTextSection()->size, 0x80u);
    cleanupFile(path);
}

TEST(ElfParser, PsxExeEntryOutsideRamIsRejected) {
    const std::string path = uniqueTempPath(".exe");
    writeBytes(path, makePsxExeFile(0xBFC00000, 0x80010000, 0x40, 0x40)); // BIOS
    ElfParser parser;
    EXPECT_FALSE(parser.load(path));
    EXPECT_FALSE(parser.isLoaded());
    EXPECT_NE(parser.getError().find("PS-EXE entry point 0xBFC00000"),
              std::string::npos) << parser.getError();
    cleanupFile(path);
}

// ELF header validation: PS1 is 32-bit little-endian MIPS.

static void writeHeaderOnlyElf(const std::string& path, unsigned char cls,
                               unsigned char enc, ELFIO::Elf_Half machine) {
    ELFIO::elfio writer;
    writer.create(cls, enc);
    writer.set_type(ELFIO::ET_EXEC);
    writer.set_machine(machine);
    writer.set_entry(0x80010000);
    auto* text = writer.sections.add(".text");
    text->set_type(ELFIO::SHT_PROGBITS);
    text->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_EXECINSTR);
    const char code[8] = {};
    text->set_data(code, sizeof(code));
    writer.save(path);
}

TEST(ElfParser, RejectsNonMipsMachine) {
    const std::string path = uniqueTempPath(".elf");
    writeHeaderOnlyElf(path, ELFIO::ELFCLASS32, ELFIO::ELFDATA2LSB, ELFIO::EM_386);
    ElfParser parser;
    EXPECT_FALSE(parser.load(path));
    EXPECT_NE(parser.getError().find("Not a MIPS ELF"), std::string::npos);
    cleanupFile(path);
}

TEST(ElfParser, Rejects64BitElf) {
    const std::string path = uniqueTempPath(".elf");
    writeHeaderOnlyElf(path, ELFIO::ELFCLASS64, ELFIO::ELFDATA2LSB, ELFIO::EM_MIPS);
    ElfParser parser;
    EXPECT_FALSE(parser.load(path));
    EXPECT_EQ(parser.getError(), "Not a 32-bit ELF");
    cleanupFile(path);
}

TEST(ElfParser, RejectsBigEndianElf) {
    const std::string path = uniqueTempPath(".elf");
    writeHeaderOnlyElf(path, ELFIO::ELFCLASS32, ELFIO::ELFDATA2MSB, ELFIO::EM_MIPS);
    ElfParser parser;
    EXPECT_FALSE(parser.load(path));
    EXPECT_NE(parser.getError().find("little-endian"), std::string::npos);
    cleanupFile(path);
}

// Section classification falls back on the section name, then on flags.

TEST(ElfParser, ClassifiesSectionsByNameThenFlags) {
    const std::string path = uniqueTempPath(".elf");
    ELFIO::elfio writer;
    writer.create(ELFIO::ELFCLASS32, ELFIO::ELFDATA2LSB);
    writer.set_type(ELFIO::ET_EXEC);
    writer.set_machine(ELFIO::EM_MIPS);
    writer.set_entry(0x80010000);
    const char bytes[4] = {1, 2, 3, 4};
    auto add = [&](const char* name, ELFIO::Elf_Xword flags) {
        auto* s = writer.sections.add(name);
        s->set_type(ELFIO::SHT_PROGBITS);
        s->set_flags(flags);
        s->set_data(bytes, sizeof(bytes));
    };
    add(".init", 0);                                  // Text by name
    add(".sbss", 0);                                  // Bss by name (PROGBITS)
    add(".lit4", 0);                                  // Data by name
    add(".mydata", ELFIO::SHF_ALLOC | ELFIO::SHF_WRITE); // Data by flags
    add(".myconst", ELFIO::SHF_ALLOC);                // Other: read-only alloc
    add(".comment", 0);                               // Other
    writer.save(path);

    ElfParser parser;
    ASSERT_TRUE(parser.load(path)) << parser.getError();
    auto type = [&](const char* n) {
        const Section* s = parser.findSection(n);
        EXPECT_NE(s, nullptr) << n;
        return s ? s->type : SectionType::Other;
    };
    EXPECT_EQ(type(".init"), SectionType::Text);
    EXPECT_EQ(type(".sbss"), SectionType::Bss);
    EXPECT_EQ(type(".lit4"), SectionType::Data);
    EXPECT_EQ(type(".mydata"), SectionType::Data);
    EXPECT_EQ(type(".myconst"), SectionType::Other);
    EXPECT_EQ(type(".comment"), SectionType::Other);
    EXPECT_EQ(parser.findSection(".nope"), nullptr);
    cleanupFile(path);
}

TEST(ElfParser, SymbolTypesMapFromElfSymbolType) {
    const std::string path = uniqueTempPath(".elf");
    ELFIO::elfio writer;
    writer.create(ELFIO::ELFCLASS32, ELFIO::ELFDATA2LSB);
    writer.set_type(ELFIO::ET_EXEC);
    writer.set_machine(ELFIO::EM_MIPS);
    writer.set_entry(0x80010000);
    auto* text = writer.sections.add(".text");
    text->set_type(ELFIO::SHT_PROGBITS);
    text->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_EXECINSTR);
    text->set_address(0x80010000);
    const char code[16] = {};
    text->set_data(code, sizeof(code));
    auto* strtab = writer.sections.add(".strtab");
    strtab->set_type(ELFIO::SHT_STRTAB);
    ELFIO::string_section_accessor stra(strtab);
    auto* symtab = writer.sections.add(".symtab");
    symtab->set_type(ELFIO::SHT_SYMTAB);
    symtab->set_entry_size(writer.get_default_entry_size(ELFIO::SHT_SYMTAB));
    symtab->set_link(strtab->get_index());
    ELFIO::symbol_section_accessor syma(writer, symtab);
    syma.add_symbol(stra, "label", 0x80010004, 0, ELFIO::STB_LOCAL,
                    ELFIO::STT_NOTYPE, 0, text->get_index());
    syma.add_symbol(stra, "sect", 0x80010000, 0, ELFIO::STB_LOCAL,
                    ELFIO::STT_SECTION, 0, text->get_index());
    writer.save(path);

    ElfParser parser;
    ASSERT_TRUE(parser.load(path)) << parser.getError();
    bool sawLabel = false, sawSect = false;
    for (const auto& s : parser.getSymbols()) {
        if (s.name == "label") { sawLabel = true; EXPECT_EQ(s.type, SymbolType::NoType); }
        if (s.name == "sect")  { sawSect = true;  EXPECT_EQ(s.type, SymbolType::Other); }
    }
    EXPECT_TRUE(sawLabel);
    EXPECT_TRUE(sawSect);
    EXPECT_TRUE(parser.getFunctionSymbols().empty());
    cleanupFile(path);
}
