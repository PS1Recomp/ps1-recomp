// Tests for ps1Analyzer -- PsyQ Signature Matcher
// Validates database, name matching, prefix classification, and stub assignment

#include <gtest/gtest.h>
#include <ps1recomp/psyq_signatures.h>
#include <ps1recomp/elf_parser.h>
#include <ps1recomp/function_finder.h>
#include <elfio/elfio.hpp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

using namespace ps1recomp;

// Helper: Create ELF with PsyQ-like symbols

static std::string createPsyQElf(
    const std::string& path,
    const std::vector<std::pair<std::string, uint32_t>>& funcs) {

    ELFIO::elfio writer;
    writer.create(ELFIO::ELFCLASS32, ELFIO::ELFDATA2LSB);
    writer.set_os_abi(ELFIO::ELFOSABI_NONE);
    writer.set_type(ELFIO::ET_EXEC);
    writer.set_machine(ELFIO::EM_MIPS);
    writer.set_entry(0x80010000);

    // .text section
    auto* text_sec = writer.sections.add(".text");
    text_sec->set_type(ELFIO::SHT_PROGBITS);
    text_sec->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_EXECINSTR);
    text_sec->set_addr_align(4);
    text_sec->set_address(0x80010000);

    // Fill with NOPs (enough space for all functions)
    std::vector<uint8_t> text_data(0x100000, 0x00);
    text_sec->set_data(reinterpret_cast<const char*>(text_data.data()), text_data.size());

    // Add symbol table
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
    for (const auto& [name, addr] : funcs) {
        syma.add_symbol(stra, name.c_str(), addr, 64,
                        ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0,
                        text_sec->get_index());
    }

    auto* segment = writer.segments.add();
    segment->set_type(ELFIO::PT_LOAD);
    segment->set_virtual_address(0x80010000);
    segment->set_physical_address(0x80010000);
    segment->set_flags(ELFIO::PF_X | ELFIO::PF_R);
    segment->set_align(0x1000);
    segment->add_section_index(text_sec->get_index(), text_sec->get_addr_align());

    writer.save(path);
    return path;
}

static void cleanupFile(const std::string& path) {
    std::remove(path.c_str());
}

// Database Tests

TEST(PsyQMatcher, DatabaseNotEmpty) {
    PsyQMatcher matcher;
    EXPECT_GT(matcher.getDatabaseSize(), 70u);
}

TEST(PsyQMatcher, KnownFunctions) {
    PsyQMatcher matcher;
    EXPECT_TRUE(matcher.isKnown("GsInitGraph"));
    EXPECT_TRUE(matcher.isKnown("SpuInit"));
    EXPECT_TRUE(matcher.isKnown("CdInit"));
    EXPECT_TRUE(matcher.isKnown("PadInit"));
    EXPECT_TRUE(matcher.isKnown("VSync"));
    EXPECT_TRUE(matcher.isKnown("malloc"));
    EXPECT_TRUE(matcher.isKnown("memcpy"));
    EXPECT_TRUE(matcher.isKnown("printf"));
    EXPECT_TRUE(matcher.isKnown("RotTransPers"));
    EXPECT_TRUE(matcher.isKnown("rsin"));
    EXPECT_FALSE(matcher.isKnown("game_update"));
    EXPECT_FALSE(matcher.isKnown("player_move"));
}

// Subsystem Classification Tests

TEST(PsyQMatcher, ClassifyGraphics) {
    EXPECT_EQ(PsyQMatcher::classifySubsystem("GsInitGraph"), PsyQSubsystem::Graphics);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("DrawOTag"), PsyQSubsystem::Graphics);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("PutDrawEnv"), PsyQSubsystem::Graphics);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("FntPrint"), PsyQSubsystem::Graphics);
}

TEST(PsyQMatcher, ClassifySound) {
    EXPECT_EQ(PsyQMatcher::classifySubsystem("SpuInit"), PsyQSubsystem::Sound);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("SsInit"), PsyQSubsystem::Sound);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("SsVabOpenHead"), PsyQSubsystem::Sound);
}

TEST(PsyQMatcher, ClassifyCDROM) {
    EXPECT_EQ(PsyQMatcher::classifySubsystem("CdInit"), PsyQSubsystem::CDROM);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("CdRead"), PsyQSubsystem::CDROM);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("CdSearchFile"), PsyQSubsystem::CDROM);
}

TEST(PsyQMatcher, ClassifyController) {
    EXPECT_EQ(PsyQMatcher::classifySubsystem("PadInit"), PsyQSubsystem::Controller);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("PadRead"), PsyQSubsystem::Controller);
}

TEST(PsyQMatcher, ClassifyGTE) {
    EXPECT_EQ(PsyQMatcher::classifySubsystem("gte_ldv0"), PsyQSubsystem::GTE);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("RotTransPers"), PsyQSubsystem::GTE);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("TransMatrix"), PsyQSubsystem::GTE);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("NormalClip"), PsyQSubsystem::GTE);
}

TEST(PsyQMatcher, ClassifyVSync) {
    EXPECT_EQ(PsyQMatcher::classifySubsystem("VSync"), PsyQSubsystem::VSync);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("DrawSync"), PsyQSubsystem::VSync);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("ResetCallback"), PsyQSubsystem::VSync);
}

TEST(PsyQMatcher, ClassifyMemory) {
    EXPECT_EQ(PsyQMatcher::classifySubsystem("malloc"), PsyQSubsystem::Memory);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("free"), PsyQSubsystem::Memory);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("InitHeap"), PsyQSubsystem::Memory);
}

TEST(PsyQMatcher, ClassifyLibC) {
    EXPECT_EQ(PsyQMatcher::classifySubsystem("printf"), PsyQSubsystem::LibC);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("memcpy"), PsyQSubsystem::LibC);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("strlen"), PsyQSubsystem::LibC);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("rand"), PsyQSubsystem::LibC);
}

TEST(PsyQMatcher, ClassifyMath) {
    EXPECT_EQ(PsyQMatcher::classifySubsystem("rsin"), PsyQSubsystem::Math);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("rcos"), PsyQSubsystem::Math);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("ratan2"), PsyQSubsystem::Math);
}

TEST(PsyQMatcher, ClassifyOther) {
    EXPECT_EQ(PsyQMatcher::classifySubsystem("game_loop"), PsyQSubsystem::Other);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("update_physics"), PsyQSubsystem::Other);
    EXPECT_EQ(PsyQMatcher::classifySubsystem(""), PsyQSubsystem::Other);
}

// Name Matching Tests

TEST(PsyQMatcher, MatchGraphicsFunctions) {
    const std::string path = "/tmp/ps1recomp_test_psyq_gfx.elf";

    createPsyQElf(path, {
        {"main",         0x80010000},
        {"GsInitGraph",  0x800B0000},
        {"GsDrawOt",     0x800B0100},
        {"DrawSync",     0x800B0200},
        {"game_update",  0x80020000}
    });

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    PsyQMatcher matcher;
    matcher.matchFunctions(elf, finder);

    // Should match GsInitGraph, GsDrawOt, DrawSync
    EXPECT_GE(matcher.getMatchCount(), 3u);

    // Stubs should include GPU functions
    auto stubs = matcher.getStubs();
    EXPECT_GE(stubs.size(), 2u); // GsInitGraph, GsDrawOt are stubs

    cleanupFile(path);
}

TEST(PsyQMatcher, MatchSoundFunctions) {
    const std::string path = "/tmp/ps1recomp_test_psyq_snd.elf";

    createPsyQElf(path, {
        {"main",     0x80010000},
        {"SpuInit",  0x800C0000},
        {"SpuSetKey",0x800C0100}
    });

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    PsyQMatcher matcher;
    matcher.matchFunctions(elf, finder);

    EXPECT_GE(matcher.getMatchCount(), 2u);

    // Both are stubs
    auto stubs = matcher.getStubs();
    bool found_spu_init = false;
    for (const auto* s : stubs) {
        if (s->name == "SpuInit") found_spu_init = true;
    }
    EXPECT_TRUE(found_spu_init);

    cleanupFile(path);
}

TEST(PsyQMatcher, MatchPassthroughs) {
    const std::string path = "/tmp/ps1recomp_test_psyq_pass.elf";

    createPsyQElf(path, {
        {"main",   0x80010000},
        {"memcpy", 0x800D0000},
        {"malloc", 0x800D0100},
        {"printf", 0x800D0200}
    });

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    PsyQMatcher matcher;
    matcher.matchFunctions(elf, finder);

    auto passthroughs = matcher.getPassthroughs();
    EXPECT_GE(passthroughs.size(), 2u); // memcpy, malloc

    auto skips = matcher.getSkips();
    EXPECT_GE(skips.size(), 1u); // printf

    cleanupFile(path);
}

TEST(PsyQMatcher, NoMatchForGameFunctions) {
    const std::string path = "/tmp/ps1recomp_test_psyq_game.elf";

    createPsyQElf(path, {
        {"main",           0x80010000},
        {"game_init",      0x80020000},
        {"player_update",  0x80030000},
        {"render_scene",   0x80040000}
    });

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    PsyQMatcher matcher;
    matcher.matchFunctions(elf, finder);

    // None of these should match PsyQ
    EXPECT_EQ(matcher.getMatchCount(), 0u);

    cleanupFile(path);
}

// Pass 3: Byte Signature Detection Tests

// Helper: write a 32-bit LE word into a byte buffer at position
static void writeLE32(std::vector<uint8_t>& buf, size_t offset, uint32_t val) {
    buf[offset + 0] = (val >>  0) & 0xFF;
    buf[offset + 1] = (val >>  8) & 0xFF;
    buf[offset + 2] = (val >> 16) & 0xFF;
    buf[offset + 3] = (val >> 24) & 0xFF;
}

// Create an ELF whose .text section contains synthetic instruction bytes at
// specific offsets relative to 0x80010000.
static std::string createSyntheticELF(
        const std::string& path,
        // (name, address) pairs -- these are func_ style (no symbols)
        const std::vector<std::pair<std::string,uint32_t>>& funcs,
        // (address, instruction_word) pairs to inject
        const std::vector<std::pair<uint32_t,uint32_t>>& instrs) {

    ELFIO::elfio writer;
    writer.create(ELFIO::ELFCLASS32, ELFIO::ELFDATA2LSB);
    writer.set_os_abi(ELFIO::ELFOSABI_NONE);
    writer.set_type(ELFIO::ET_EXEC);
    writer.set_machine(ELFIO::EM_MIPS);
    writer.set_entry(0x80010000);

    constexpr uint32_t TEXT_BASE = 0x80010000;
    constexpr size_t   TEXT_SIZE = 0x100000; // 1MB

    auto* text_sec = writer.sections.add(".text");
    text_sec->set_type(ELFIO::SHT_PROGBITS);
    text_sec->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_EXECINSTR);
    text_sec->set_addr_align(4);
    text_sec->set_address(TEXT_BASE);

    std::vector<uint8_t> text_data(TEXT_SIZE, 0x00);
    for (const auto& [addr, word] : instrs) {
        if (addr >= TEXT_BASE && addr < TEXT_BASE + TEXT_SIZE) {
            writeLE32(text_data, addr - TEXT_BASE, word);
        }
    }
    text_sec->set_data(reinterpret_cast<const char*>(text_data.data()),
                       text_data.size());

    // Add function symbols (STT_FUNC) so FunctionFinder can find them
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
    for (const auto& [name, addr] : funcs) {
        syma.add_symbol(stra, name.c_str(), addr, 128,
                        ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0,
                        text_sec->get_index());
    }

    auto* seg = writer.segments.add();
    seg->set_type(ELFIO::PT_LOAD);
    seg->set_virtual_address(TEXT_BASE);
    seg->set_physical_address(TEXT_BASE);
    seg->set_flags(ELFIO::PF_X | ELFIO::PF_R);
    seg->set_align(0x1000);
    seg->add_section_index(text_sec->get_index(), text_sec->get_addr_align());

    writer.save(path);
    return path;
}

// Note: the prior Pass3 byte-pattern detection tests
// (Pass3DetectsSpuInitBySPURegWrite, Pass3DetectsCdInitByBIOSCallIndex)
// were removed in Sessao 0.4 -- the InstrPattern/ByteSig matcher is gone.
// Equivalent coverage now lives in test_psyq_hash_matching.cpp, which uses
// real SHA-256 hashes from psyq_signatures.toml.

TEST(PsyQMatcher, Pass3DoesNotMatchUnrelatedFunction) {
    // A function with only NOPs -- should NOT match any signature
    constexpr uint32_t FUNC_ADDR = 0x80070000;

    createSyntheticELF("/tmp/ps1recomp_test_psyq_p3_nop.elf",
        {{"func_80070000", FUNC_ADDR}},
        {} /* no injected instructions -- all NOPs */);

    ElfParser elf;
    ASSERT_TRUE(elf.load("/tmp/ps1recomp_test_psyq_p3_nop.elf"));

    FunctionFinder finder;
    finder.findFunctions(elf);

    PsyQMatcher matcher;
    matcher.matchFunctions(elf, finder);

    // NOP-only function should not trigger any signature
    EXPECT_EQ(matcher.getMatchCount(), 0u)
        << "NOP-only function should not match any PsyQ signature";

    std::remove("/tmp/ps1recomp_test_psyq_p3_nop.elf");
}

// String Helper Tests

TEST(PsyQMatcher, SubsystemNames) {
    EXPECT_STREQ(PsyQMatcher::subsystemName(PsyQSubsystem::Graphics), "Graphics");
    EXPECT_STREQ(PsyQMatcher::subsystemName(PsyQSubsystem::Sound), "Sound");
    EXPECT_STREQ(PsyQMatcher::subsystemName(PsyQSubsystem::CDROM), "CD-ROM");
    EXPECT_STREQ(PsyQMatcher::subsystemName(PsyQSubsystem::Controller), "Controller");
    EXPECT_STREQ(PsyQMatcher::subsystemName(PsyQSubsystem::GTE), "GTE");
    EXPECT_STREQ(PsyQMatcher::subsystemName(PsyQSubsystem::VSync), "VSync");
    EXPECT_STREQ(PsyQMatcher::subsystemName(PsyQSubsystem::Memory), "Memory");
    EXPECT_STREQ(PsyQMatcher::subsystemName(PsyQSubsystem::LibC), "LibC");
    EXPECT_STREQ(PsyQMatcher::subsystemName(PsyQSubsystem::Math), "Math");
    EXPECT_STREQ(PsyQMatcher::subsystemName(PsyQSubsystem::Other), "Other");
}

TEST(PsyQMatcher, StubTypeNames) {
    EXPECT_STREQ(PsyQMatcher::stubTypeName(StubType::Stub), "stub");
    EXPECT_STREQ(PsyQMatcher::stubTypeName(StubType::Skip), "skip");
    EXPECT_STREQ(PsyQMatcher::stubTypeName(StubType::Passthrough), "passthrough");
    EXPECT_STREQ(PsyQMatcher::stubTypeName(StubType::Recompile), "recompile");
}

// Subsystem classification table: one row per PsyQ name the classifier
// recognises, grouped by the SDK library that ships it (libgpu/libgs ->
// Graphics, libgte -> GTE, libspu/libsnd -> Sound, libcd -> CDROM,
// libpad/libgun -> Controller, libetc sync/callbacks -> VSync, libapi heap
// -> Memory, libc -> LibC, libgte fixed-point trig -> Math).

struct ClassifyCase {
    const char* name;
    PsyQSubsystem expected;
};

class PsyQClassifyTable : public ::testing::TestWithParam<ClassifyCase> {};

TEST_P(PsyQClassifyTable, NameMapsToItsLibrarySubsystem) {
    const auto& c = GetParam();
    EXPECT_EQ(PsyQMatcher::classifySubsystem(c.name), c.expected) << c.name;
}

INSTANTIATE_TEST_SUITE_P(
    AllRecognisedNames, PsyQClassifyTable,
    ::testing::Values(
        ClassifyCase{"VSync", PsyQSubsystem::VSync},
        ClassifyCase{"VSyncCallback", PsyQSubsystem::VSync},
        ClassifyCase{"DrawSync", PsyQSubsystem::VSync},
        ClassifyCase{"ResetCallback", PsyQSubsystem::VSync},
        ClassifyCase{"StopCallback", PsyQSubsystem::VSync},
        ClassifyCase{"RestartCallback", PsyQSubsystem::VSync},
        ClassifyCase{"ChangeClearPAD", PsyQSubsystem::VSync},
        ClassifyCase{"malloc", PsyQSubsystem::Memory},
        ClassifyCase{"free", PsyQSubsystem::Memory},
        ClassifyCase{"calloc", PsyQSubsystem::Memory},
        ClassifyCase{"realloc", PsyQSubsystem::Memory},
        ClassifyCase{"malloc3", PsyQSubsystem::Memory},
        ClassifyCase{"InitHeap", PsyQSubsystem::Memory},
        ClassifyCase{"printf", PsyQSubsystem::LibC},
        ClassifyCase{"sprintf", PsyQSubsystem::LibC},
        ClassifyCase{"memset", PsyQSubsystem::LibC},
        ClassifyCase{"strcmp", PsyQSubsystem::LibC},
        ClassifyCase{"rand", PsyQSubsystem::LibC},
        ClassifyCase{"srand", PsyQSubsystem::LibC},
        ClassifyCase{"abs", PsyQSubsystem::LibC},
        ClassifyCase{"labs", PsyQSubsystem::LibC},
        ClassifyCase{"atoi", PsyQSubsystem::LibC},
        ClassifyCase{"atol", PsyQSubsystem::LibC},
        ClassifyCase{"rsin", PsyQSubsystem::Math},
        ClassifyCase{"rcos", PsyQSubsystem::Math},
        ClassifyCase{"ratan2", PsyQSubsystem::Math},
        ClassifyCase{"SquareRoot0", PsyQSubsystem::Math},
        ClassifyCase{"SquareRoot12", PsyQSubsystem::Math},
        ClassifyCase{"SpuSetVoiceAttr", PsyQSubsystem::Sound},
        ClassifyCase{"SPU_Init", PsyQSubsystem::Sound},
        ClassifyCase{"SsSeqOpen", PsyQSubsystem::Sound},
        ClassifyCase{"SND_Play", PsyQSubsystem::Sound},
        ClassifyCase{"CdRead", PsyQSubsystem::CDROM},
        ClassifyCase{"PadRead", PsyQSubsystem::Controller},
        ClassifyCase{"GunInit", PsyQSubsystem::Controller},
        ClassifyCase{"GsSortOt", PsyQSubsystem::Graphics},
        ClassifyCase{"GPU_cw", PsyQSubsystem::Graphics},
        ClassifyCase{"FntPrint", PsyQSubsystem::Graphics},
        ClassifyCase{"PutDispEnv", PsyQSubsystem::Graphics},
        ClassifyCase{"ResetGraph", PsyQSubsystem::Graphics},
        ClassifyCase{"DrawPrim", PsyQSubsystem::Graphics},
        ClassifyCase{"DrawOTag", PsyQSubsystem::Graphics},
        ClassifyCase{"DrawOTagEnv", PsyQSubsystem::Graphics},
        ClassifyCase{"ClearOTag", PsyQSubsystem::Graphics},
        ClassifyCase{"ClearOTagR", PsyQSubsystem::Graphics},
        ClassifyCase{"LoadImage", PsyQSubsystem::Graphics},
        ClassifyCase{"StoreImage", PsyQSubsystem::Graphics},
        ClassifyCase{"MoveImage", PsyQSubsystem::Graphics},
        ClassifyCase{"LoadTPage", PsyQSubsystem::Graphics},
        ClassifyCase{"LoadClut", PsyQSubsystem::Graphics},
        ClassifyCase{"GetTPage", PsyQSubsystem::Graphics},
        ClassifyCase{"GetClut", PsyQSubsystem::Graphics},
        ClassifyCase{"SetDispMask", PsyQSubsystem::Graphics},
        ClassifyCase{"SetDefDrawEnv", PsyQSubsystem::Graphics},
        ClassifyCase{"SetDefDispEnv", PsyQSubsystem::Graphics},
        ClassifyCase{"gte_rtps", PsyQSubsystem::GTE},
        ClassifyCase{"RotTransPers", PsyQSubsystem::GTE},
        ClassifyCase{"TransMatrix", PsyQSubsystem::GTE},
        ClassifyCase{"MulMatrix", PsyQSubsystem::GTE},
        ClassifyCase{"CompMatrix", PsyQSubsystem::GTE},
        ClassifyCase{"ApplyMatrix", PsyQSubsystem::GTE},
        ClassifyCase{"InitGeom", PsyQSubsystem::GTE},
        ClassifyCase{"NormalClip", PsyQSubsystem::GTE},
        ClassifyCase{"AverageZ3", PsyQSubsystem::GTE},
        ClassifyCase{"OuterProduct0", PsyQSubsystem::GTE},
        ClassifyCase{"SetGeomOffset", PsyQSubsystem::GTE},
        ClassifyCase{"SetGeomScreen", PsyQSubsystem::GTE},
        ClassifyCase{"SetRotMatrix", PsyQSubsystem::GTE},
        ClassifyCase{"SetTransMatrix", PsyQSubsystem::GTE},
        ClassifyCase{"SetLightMatrix", PsyQSubsystem::GTE},
        ClassifyCase{"SetColorMatrix", PsyQSubsystem::GTE},
        ClassifyCase{"SetBackColor", PsyQSubsystem::GTE},
        ClassifyCase{"SetFarColor", PsyQSubsystem::GTE},
        ClassifyCase{"DMACallback", PsyQSubsystem::VSync},
        ClassifyCase{"MySync", PsyQSubsystem::VSync},
        ClassifyCase{"game_main", PsyQSubsystem::Other},
        ClassifyCase{"", PsyQSubsystem::Other}));

// Matcher fed from a private data directory.
//
// The constructor always loads the default database. PS1RECOMP_DATA_DIR is
// searched first, so pointing it at a per-test directory gives the matcher a
// database made only of the rows the test wrote -- independent of the
// 2000-row in-tree TOML.

namespace {

constexpr uint32_t kSynthBase = 0x80010000;

std::string uniqueDir() {
    static int counter = 0;
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string name = std::string(info->test_suite_name()) + "_" + info->name();
    for (auto& ch : name)
        if (ch == '/') ch = '_';
    auto dir = std::filesystem::temp_directory_path() /
               ("ps1_psyq_" + name + "_" + std::to_string(getpid()) + "_" +
                std::to_string(counter++));
    std::filesystem::create_directories(dir);
    return dir.string();
}

void writeFile(const std::string& path, const std::string& text) {
    std::ofstream(path) << text;
}

// Owns the private data dir and restores PS1RECOMP_DATA_DIR on scope exit.
class DataDir {
public:
    DataDir(const std::string& sigs, const std::string& meta) : dir_(uniqueDir()) {
        writeFile(dir_ + "/psyq_signatures.toml", sigs);
        writeFile(dir_ + "/psyq_metadata.toml", meta);
        if (const char* old = std::getenv("PS1RECOMP_DATA_DIR")) old_ = old;
        setenv("PS1RECOMP_DATA_DIR", dir_.c_str(), 1);
    }
    ~DataDir() {
        if (old_) setenv("PS1RECOMP_DATA_DIR", old_->c_str(), 1);
        else unsetenv("PS1RECOMP_DATA_DIR");
        std::filesystem::remove_all(dir_);
    }
    std::string path(const std::string& file) const { return dir_ + "/" + file; }

private:
    std::string dir_;
    std::optional<std::string> old_;
};

std::string hex16(uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

std::vector<uint8_t> bytesOfWords(const std::vector<uint32_t>& words) {
    std::vector<uint8_t> b(words.size() * 4);
    for (size_t i = 0; i < words.size(); ++i)
        for (int k = 0; k < 4; ++k)
            b[i * 4 + k] = static_cast<uint8_t>(words[i] >> (8 * k));
    return b;
}

std::string sigRow(const std::string& name, const std::string& lib, size_t size,
                   const std::string& extra) {
    std::string r = "[[signature]]\nname = \"" + name + "\"\nsize = " +
                    std::to_string(size) + "\n";
    if (!lib.empty()) r += "library = \"" + lib + "\"\n";
    return r + extra + "\n";
}

struct Sym {
    std::string name;
    uint32_t addr;
};

// ELF whose .text at `base` holds `words`, with one STT_FUNC per symbol.
void writeElf(const std::string& path, uint32_t base,
              const std::vector<uint32_t>& words, const std::vector<Sym>& syms) {
    ELFIO::elfio w;
    w.create(ELFIO::ELFCLASS32, ELFIO::ELFDATA2LSB);
    w.set_type(ELFIO::ET_EXEC);
    w.set_machine(ELFIO::EM_MIPS);
    // Entry outside .text, so no "__start" shadows the symbol under test.
    w.set_entry(0x80000000u);
    auto* text = w.sections.add(".text");
    text->set_type(ELFIO::SHT_PROGBITS);
    text->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_EXECINSTR);
    text->set_addr_align(4);
    text->set_address(base);
    const auto bytes = bytesOfWords(words);
    text->set_data(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    auto* strtab = w.sections.add(".strtab");
    strtab->set_type(ELFIO::SHT_STRTAB);
    ELFIO::string_section_accessor stra(strtab);
    auto* symtab = w.sections.add(".symtab");
    symtab->set_type(ELFIO::SHT_SYMTAB);
    symtab->set_info(2);
    symtab->set_addr_align(4);
    symtab->set_entry_size(w.get_default_entry_size(ELFIO::SHT_SYMTAB));
    symtab->set_link(strtab->get_index());
    ELFIO::symbol_section_accessor syma(w, symtab);
    for (const auto& s : syms)
        syma.add_symbol(stra, s.name.c_str(), s.addr, 0, ELFIO::STB_GLOBAL,
                        ELFIO::STT_FUNC, 0, text->get_index());
    auto* seg = w.segments.add();
    seg->set_type(ELFIO::PT_LOAD);
    seg->set_virtual_address(base);
    seg->set_physical_address(base);
    seg->set_flags(ELFIO::PF_X | ELFIO::PF_R);
    seg->set_align(0x1000);
    seg->add_section_index(text->get_index(), text->get_addr_align());
    w.save(path);
}

// Runs all three passes over the ELF and returns the match for `addr`.
std::optional<PsyQMatch> matchAt(PsyQMatcher& m, const std::string& elfPath,
                                 uint32_t addr) {
    ElfParser elf;
    if (!elf.load(elfPath)) return std::nullopt;
    FunctionFinder finder;
    finder.findFunctions(elf);
    m.matchFunctions(elf, finder);
    for (const auto& x : m.getMatches())
        if (x.address == addr) return x;
    return std::nullopt;
}

// Four distinct, harmless words: addiu $v0,$zero,i ; ... ; jr $ra ; nop.
std::vector<uint32_t> body16(uint16_t tag) {
    return {0x24020000u | tag, 0x24030000u | tag, 0x03E00008u, 0x00000000u};
}

} // namespace

TEST(PsyQMatcherDataDir, EnvironmentDirectoryReplacesTheBundledDatabase) {
    DataDir dd(sigRow("OnlyInEnv", "libtest", 16, ""), "");

    PsyQMatcher matcher;

    EXPECT_TRUE(matcher.isKnown("OnlyInEnv"));
    EXPECT_FALSE(matcher.isKnown("ResetGraph"));
    EXPECT_EQ(matcher.getDatabaseSize(), 1u);
}

TEST(PsyQMatcherDataDir, MetadataSubsystemStringsMapToTheirEnum) {
    const std::pair<const char*, PsyQSubsystem> rows[] = {
        {"graphics", PsyQSubsystem::Graphics}, {"sound", PsyQSubsystem::Sound},
        {"cdrom", PsyQSubsystem::CDROM},       {"controller", PsyQSubsystem::Controller},
        {"gte", PsyQSubsystem::GTE},           {"vsync", PsyQSubsystem::VSync},
        {"memory", PsyQSubsystem::Memory},     {"libc", PsyQSubsystem::LibC},
        {"math", PsyQSubsystem::Math},         {"bogus", PsyQSubsystem::Other}};
    std::string meta;
    std::vector<Sym> syms;
    std::vector<uint32_t> words;
    uint32_t i = 0;
    for (const auto& [sub, _] : rows) {
        const std::string name = std::string("meta_") + sub;
        meta += "[[function]]\nname = \"" + name + "\"\nsubsystem = \"" + sub + "\"\n\n";
        syms.push_back({name, kSynthBase + i++ * 16});
        for (uint32_t w : body16(static_cast<uint16_t>(i))) words.push_back(w);
    }
    DataDir dd("", meta);
    const std::string elf = dd.path("meta.elf");
    writeElf(elf, kSynthBase, words, syms);
    PsyQMatcher matcher;

    std::vector<std::optional<PsyQMatch>> got;
    for (const auto& s : syms) got.push_back(matchAt(matcher, elf, s.addr));

    for (size_t k = 0; k < got.size(); ++k) {
        ASSERT_TRUE(got[k].has_value()) << rows[k].first;
        EXPECT_EQ(got[k]->subsystem, rows[k].second) << rows[k].first;
        EXPECT_TRUE(got[k]->exactMatch);
    }
}

TEST(PsyQMatcherDataDir, MetadataStubTypeStringsMapToTheirEnum) {
    const std::pair<const char*, StubType> rows[] = {
        {"stub", StubType::Stub},
        {"skip", StubType::Skip},
        {"passthrough", StubType::Passthrough},
        {"recompile", StubType::Recompile},
        {"bogus", StubType::Recompile}};
    std::string meta;
    std::vector<Sym> syms;
    std::vector<uint32_t> words;
    uint32_t i = 0;
    for (const auto& [st, _] : rows) {
        const std::string name = std::string("st_") + st;
        meta += "[[function]]\nname = \"" + name + "\"\nstub_type = \"" + st + "\"\n\n";
        syms.push_back({name, kSynthBase + i++ * 16});
        for (uint32_t w : body16(static_cast<uint16_t>(i))) words.push_back(w);
    }
    DataDir dd("", meta);
    const std::string elf = dd.path("stub.elf");
    writeElf(elf, kSynthBase, words, syms);
    PsyQMatcher matcher;

    std::vector<std::optional<PsyQMatch>> got;
    for (const auto& s : syms) got.push_back(matchAt(matcher, elf, s.addr));

    for (size_t k = 0; k < got.size(); ++k) {
        ASSERT_TRUE(got[k].has_value()) << rows[k].first;
        EXPECT_EQ(got[k]->stubType, rows[k].second) << rows[k].first;
        // Without a subsystem key the metadata row is unclassified.
        EXPECT_EQ(got[k]->subsystem, PsyQSubsystem::Other);
    }
}

TEST(PsyQMatcherDataDir, StubQueriesPartitionMatchesByStubType) {
    const std::string meta =
        "[[function]]\nname = \"a_stub\"\nstub_type = \"stub\"\n\n"
        "[[function]]\nname = \"a_skip\"\nstub_type = \"skip\"\n\n"
        "[[function]]\nname = \"a_pass\"\nstub_type = \"passthrough\"\n\n";
    DataDir dd("", meta);
    const std::string elf = dd.path("q.elf");
    std::vector<uint32_t> words;
    for (uint16_t t = 1; t <= 3; ++t)
        for (uint32_t w : body16(t)) words.push_back(w);
    writeElf(elf, kSynthBase, words,
             {{"a_stub", kSynthBase}, {"a_skip", kSynthBase + 16},
              {"a_pass", kSynthBase + 32}});
    PsyQMatcher matcher;

    matchAt(matcher, elf, kSynthBase);

    ASSERT_EQ(matcher.getStubs().size(), 1u);
    EXPECT_EQ(matcher.getStubs()[0]->name, "a_stub");
    ASSERT_EQ(matcher.getSkips().size(), 1u);
    EXPECT_EQ(matcher.getSkips()[0]->name, "a_skip");
    ASSERT_EQ(matcher.getPassthroughs().size(), 1u);
    EXPECT_EQ(matcher.getPassthroughs()[0]->name, "a_pass");
}

TEST(PsyQMatcherDataDir, SignatureWithoutSubsystemIsClassifiedByName) {
    DataDir dd(sigRow("CdControlF@v2", "", 16, ""), "");
    const std::string elf = dd.path("cd.elf");
    writeElf(elf, kSynthBase, body16(1), {{"CdControlF", kSynthBase}});
    PsyQMatcher matcher;

    auto m = matchAt(matcher, elf, kSynthBase);

    // The @v2 variant suffix is stripped, so the plain symbol name matches.
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->subsystem, PsyQSubsystem::CDROM);
    EXPECT_EQ(m->stubType, StubType::Recompile);
}

TEST(PsyQMatcherDataDir, ExplicitSubsystemAndStubTypeOnSignatureAreKept) {
    DataDir dd(sigRow("CdLike", "", 16,
                      "subsystem = \"sound\"\nstub_type = \"skip\"\n"), "");
    const std::string elf = dd.path("s.elf");
    writeElf(elf, kSynthBase, body16(1), {{"CdLike", kSynthBase}});
    PsyQMatcher matcher;

    auto m = matchAt(matcher, elf, kSynthBase);

    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->subsystem, PsyQSubsystem::Sound);
    EXPECT_EQ(m->stubType, StubType::Skip);
}

TEST(PsyQMatcherDataDir, MissingFilesAreReportedAsNotLoaded) {
    DataDir dd("", "");
    PsyQMatcher matcher;

    EXPECT_FALSE(matcher.loadFromToml(dd.path("nope_sigs.toml"), ""));
    EXPECT_FALSE(matcher.loadFromToml("", dd.path("nope_meta.toml")));
}

TEST(PsyQMatcherDataDir, MalformedTomlIsReportedAsNotLoaded) {
    DataDir dd("", "");
    writeFile(dd.path("bad.toml"), "[[signature]\nname = = \n");
    PsyQMatcher matcher;

    EXPECT_FALSE(matcher.loadFromToml(dd.path("bad.toml"), ""));
    EXPECT_FALSE(matcher.loadFromToml("", dd.path("bad.toml")));
}

TEST(PsyQMatcherDataDir, FilesWithoutTablesLoadButAddNothing) {
    DataDir dd("", "");
    writeFile(dd.path("other.toml"), "title = \"no rows\"\n");
    PsyQMatcher matcher;
    const size_t before = matcher.getDatabaseSize();

    EXPECT_TRUE(matcher.loadFromToml(dd.path("other.toml"), dd.path("other.toml")));
    EXPECT_EQ(matcher.getDatabaseSize(), before);
    EXPECT_EQ(matcher.getSignatureCount(), 0u);
}

TEST(PsyQMatcherDataDir, LibraryCountsTallyHashMatchesPerLibrary) {
    const char* libs[] = {"libgpu", "libetc", "libapi", "libcd",
                          "libgte", "libgs",  "libspu"};
    std::string sigs;
    std::vector<uint32_t> words;
    std::vector<Sym> syms;
    for (uint16_t i = 0; i < 7; ++i) {
        const auto b = bytesOfWords(body16(static_cast<uint16_t>(i + 1)));
        sigs += sigRow(std::string("Sig") + libs[i], libs[i], 16,
                       "hash_full = \"" + hex16(PsyQMatcher::hashFull(b.data(), b.size())) +
                           "\"\nmatch_mode = \"full\"\n");
        for (uint32_t w : body16(static_cast<uint16_t>(i + 1))) words.push_back(w);
        syms.push_back({"game_fn_" + std::to_string(i), kSynthBase + i * 16u});
    }
    DataDir dd(sigs, "");
    const std::string elf = dd.path("libs.elf");
    writeElf(elf, kSynthBase, words, syms);
    PsyQMatcher matcher;

    matchAt(matcher, elf, kSynthBase);
    const auto c = matcher.getLibraryCounts();

    EXPECT_EQ(matcher.getMatchCount(), 7u);
    EXPECT_EQ(c.libgpu, 1u);
    EXPECT_EQ(c.libetc, 1u);
    EXPECT_EQ(c.libapi, 1u);
    EXPECT_EQ(c.libcd, 1u);
    EXPECT_EQ(c.libgte, 1u);
    EXPECT_EQ(c.libgs, 1u);
    EXPECT_EQ(c.other, 1u);
}

TEST(PsyQMatcherDataDir, FullHashWithDifferentSizeDoesNotMatch) {
    const auto b = bytesOfWords(body16(1));
    DataDir dd(sigRow("SizeMismatch", "libgpu", 20,
                      "hash_full = \"" + hex16(PsyQMatcher::hashFull(b.data(), b.size())) + "\"\n"),
               "");
    const std::string elf = dd.path("m.elf");
    writeElf(elf, kSynthBase, body16(1), {{"game_fn", kSynthBase}});
    PsyQMatcher matcher;

    EXPECT_FALSE(matchAt(matcher, elf, kSynthBase).has_value());
}

TEST(PsyQMatcherDataDir, MaskedHashMatchesRelocatedBodyAboveFullModeLimit) {
    // 32 bytes (> 24): the masked hash is consulted when the full one misses.
    std::vector<uint32_t> ref = {0x3C048001u, 0x24840010u, 0x8C820000u, 0x24420001u,
                                 0xAC820000u, 0x03E00008u, 0x00000000u, 0x00000000u};
    std::vector<uint32_t> relocated = ref;
    relocated[0] = 0x3C048002u; // lui with another hi16
    relocated[1] = 0x24840ABCu; // addiu with another lo16
    const auto rb = bytesOfWords(ref);
    DataDir dd(sigRow("MaskedFn", "libgte", 32,
                      "hash_masked = \"" +
                          hex16(PsyQMatcher::hashMasked(rb.data(), rb.size())) + "\"\n"),
               "");
    const std::string elf = dd.path("mk.elf");
    writeElf(elf, kSynthBase, relocated, {{"game_fn", kSynthBase}});
    PsyQMatcher matcher;

    auto m = matchAt(matcher, elf, kSynthBase);

    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->name, "MaskedFn");
    EXPECT_EQ(m->library, "libgte");
}

TEST(PsyQMatcherDataDir, MaskedHashWithDifferentSizeDoesNotMatch) {
    std::vector<uint32_t> ref = {0x3C048001u, 0x24840010u, 0x8C820000u, 0x24420001u,
                                 0xAC820000u, 0x03E00008u, 0x00000000u, 0x00000000u};
    const auto rb = bytesOfWords(ref);
    DataDir dd(sigRow("MaskedFn", "libgte", 64,
                      "hash_masked = \"" +
                          hex16(PsyQMatcher::hashMasked(rb.data(), rb.size())) + "\"\n"),
               "");
    const std::string elf = dd.path("mk.elf");
    writeElf(elf, kSynthBase, ref, {{"game_fn", kSynthBase}});
    PsyQMatcher matcher;

    EXPECT_FALSE(matchAt(matcher, elf, kSynthBase).has_value());
}

TEST(PsyQMatcherDataDir, EightByteBodiesAreNeverHashMatched) {
    // `j tgt; nop` style trampolines collide with game code; they are skipped
    // even when the hash is in the database.
    const std::vector<uint32_t> tiny = {0x03E00008u, 0x24020007u};
    const auto b = bytesOfWords(tiny);
    DataDir dd(sigRow("Tiny", "libapi", 8,
                      "hash_full = \"" + hex16(PsyQMatcher::hashFull(b.data(), b.size())) + "\"\n"),
               "");
    const std::string elf = dd.path("t.elf");
    writeElf(elf, kSynthBase, tiny, {{"game_fn", kSynthBase}});
    PsyQMatcher matcher;

    EXPECT_FALSE(matchAt(matcher, elf, kSynthBase).has_value());
}

TEST(PsyQMatcherDataDir, PrefixPassClaimsNamedFunctionsInPsyQRange) {
    // isLikelyPsyQ: the SDK is linked at >= 0x800A0000.
    constexpr uint32_t kHigh = 0x800A0000;
    DataDir dd("", "");
    const std::string elf = dd.path("p.elf");
    std::vector<uint32_t> words;
    for (uint16_t t = 1; t <= 3; ++t)
        for (uint32_t w : body16(t)) words.push_back(w);
    writeElf(elf, kHigh, words,
             {{"SpuSetKey", kHigh}, {"func_800A0010", kHigh + 16},
              {"game_logic", kHigh + 32}});
    PsyQMatcher matcher;

    auto spu = matchAt(matcher, elf, kHigh);

    ASSERT_TRUE(spu.has_value());
    EXPECT_EQ(spu->subsystem, PsyQSubsystem::Sound);
    EXPECT_EQ(spu->stubType, StubType::Stub);
    EXPECT_FALSE(spu->exactMatch);
    // Auto-named and unclassifiable functions are left to the recompiler.
    EXPECT_EQ(matcher.getMatchCount(), 1u);
}

TEST(PsyQMatcherDataDir, PrefixPassIgnoresFunctionsBelowPsyQRange) {
    DataDir dd("", "");
    const std::string elf = dd.path("low.elf");
    writeElf(elf, kSynthBase, body16(1), {{"SpuSetKey", kSynthBase}});
    PsyQMatcher matcher;

    EXPECT_FALSE(matchAt(matcher, elf, kSynthBase).has_value());
}
