// Tests for ps1Analyzer -- Config Generator
// Validates TOML generation from analyzer results

#include <gtest/gtest.h>
#include <ps1recomp/config_generator.h>
#include <ps1recomp/elf_parser.h>
#include <ps1recomp/function_finder.h>
#include <ps1recomp/psyq_signatures.h>
#include <elfio/elfio.hpp>
#include <toml.hpp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ps1recomp/psyq_hle_allowlist.h>
#include <sstream>
#include <unistd.h>

using namespace ps1recomp;

// Helper: Create test ELF with known content

static std::string createTestElf(
    const std::string& path,
    const std::vector<std::pair<std::string, uint32_t>>& funcs,
    const std::vector<uint32_t>& code = {}) {

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

    // Use provided code or fill with NOPs
    std::vector<uint8_t> text_data;
    if (!code.empty()) {
        text_data.resize(code.size() * 4);
        for (size_t i = 0; i < code.size(); ++i) {
            text_data[i * 4 + 0] = (code[i] >>  0) & 0xFF;
            text_data[i * 4 + 1] = (code[i] >>  8) & 0xFF;
            text_data[i * 4 + 2] = (code[i] >> 16) & 0xFF;
            text_data[i * 4 + 3] = (code[i] >> 24) & 0xFF;
        }
    } else {
        text_data.resize(0x1000, 0x00); // 4KB of NOPs
    }
    text_sec->set_data(reinterpret_cast<const char*>(text_data.data()), text_data.size());

    // .data section
    auto* data_sec = writer.sections.add(".data");
    data_sec->set_type(ELFIO::SHT_PROGBITS);
    data_sec->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_WRITE);
    data_sec->set_addr_align(4);
    data_sec->set_address(0x80020000);
    std::vector<uint8_t> data_data(256, 0xAA);
    data_sec->set_data(reinterpret_cast<const char*>(data_data.data()), data_data.size());

    // Add symbol table
    if (!funcs.empty()) {
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

// Basic Generation Tests

TEST(ConfigGenerator, GeneratesValidTOML) {
    const std::string path = "/tmp/ps1recomp_test_config_basic.elf";

    createTestElf(path, {
        {"main",       0x80010000},
        {"game_loop",  0x80010100}
    });

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    PsyQMatcher matcher;
    matcher.matchFunctions(elf, finder);

    ConfigGenerator gen;
    std::string tomlStr = gen.generateString(elf, finder, matcher, path);

    // Should not be empty
    EXPECT_FALSE(tomlStr.empty());

    // Should parse as valid TOML
    std::istringstream is(tomlStr);
    auto parsed = toml::parse(is);

    // Check [binary] section
    EXPECT_TRUE(parsed.contains("binary"));
    auto& binary = parsed.at("binary");
    EXPECT_EQ(toml::find<std::string>(binary, "entry_point"), "0x80010000");

    cleanupFile(path);
}

TEST(ConfigGenerator, ContainsBinarySection) {
    const std::string path = "/tmp/ps1recomp_test_config_binary.elf";

    createTestElf(path, {{"main", 0x80010000}});

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    PsyQMatcher matcher;
    matcher.matchFunctions(elf, finder);

    ConfigGenerator gen;
    std::string tomlStr = gen.generateString(elf, finder, matcher, path);

    std::istringstream is(tomlStr);
    auto parsed = toml::parse(is);

    auto& binary = parsed.at("binary");
    EXPECT_EQ(toml::find<std::string>(binary, "path"), path);
    EXPECT_EQ(toml::find<std::string>(binary, "entry_point"), "0x80010000");
    EXPECT_GT(toml::find<int64_t>(binary, "total_code_size"), 0);

    cleanupFile(path);
}

TEST(ConfigGenerator, ContainsStatsSection) {
    const std::string path = "/tmp/ps1recomp_test_config_stats.elf";

    createTestElf(path, {
        {"main",        0x80010000},
        {"GsInitGraph", 0x800B0000}
    });

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    PsyQMatcher matcher;
    matcher.matchFunctions(elf, finder);

    ConfigGenerator gen;
    std::string tomlStr = gen.generateString(elf, finder, matcher, path);

    std::istringstream is(tomlStr);
    auto parsed = toml::parse(is);

    EXPECT_TRUE(parsed.contains("stats"));
    auto& stats = parsed.at("stats");
    EXPECT_GE(toml::find<int64_t>(stats, "total_functions"), 1);
    EXPECT_GE(toml::find<int64_t>(stats, "psyq_functions"), 1);

    cleanupFile(path);
}

// Function List Tests

TEST(ConfigGenerator, ListsGameFunctions) {
    const std::string path = "/tmp/ps1recomp_test_config_funcs.elf";

    createTestElf(path, {
        {"main",         0x80010000},
        {"game_update",  0x80010100}
    });

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    PsyQMatcher matcher;
    matcher.matchFunctions(elf, finder);

    ConfigGenerator gen;
    std::string tomlStr = gen.generateString(elf, finder, matcher, path);

    std::istringstream is(tomlStr);
    auto parsed = toml::parse(is);

    EXPECT_TRUE(parsed.contains("functions"));
    auto& funcs = parsed.at("functions");
    EXPECT_GE(funcs.as_array().size(), 1u);

    // Check first function has required fields
    auto& first = funcs.as_array().at(0);
    EXPECT_TRUE(first.contains("name"));
    EXPECT_TRUE(first.contains("address"));
    EXPECT_TRUE(first.contains("size"));
    EXPECT_TRUE(first.contains("source"));
    EXPECT_TRUE(first.contains("leaf"));

    cleanupFile(path);
}

// PsyQ Classification Tests

TEST(ConfigGenerator, SeparatesPsyQFunctions) {
    const std::string path = "/tmp/ps1recomp_test_config_psyq.elf";

    createTestElf(path, {
        {"main",         0x80010000},
        {"game_update",  0x80010100},
        {"GsInitGraph",  0x800B0000},  // stub
        {"printf",       0x800D0000},  // skip
        {"memcpy",       0x800D0100}   // passthrough
    });

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    PsyQMatcher matcher;
    matcher.matchFunctions(elf, finder);

    ConfigGenerator gen;
    std::string tomlStr = gen.generateString(elf, finder, matcher, path);

    std::istringstream is(tomlStr);
    auto parsed = toml::parse(is);

    // Stubs should contain GsInitGraph
    EXPECT_TRUE(parsed.contains("stubs"));
    bool hasGsInit = false;
    for (const auto& s : parsed.at("stubs").as_array()) {
        if (toml::find<std::string>(s, "name") == "GsInitGraph") {
            hasGsInit = true;
            EXPECT_TRUE(s.contains("subsystem"));
        }
    }
    EXPECT_TRUE(hasGsInit);

    // Skips should contain printf
    EXPECT_TRUE(parsed.contains("skips"));
    bool hasPrintf = false;
    for (const auto& s : parsed.at("skips").as_array()) {
        if (toml::find<std::string>(s, "name") == "printf") {
            hasPrintf = true;
        }
    }
    EXPECT_TRUE(hasPrintf);

    // Passthroughs should contain memcpy
    EXPECT_TRUE(parsed.contains("passthroughs"));
    bool hasMemcpy = false;
    for (const auto& s : parsed.at("passthroughs").as_array()) {
        if (toml::find<std::string>(s, "name") == "memcpy") {
            hasMemcpy = true;
        }
    }
    EXPECT_TRUE(hasMemcpy);

    // Game functions should NOT contain PsyQ functions
    for (const auto& f : parsed.at("functions").as_array()) {
        auto name = toml::find<std::string>(f, "name");
        EXPECT_NE(name, "GsInitGraph") << "PsyQ stub should not be in functions";
        EXPECT_NE(name, "printf") << "PsyQ skip should not be in functions";
        EXPECT_NE(name, "memcpy") << "PsyQ passthrough should not be in functions";
    }

    cleanupFile(path);
}

// File Write Test

TEST(ConfigGenerator, WritesToFile) {
    const std::string elfPath = "/tmp/ps1recomp_test_config_file.elf";
    const std::string outPath = "/tmp/ps1recomp_test_config_output.toml";

    createTestElf(elfPath, {{"main", 0x80010000}});

    ElfParser elf;
    ASSERT_TRUE(elf.load(elfPath));

    FunctionFinder finder;
    finder.findFunctions(elf);

    PsyQMatcher matcher;
    matcher.matchFunctions(elf, finder);

    ConfigGenerator gen;
    EXPECT_TRUE(gen.generate(elf, finder, matcher, elfPath, outPath));

    // Read back and parse
    auto parsed = toml::parse(outPath);
    EXPECT_TRUE(parsed.contains("binary"));
    EXPECT_TRUE(parsed.contains("stats"));
    EXPECT_TRUE(parsed.contains("functions"));

    cleanupFile(elfPath);
    cleanupFile(outPath);
}

TEST(ConfigGenerator, ReportsFileError) {
    const std::string elfPath = "/tmp/ps1recomp_test_config_err.elf";

    createTestElf(elfPath, {{"main", 0x80010000}});

    ElfParser elf;
    ASSERT_TRUE(elf.load(elfPath));

    FunctionFinder finder;
    finder.findFunctions(elf);

    PsyQMatcher matcher;
    matcher.matchFunctions(elf, finder);

    ConfigGenerator gen;
    // Invalid path should fail
    EXPECT_FALSE(gen.generate(elf, finder, matcher, elfPath,
                              "/nonexistent/dir/config.toml"));
    EXPECT_FALSE(gen.getError().empty());

    cleanupFile(elfPath);
}

// Empty Input Test

TEST(ConfigGenerator, HandleEmptyAnalysis) {
    const std::string path = "/tmp/ps1recomp_test_config_empty.elf";

    createTestElf(path, {}); // No symbols

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    PsyQMatcher matcher;
    matcher.matchFunctions(elf, finder);

    ConfigGenerator gen;
    std::string tomlStr = gen.generateString(elf, finder, matcher, path);

    // Should still produce valid TOML
    std::istringstream is(tomlStr);
    auto parsed = toml::parse(is);
    EXPECT_TRUE(parsed.contains("binary"));
    EXPECT_TRUE(parsed.contains("stats"));

    cleanupFile(path);
}

// HLE allowlist: the analyzer's view of what the runtime registry answers for.

TEST(PsyqHleAllowlist, RegisteredRuntimeNamesAreImplemented) {
    EXPECT_TRUE(psyqHleIsImplemented("libgpu_PutDispEnv"));
    EXPECT_TRUE(psyqHleIsImplemented("libetc_PadInit"));
    EXPECT_TRUE(psyqHleIsImplemented("libapi_ChangeClearPAD"));
}

TEST(PsyqHleAllowlist, UnregisteredOrMisspelledNamesAreNot) {
    EXPECT_FALSE(psyqHleIsImplemented("libgpu_NotARealFunction"));
    EXPECT_FALSE(psyqHleIsImplemented("PutDispEnv")) << "library prefix is part of the key";
    EXPECT_FALSE(psyqHleIsImplemented("libgpu_putdispenv")) << "keys are case-sensitive";
    EXPECT_FALSE(psyqHleIsImplemented(""));
}

// Hash-detected matches carry a library. Whether the runtime implements
// `<library>_<name>` decides if the function leaves [[functions]].
//
// The matcher reads its database from PS1RECOMP_DATA_DIR first, so each test
// points it at a private directory holding only the rows it needs.

namespace {

constexpr uint32_t kCfgBase = 0x80010000;

class PrivateDb {
public:
    explicit PrivateDb(const std::string& sigs) {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = (std::filesystem::temp_directory_path() /
                (std::string("ps1_cfg_") + info->name() + "_" + std::to_string(getpid())))
                   .string();
        std::filesystem::create_directories(dir_);
        std::ofstream(dir_ + "/psyq_signatures.toml") << sigs;
        std::ofstream(dir_ + "/psyq_metadata.toml") << "";
        if (const char* old = std::getenv("PS1RECOMP_DATA_DIR")) old_ = old;
        setenv("PS1RECOMP_DATA_DIR", dir_.c_str(), 1);
    }
    ~PrivateDb() {
        if (old_) setenv("PS1RECOMP_DATA_DIR", old_->c_str(), 1);
        else unsetenv("PS1RECOMP_DATA_DIR");
        std::filesystem::remove_all(dir_);
    }
    std::string path(const std::string& f) const { return dir_ + "/" + f; }

private:
    std::string dir_;
    std::optional<std::string> old_;
};

// 16-byte body unique per tag: addiu $v0,$0,tag ; addiu $v1,$0,tag ; jr $ra ; nop
std::vector<uint32_t> tagged(uint16_t tag) {
    return {0x24020000u | tag, 0x24030000u | tag, 0x03E00008u, 0x00000000u};
}

std::string fullHashOf(const std::vector<uint32_t>& words) {
    std::vector<uint8_t> b;
    for (uint32_t w : words)
        for (int k = 0; k < 4; ++k) b.push_back(static_cast<uint8_t>(w >> (8 * k)));
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx",
                  static_cast<unsigned long long>(PsyQMatcher::hashFull(b.data(), b.size())));
    return hex;
}

struct HashRow {
    const char* name;
    const char* library;
    const char* stubType;
};

// One function per row at kCfgBase + 16*i, each matched by hash to its row.
toml::value configFor(const std::vector<HashRow>& rows, std::optional<PrivateDb>& holder) {
    std::string sigs;
    std::vector<uint32_t> code;
    std::vector<std::pair<std::string, uint32_t>> syms;
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto body = tagged(static_cast<uint16_t>(i + 1));
        sigs += std::string("[[signature]]\nname = \"") + rows[i].name +
                "\"\nlibrary = \"" + rows[i].library + "\"\nsize = 16\nstub_type = \"" +
                rows[i].stubType + "\"\nhash_full = \"" + fullHashOf(body) + "\"\n\n";
        code.insert(code.end(), body.begin(), body.end());
        syms.push_back({"game_fn_" + std::to_string(i),
                        kCfgBase + static_cast<uint32_t>(i * 16)});
    }
    holder.emplace(sigs);
    const std::string elfPath = holder->path("game.elf");
    createTestElf(elfPath, syms, code);
    ElfParser elf;
    elf.load(elfPath);
    FunctionFinder finder;
    finder.findFunctions(elf);
    PsyQMatcher matcher;
    matcher.matchFunctions(elf, finder);
    std::istringstream is(ConfigGenerator().generateString(elf, finder, matcher, elfPath));
    return toml::parse(is);
}

const toml::value* rowNamed(const toml::value& cfg, const char* table,
                            const std::string& key, const std::string& value) {
    for (const auto& r : toml::find(cfg, table).as_array())
        if (toml::find<std::string>(r, key) == value) return &r;
    return nullptr;
}

bool listsFunctionAt(const toml::value& cfg, const std::string& addr) {
    return rowNamed(cfg, "functions", "address", addr) != nullptr;
}

} // namespace

TEST(ConfigGeneratorHle, ImplementedHashMatchIsDispatchedAndNotRecompiled) {
    std::optional<PrivateDb> holder;
    auto cfg = configFor({{"PutDispEnv", "libgpu", "recompile"}}, holder);

    const auto* hle = rowNamed(cfg, "hle_functions", "name", "libgpu_PutDispEnv");

    ASSERT_NE(hle, nullptr);
    EXPECT_TRUE(toml::find<bool>(*hle, "hle"));
    EXPECT_EQ(toml::find<std::string>(*hle, "address"), "0x80010000");
    EXPECT_EQ(toml::find<std::string>(*hle, "library"), "libgpu");
    EXPECT_EQ(toml::find<std::string>(*hle, "subsystem"), "Graphics");
    EXPECT_EQ(toml::find<std::string>(*hle, "stub_type"), "recompile");
    EXPECT_FALSE(listsFunctionAt(cfg, "0x80010000"));
}

TEST(ConfigGeneratorHle, UnimplementedHashMatchStaysARecompiledFunction) {
    std::optional<PrivateDb> holder;
    auto cfg = configFor({{"NoBodyYet", "libgpu", "stub"}}, holder);

    const auto* hle = rowNamed(cfg, "hle_functions", "name", "libgpu_NoBodyYet");

    // Identified, but with no runtime body it must not be dispatched.
    ASSERT_NE(hle, nullptr);
    EXPECT_FALSE(toml::find<bool>(*hle, "hle"));
    EXPECT_TRUE(listsFunctionAt(cfg, "0x80010000"));
    EXPECT_EQ(rowNamed(cfg, "stubs", "name", "NoBodyYet"), nullptr);
}

TEST(ConfigGeneratorHle, HandledHashMatchesKeepTheirLibraryInStubTables) {
    std::optional<PrivateDb> holder;
    auto cfg = configFor({{"PadInit", "libetc", "stub"},
                          {"strlen", "libetc", "skip"},
                          {"labs", "libetc", "passthrough"}},
                         holder);

    const auto* stub = rowNamed(cfg, "stubs", "name", "PadInit");
    const auto* skip = rowNamed(cfg, "skips", "name", "strlen");
    const auto* pass = rowNamed(cfg, "passthroughs", "name", "labs");

    ASSERT_NE(stub, nullptr);
    ASSERT_NE(skip, nullptr);
    ASSERT_NE(pass, nullptr);
    EXPECT_EQ(toml::find<std::string>(*stub, "library"), "libetc");
    EXPECT_EQ(toml::find<std::string>(*skip, "library"), "libetc");
    EXPECT_EQ(toml::find<std::string>(*pass, "library"), "libetc");
}

TEST(ConfigGeneratorHle, UnhandledSkipsAndPassthroughsAreLeftOut) {
    std::optional<PrivateDb> holder;
    auto cfg = configFor({{"NoSkip", "libetc", "skip"},
                          {"NoPass", "libetc", "passthrough"}},
                         holder);

    EXPECT_EQ(rowNamed(cfg, "skips", "name", "NoSkip"), nullptr);
    EXPECT_EQ(rowNamed(cfg, "passthroughs", "name", "NoPass"), nullptr);
    EXPECT_TRUE(listsFunctionAt(cfg, "0x80010000"));
    EXPECT_TRUE(listsFunctionAt(cfg, "0x80010010"));
}

TEST(ConfigGeneratorSource, SourceNamesTheDetectionPassThatFoundEachFunction) {
    // Entry at +0, computed jump into 8-byte returning slots at +0x20..+0x30,
    // a stack-frame prologue at +0x38, and an unreferenced body at +0x48.
    std::vector<uint32_t> code(21, 0);
    code[0] = 0x3C088001u;           // lui   $t0, 0x8001
    code[1] = 0x25080020u;           // addiu $t0, $t0, 0x20
    code[2] = 0x000448C0u;           // sll   $t1, $a0, 3
    code[3] = 0x01094021u;           // addu  $t0, $t0, $t1
    code[4] = 0x01000008u;           // jr    $t0
    code[8] = 0x03E00008u;           // slot 0
    code[10] = 0x03E00008u;          // slot 1
    code[12] = 0x03E00008u;          // slot 2
    code[14] = 0x27BDFFF0u;          // addiu $sp, $sp, -16
    code[16] = 0x03E00008u;
    code[18] = 0x00851021u;          // addu $v0, $a0, $a1
    code[19] = 0x03E00008u;
    std::optional<PrivateDb> holder;
    holder.emplace("");
    const std::string elfPath = holder->path("src.elf");
    createTestElf(elfPath, {}, code);
    ElfParser elf;
    ASSERT_TRUE(elf.load(elfPath));
    FunctionFinder finder;
    finder.findFunctions(elf);
    PsyQMatcher matcher;
    matcher.matchFunctions(elf, finder);

    std::istringstream is(ConfigGenerator().generateString(elf, finder, matcher, elfPath));
    auto cfg = toml::parse(is);

    auto sourceAt = [&](const std::string& addr) {
        const auto* f = rowNamed(cfg, "functions", "address", addr);
        return f ? toml::find<std::string>(*f, "source") : std::string("<missing>");
    };
    EXPECT_EQ(sourceAt("0x80010000"), "entry_point");
    EXPECT_EQ(sourceAt("0x80010020"), "jump_array");
    EXPECT_EQ(sourceAt("0x80010038"), "prologue");
    EXPECT_EQ(sourceAt("0x80010048"), "linear_sweep");
}
