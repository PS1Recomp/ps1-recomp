// Tests: ps1Analyzer and ps1Recomp command-line tools.
// Every input (PS-X EXE, ELF, BIN disc, TOML) is synthesized in a unique temp
// dir, so these run in CI without a ROM. The executables are run as child
// processes; their output and exit status are the contract under test.

#include <gtest/gtest.h>

#include <elfio/elfio.hpp>
#include <toml.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct RunResult {
  int code;
  std::string out; // stdout + stderr
};

std::string q(const std::string &s) { return "'" + s + "'"; }

RunResult run(const std::string &cmd) {
  RunResult r{-1, {}};
  FILE *p = popen((cmd + " 2>&1").c_str(), "r");
  if (!p)
    return r;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), p)) > 0)
    r.out.append(buf, n);
  int st = pclose(p);
  r.code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  return r;
}

std::string readText(const fs::path &p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void writeBytes(const fs::path &p, const std::vector<uint8_t> &d) {
  std::ofstream f(p, std::ios::binary);
  f.write(reinterpret_cast<const char *>(d.data()), d.size());
}

void writeText(const fs::path &p, const std::string &s) {
  std::ofstream f(p, std::ios::binary);
  f << s;
}

void put32(std::vector<uint8_t> &d, size_t off, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    d[off + i] = static_cast<uint8_t>(v >> (8 * i));
}

// psx-spx "CDROM File Formats" / PS-X EXE: 2KB header, pc0 at 0x10,
// t_addr at 0x18, t_size at 0x1C, little-endian code payload at 0x800.
std::vector<uint8_t> makePsxExe(uint32_t pc, uint32_t tAddr,
                                const std::vector<uint32_t> &code) {
  std::vector<uint8_t> d(0x800 + code.size() * 4, 0);
  std::memcpy(d.data(), "PS-X EXE", 8);
  put32(d, 0x10, pc);
  put32(d, 0x18, tAddr);
  put32(d, 0x1C, static_cast<uint32_t>(code.size() * 4));
  for (size_t i = 0; i < code.size(); ++i)
    put32(d, 0x800 + i * 4, code[i]);
  return d;
}

constexpr uint32_t kBase = 0x80010000;
constexpr uint32_t kNop = 0x00000000;
constexpr uint32_t kJrRa = 0x03E00008;

// MIPS J-type target field: (addr >> 2) & 0x03FFFFFF.
uint32_t jal(uint32_t target) { return 0x0C000000 | ((target >> 2) & 0x03FFFFFF); }

// Two functions: the entry (32 bytes, framed, calls the leaf) and a leaf at
// +0x20 (12 bytes) returning 42. Each ends with JR $ra + its delay slot.
std::vector<uint32_t> twoFunctionCode() {
  return {
      0x27BDFFF8,           // addiu sp, sp, -8
      0xAFBF0004,           // sw    ra, 4(sp)
      jal(kBase + 0x20),    // jal   leaf
      kNop,                 //
      0x8FBF0004,           // lw    ra, 4(sp)
      0x27BD0008,           // addiu sp, sp, 8
      kJrRa,                // jr    ra
      kNop,                 //
      0x2402002A,           // leaf: addiu v0, zero, 42
      kJrRa,                // jr    ra
      kNop,                 //
  };
}

// ISO9660 directory record (ECMA-119 9.1): length, LBA LE at 2, size LE at 10,
// flags at 25, name length at 32, name at 33; padded to an even length.
void putDirRecord(std::vector<uint8_t> &sec, size_t &off, uint32_t lba,
                  uint32_t size, const std::string &name) {
  size_t len = 33 + name.size();
  len += len & 1;
  sec[off] = static_cast<uint8_t>(len);
  put32(sec, off + 2, lba);
  put32(sec, off + 10, size);
  sec[off + 32] = static_cast<uint8_t>(name.size());
  std::memcpy(&sec[off + 33], name.data(), name.size());
  off += len;
}

// Raw Mode 2 BIN (2352-byte sectors, user data at +24 per psx-spx "CDROM
// Sector Encoding"): PVD at LBA 16, root directory at LBA 18, files from 20.
std::vector<uint8_t>
makeBin(const std::vector<std::pair<std::string, std::vector<uint8_t>>> &files,
        bool validPvd = true) {
  constexpr size_t kRaw = 2352, kData = 2048, kHdr = 24;
  std::vector<std::vector<uint8_t>> sectors(20, std::vector<uint8_t>(kData, 0));
  auto &pvd = sectors[16];
  pvd[0] = 0x01;
  std::memcpy(&pvd[1], validPvd ? "CD001" : "XXXXX", 5);
  put32(pvd, 156 + 2, 18);
  put32(pvd, 156 + 10, kData);

  size_t off = 0;
  for (const auto &[name, data] : files) {
    uint32_t lba = static_cast<uint32_t>(sectors.size());
    putDirRecord(sectors[18], off, lba, static_cast<uint32_t>(data.size()),
                 name + ";1");
    for (size_t i = 0; i < data.size(); i += kData) {
      std::vector<uint8_t> s(kData, 0);
      std::memcpy(s.data(), data.data() + i, std::min(kData, data.size() - i));
      sectors.push_back(std::move(s));
    }
  }

  std::vector<uint8_t> bin(sectors.size() * kRaw, 0);
  for (size_t i = 0; i < sectors.size(); ++i)
    std::memcpy(&bin[i * kRaw + kHdr], sectors[i].data(), kData);
  return bin;
}

std::vector<uint8_t> bytesOf(const std::string &s) { return {s.begin(), s.end()}; }

// Analyzer: <bin> <input> [output] [--add-func addr]... prints and writes TOML.
class AnalyzerCli : public ::testing::Test {
protected:
  fs::path dir;
  const std::string bin = PS1RECOMP_ANALYZER_BIN;

  void SetUp() override {
    auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir = fs::temp_directory_path() /
          (std::string("ps1_cli_") + info->test_case_name() + "_" + info->name() +
           "_" + std::to_string(getpid())); // two builds may run the suite at once
    fs::remove_all(dir);
    fs::create_directories(dir);
  }
  void TearDown() override { fs::remove_all(dir); }

  RunResult analyze(const std::string &args) { return run(q(bin) + " " + args); }

  fs::path writeExe(const std::string &name = "game.exe") {
    auto p = dir / name;
    writeBytes(p, makePsxExe(kBase, kBase, twoFunctionCode()));
    return p;
  }

  // Returns the [[functions]] entry whose address matches, or nullptr.
  static const toml::value *findFunc(const toml::value &cfg, const std::string &addr) {
    for (const auto &f : toml::find(cfg, "functions").as_array())
      if (toml::find<std::string>(f, "address") == addr)
        return &f;
    return nullptr;
  }
};

TEST_F(AnalyzerCli, NoArgumentsPrintsUsageAndFails) {
  auto r = run(q(bin));
  EXPECT_EQ(r.code, 1);
  EXPECT_NE(r.out.find("Usage: ps1Analyzer"), std::string::npos);
}

TEST_F(AnalyzerCli, MissingInputFileFails) {
  auto r = analyze(q((dir / "nope.exe").string()));
  EXPECT_EQ(r.code, 1);
  EXPECT_NE(r.out.find("Could not open file"), std::string::npos);
}

TEST_F(AnalyzerCli, PsxExeEntryPointBadFails) {
  // pc0 in Expansion Region 1 is not main RAM (psx-spx "Memory Map").
  auto p = dir / "bad.exe";
  writeBytes(p, makePsxExe(0x1F000000, kBase, twoFunctionCode()));
  auto r = analyze(q(p.string()));
  EXPECT_EQ(r.code, 1);
  EXPECT_NE(r.out.find("not in PS1 RAM"), std::string::npos);
}

TEST_F(AnalyzerCli, PsxExeConfigListsEntryAndCalleeWithSizes) {
  auto exe = writeExe();
  auto out = dir / "cfg.toml";
  auto r = analyze(q(exe.string()) + " " + q(out.string()));
  ASSERT_EQ(r.code, 0) << r.out;
  EXPECT_NE(r.out.find("Entry point: 0x80010000"), std::string::npos);
  EXPECT_NE(r.out.find("[TEXT]"), std::string::npos);

  auto cfg = toml::parse(out.string());
  EXPECT_EQ(toml::find<std::string>(cfg, "binary", "entry_point"), "0x80010000");
  EXPECT_EQ(toml::find<std::string>(cfg, "binary", "path"), exe.string());
  const auto *entry = findFunc(cfg, "0x80010000");
  const auto *leaf = findFunc(cfg, "0x80010020");
  ASSERT_NE(entry, nullptr);
  ASSERT_NE(leaf, nullptr);
  // Each function spans up to and including the JR $ra delay slot.
  EXPECT_EQ(toml::find<int64_t>(*entry, "size"), 32);
  EXPECT_EQ(toml::find<int64_t>(*leaf, "size"), 12);
}

TEST_F(AnalyzerCli, WithoutOutputPathSkipsGeneration) {
  auto exe = writeExe();
  auto r = analyze(q(exe.string()));
  EXPECT_EQ(r.code, 0);
  EXPECT_NE(r.out.find("Skipping generation"), std::string::npos);
  EXPECT_NE(r.out.find("Total functions:"), std::string::npos);
}

TEST_F(AnalyzerCli, AddFuncInsertsNamedFunctionAndSplitsCaller) {
  auto exe = writeExe();
  auto out = dir / "cfg.toml";
  // 0x80010010 sits inside the entry function (after the JAL delay slot).
  auto r = analyze(q(exe.string()) + " --add-func 80010010 " + q(out.string()));
  ASSERT_EQ(r.code, 0) << r.out;
  auto cfg = toml::parse(out.string());
  const auto *added = findFunc(cfg, "0x80010010");
  ASSERT_NE(added, nullptr);
  EXPECT_EQ(toml::find<std::string>(*added, "name"), "func_added_80010010");
  // The enclosing function now ends where the added one starts.
  EXPECT_EQ(toml::find<int64_t>(*findFunc(cfg, "0x80010000"), "size"), 16);
}

TEST_F(AnalyzerCli, UnwritableOutputPathFails) {
  auto exe = writeExe();
  auto r = analyze(q(exe.string()) + " " + q((dir / "no_dir" / "cfg.toml").string()));
  EXPECT_EQ(r.code, 1);
  EXPECT_NE(r.out.find("Failed to generate config"), std::string::npos);
}

TEST_F(AnalyzerCli, ElfSectionsAreClassifiedInListing) {
  ELFIO::elfio w;
  w.create(ELFIO::ELFCLASS32, ELFIO::ELFDATA2LSB);
  w.set_type(ELFIO::ET_EXEC);
  w.set_machine(ELFIO::EM_MIPS);
  w.set_entry(kBase);
  auto code = twoFunctionCode();
  auto *text = w.sections.add(".text");
  text->set_type(ELFIO::SHT_PROGBITS);
  text->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_EXECINSTR);
  text->set_address(kBase);
  text->set_data(reinterpret_cast<const char *>(code.data()), code.size() * 4);
  auto *data = w.sections.add(".data");
  data->set_type(ELFIO::SHT_PROGBITS);
  data->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_WRITE);
  data->set_address(kBase + 0x1000);
  data->set_data("\1\2\3\4", 4);
  auto *bss = w.sections.add(".bss");
  bss->set_type(ELFIO::SHT_NOBITS);
  bss->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_WRITE);
  bss->set_address(kBase + 0x2000);
  bss->set_size(64);
  auto p = dir / "game.elf";
  ASSERT_TRUE(w.save(p.string()));

  auto r = analyze(q(p.string()) + " " + q((dir / "cfg.toml").string()));
  ASSERT_EQ(r.code, 0) << r.out;
  EXPECT_NE(r.out.find(".text"), std::string::npos);
  EXPECT_NE(r.out.find("[TEXT]"), std::string::npos);
  EXPECT_NE(r.out.find("[DATA]"), std::string::npos);
  EXPECT_NE(r.out.find("[BSS ]"), std::string::npos);
  auto cfg = toml::parse((dir / "cfg.toml").string());
  EXPECT_NE(findFunc(cfg, "0x80010000"), nullptr);
}

TEST_F(AnalyzerCli, BinDiscBootsExeNamedInSystemCnf) {
  auto exe = makePsxExe(kBase, kBase, twoFunctionCode());
  auto disc = dir / "game.bin";
  writeBytes(disc, makeBin({{"SYSTEM.CNF", bytesOf("BOOT = cdrom:\\SLUS_000.01;1\r\n")},
                            {"SLUS_000.01", exe}}));
  auto out = dir / "cfg.toml";
  auto r = analyze(q(disc.string()) + " " + q(out.string()));
  ASSERT_EQ(r.code, 0) << r.out;
  EXPECT_NE(r.out.find("Boot executable: SLUS_000.01"), std::string::npos);
  // The extracted EXE is byte-identical to what was put on the disc.
  auto extracted = fs::path(disc.string() + ".boot.exe");
  ASSERT_TRUE(fs::exists(extracted));
  auto got = readText(extracted);
  EXPECT_EQ(std::vector<uint8_t>(got.begin(), got.end()), exe);
  auto cfg = toml::parse(out.string());
  EXPECT_EQ(toml::find<std::string>(cfg, "binary", "entry_point"), "0x80010000");
  EXPECT_NE(findFunc(cfg, "0x80010020"), nullptr);
}

TEST_F(AnalyzerCli, BinDiscWithoutCd001Fails) {
  auto disc = dir / "bad.bin";
  writeBytes(disc, makeBin({{"PSX.EXE", makePsxExe(kBase, kBase, twoFunctionCode())}},
                           /*validPvd=*/false));
  auto r = analyze(q(disc.string()));
  EXPECT_EQ(r.code, 1);
  EXPECT_NE(r.out.find("CD001"), std::string::npos);
}

TEST_F(AnalyzerCli, MissingBinDiscFails) {
  auto r = analyze(q((dir / "nope.bin").string()));
  EXPECT_EQ(r.code, 1);
  EXPECT_NE(r.out.find("Could not open BIN file"), std::string::npos);
}

TEST_F(AnalyzerCli, BinDiscWithoutBootInfoFails) {
  auto disc = dir / "noboot.bin";
  writeBytes(disc, makeBin({{"README.TXT", bytesOf(std::string(300, 'x'))}}));
  auto r = analyze(q(disc.string()));
  EXPECT_EQ(r.code, 1);
  EXPECT_NE(r.out.find("Could not determine boot executable"), std::string::npos);
}

TEST_F(AnalyzerCli, BinDiscBootFileAbsentFails) {
  auto disc = dir / "dangling.bin";
  writeBytes(disc, makeBin({{"SYSTEM.CNF", bytesOf("BOOT = cdrom:\\MISSING.EXE;1\n")}}));
  auto r = analyze(q(disc.string()));
  EXPECT_EQ(r.code, 1);
  EXPECT_NE(r.out.find("Boot file 'MISSING.EXE' not found"), std::string::npos);
}

TEST_F(AnalyzerCli, BinDiscEmptyBootFileFails) {
  auto disc = dir / "empty.bin";
  writeBytes(disc, makeBin({{"PSX.EXE", {}}}));
  auto r = analyze(q(disc.string()));
  EXPECT_EQ(r.code, 1);
  EXPECT_NE(r.out.find("Failed to read boot executable"), std::string::npos);
}

TEST_F(AnalyzerCli, ScanOverlaysWithoutDiscArgumentFails) {
  auto r = analyze("--scan-overlays");
  EXPECT_EQ(r.code, 1);
  EXPECT_NE(r.out.find("--scan-overlays <disc.bin>"), std::string::npos);
}

TEST_F(AnalyzerCli, ScanOverlaysMissingDiscFails) {
  auto r = analyze("--scan-overlays " + q((dir / "nope.bin").string()));
  EXPECT_EQ(r.code, 1);
  EXPECT_NE(r.out.find("Error opening disc"), std::string::npos);
}

TEST_F(AnalyzerCli, ScanOverlaysBadFilesystemFails) {
  auto disc = dir / "bad.bin";
  writeBytes(disc, makeBin({}, /*validPvd=*/false));
  auto r = analyze("--scan-overlays " + q(disc.string()));
  EXPECT_EQ(r.code, 1);
  EXPECT_NE(r.out.find("Error parsing filesystem"), std::string::npos);
}

class AnalyzerOverlayDisc : public AnalyzerCli {
protected:
  fs::path disc;
  void SetUp() override {
    AnalyzerCli::SetUp();
    disc = dir / "ovl.bin";
    // Boot EXE is excluded from the scan; OVL.EXE has a PS-X EXE header at
    // its own load address, so it must be reported as an overlay.
    writeBytes(disc, makeBin({{"PSX.EXE", makePsxExe(kBase, kBase, twoFunctionCode())},
                              {"OVL.EXE", makePsxExe(0x80100000, 0x80100000,
                                                     twoFunctionCode())}}));
  }
};

TEST_F(AnalyzerOverlayDisc, WritesOverlayTomlForNonBootExe) {
  auto out = dir / "ovl.toml";
  auto r = analyze("--scan-overlays " + q(disc.string()) + " " + q(out.string()));
  ASSERT_EQ(r.code, 0) << r.out;
  EXPECT_NE(r.out.find("Disc loaded: 2 files found"), std::string::npos);
  EXPECT_NE(r.out.find("OVL.EXE -> RAM 0x80100000"), std::string::npos);
  auto cfg = toml::parse(out.string());
  const auto &ovls = toml::find(cfg, "overlays").as_array();
  ASSERT_EQ(ovls.size(), 1u);
  EXPECT_EQ(toml::find<std::string>(ovls[0], "name"), "OVL");
  EXPECT_EQ(toml::find<int64_t>(ovls[0], "ram_base"), 0x80100000);
}

TEST_F(AnalyzerOverlayDisc, PrintsOverlayTomlWithoutOutputPath) {
  auto r = analyze("--scan-overlays " + q(disc.string()));
  ASSERT_EQ(r.code, 0) << r.out;
  EXPECT_NE(r.out.find("[[overlays]]"), std::string::npos);
  EXPECT_NE(r.out.find("name = \"OVL\""), std::string::npos);
  EXPECT_EQ(r.out.find("name = \"PSX\""), std::string::npos);
}

TEST_F(AnalyzerOverlayDisc, UnwritableOverlayOutputFails) {
  auto r = analyze("--scan-overlays " + q(disc.string()) + " " +
                   q((dir / "no_dir" / "o.toml").string()));
  EXPECT_EQ(r.code, 1);
  EXPECT_NE(r.out.find("could not write"), std::string::npos);
}

// Recompiler: <bin> <config.toml> [output.cpp] emits one C++ TU.
class RecompCli : public AnalyzerCli {
protected:
  const std::string recomp = PS1RECOMP_RECOMP_BIN;
  fs::path exe, out;

  void SetUp() override {
    AnalyzerCli::SetUp();
    exe = writeExe();
    out = dir / "out.cpp";
  }

  std::string header() const {
    return "[binary]\npath = \"" + exe.string() + "\"\nentry_point = \"0x80010000\"\n\n";
  }

  static std::string func(const std::string &name, const std::string &addr, int size) {
    return "[[functions]]\nname = \"" + name + "\"\naddress = \"" + addr +
           "\"\nsize = " + std::to_string(size) + "\n\n";
  }

  RunResult recompile(const std::string &toml) {
    auto cfg = dir / "cfg.toml";
    writeText(cfg, toml);
    return run(q(recomp) + " " + q(cfg.string()) + " " + q(out.string()));
  }

  std::string output() const { return readText(out); }

  static bool has(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
  }
  static std::string def(const std::string &name) {
    return "void " + name + "(uint8_t* rdram, recomp_context* ctx) {";
  }
};

TEST_F(RecompCli, NoArgumentsPrintsUsageAndFails) {
  auto r = run(q(recomp));
  EXPECT_EQ(r.code, 1);
  EXPECT_TRUE(has(r.out, "Usage: ps1Recomp"));
}

TEST_F(RecompCli, MissingConfigFails) {
  auto r = run(q(recomp) + " " + q((dir / "nope.toml").string()));
  EXPECT_EQ(r.code, 1);
  EXPECT_TRUE(has(r.out, "Error parsing config"));
}

TEST_F(RecompCli, MissingBinaryFails) {
  auto r = recompile("[binary]\npath = \"" + (dir / "nope.exe").string() +
                     "\"\nentry_point = \"0x80010000\"\n");
  EXPECT_EQ(r.code, 1);
  EXPECT_TRUE(has(r.out, "Failed to load ELF"));
}

TEST_F(RecompCli, ConfigWithoutFunctionsArrayFails) {
  auto r = recompile(header());
  EXPECT_EQ(r.code, 1);
  EXPECT_TRUE(has(r.out, "Error parsing config"));
  EXPECT_FALSE(fs::exists(out));
}

TEST_F(RecompCli, UnwritableOutputFails) {
  auto cfg = dir / "cfg.toml";
  writeText(cfg, header() + func("f", "0x80010020", 12));
  auto r = run(q(recomp) + " " + q(cfg.string()) + " " +
               q((dir / "no_dir" / "o.cpp").string()));
  EXPECT_EQ(r.code, 1);
  EXPECT_TRUE(has(r.out, "Failed to write output"));
}

TEST_F(RecompCli, DefaultOutputIsRecompiledOutInWorkingDir) {
  auto cfg = dir / "cfg.toml";
  writeText(cfg, header() + func("f", "0x80010020", 12));
  auto r = run("cd " + q(dir.string()) + " && " + q(recomp) + " cfg.toml");
  ASSERT_EQ(r.code, 0) << r.out;
  EXPECT_TRUE(has(readText(dir / "recompiled_out.cpp"), def("f")));
}

TEST_F(RecompCli, EmitsDefinitionAndDispatchEntryPerFunction) {
  auto r = recompile(header() + func("main", "0x80010000", 32) +
                     func("lib.leaf", "0x80010020", 12));
  ASSERT_EQ(r.code, 0) << r.out;
  auto cpp = output();
  // "main" would clash with the host main; dots are not valid identifiers.
  EXPECT_TRUE(has(cpp, def("func_main")));
  EXPECT_TRUE(has(cpp, def("lib_leaf")));
  EXPECT_TRUE(has(cpp, "recomp_func_table[0x80010000] = func_main;"));
  EXPECT_TRUE(has(cpp, "recomp_func_table[0x80010020] = lib_leaf;"));
  EXPECT_TRUE(has(r.out, "Dispatch table: 2 entries"));
  // The JAL target is a known function, so no stub is generated for it.
  EXPECT_FALSE(has(cpp, "STUB generated by JAL"));
}

TEST_F(RecompCli, JalToUnlistedTargetGetsStub) {
  auto r = recompile(header() + func("entry", "0x80010000", 32));
  ASSERT_EQ(r.code, 0) << r.out;
  EXPECT_TRUE(has(output(), "void func_80010020(uint8_t* rdram, recomp_context* ctx) "
                            "{ /* STUB generated by JAL */ }"));
}

TEST_F(RecompCli, FunctionOutsideTextIsNotTranslated) {
  auto r = recompile(header() + func("entry", "0x80010000", 32) +
                     func("ghost", "0x80200000", 8));
  ASSERT_EQ(r.code, 0) << r.out;
  auto cpp = output();
  EXPECT_TRUE(has(cpp, "void ghost(uint8_t* rdram, recomp_context* ctx);"));
  EXPECT_FALSE(has(cpp, def("ghost")));
}

TEST_F(RecompCli, HleOverrideReplacesMipsBody) {
  auto r = recompile(header() + func("entry", "0x80010000", 32) +
                     func("leaf", "0x80010020", 12) +
                     "[hle_overrides]\n\"0x80010020\" = \"hle_VSync\"\n");
  ASSERT_EQ(r.code, 0) << r.out;
  auto cpp = output();
  EXPECT_TRUE(has(cpp, def("leaf") + "\n    ps1::psyq::hle_VSync(ctx);\n}"));
  EXPECT_TRUE(has(r.out, "HLE overrides: 1 address(es)"));
}

TEST_F(RecompCli, MissingFunctionBodyIsEmittedVerbatim) {
  auto r = recompile(header() + func("leaf", "0x80010020", 12) +
                     "[missing_functions]\n\"0x80010020\" = \"ctx->r[2] = 7;\"\n");
  ASSERT_EQ(r.code, 0) << r.out;
  EXPECT_TRUE(has(output(), def("leaf") + "\n    ctx->r[2] = 7;\n}"));
}

TEST_F(RecompCli, HleFunctionsDispatchByNameAndHonourHleFlag) {
  auto r = recompile(header() + func("entry", "0x80010000", 32) +
                     func("leaf", "0x80010020", 12) +
                     "[[hle_functions]]\naddress = \"0x80010020\"\nhle = true\n"
                     "name = \"libgpu_PutDispEnv\"\n\n"
                     "[[hle_functions]]\naddress = \"0x80030000\"\nhle = true\n"
                     "name = \"libetc_VSync\"\n\n"
                     "[[hle_functions]]\naddress = \"0x80040000\"\nhle = false\n"
                     "name = \"libapi_Ignored\"\n");
  ASSERT_EQ(r.code, 0) << r.out;
  auto cpp = output();
  // Listed function: HLE body replaces translation under its own name.
  EXPECT_TRUE(has(cpp, def("leaf") + "\n    psyq_dispatch(\"libgpu_PutDispEnv\", ctx);"));
  // Unlisted HLE address: body emitted under func_<addr> and dispatch-mapped.
  EXPECT_TRUE(has(cpp, def("func_80030000") + "\n    psyq_dispatch(\"libetc_VSync\", ctx);"));
  EXPECT_TRUE(has(cpp, "recomp_func_table[0x80030000] = func_80030000;"));
  // hle = false entries are left to LLE.
  EXPECT_FALSE(has(cpp, "libapi_Ignored"));
  EXPECT_TRUE(has(r.out, "HLE functions ([[hle_functions]]): 2"));
}

TEST_F(RecompCli, StubsMapKnownPsyQNamesToHleAndOthersToNoop) {
  std::string stubs;
  const char *names[] = {"VSync",      "DrawSync",   "ResetGraph",   "ClearOTag",
                         "ClearOTagR", "DrawOTag",   "SetDefDispEnv", "PutDispEnv",
                         "SetDefDrawEnv", "PutDrawEnv", "Unknown.Fn"};
  uint32_t a = 0x80050000;
  for (const char *n : names) {
    char addr[16];
    std::snprintf(addr, sizeof(addr), "0x%08X", a);
    a += 0x10;
    stubs += std::string("[[stubs]]\nname = \"") + n + "\"\naddress = \"" + addr + "\"\n\n";
  }
  auto r = recompile(header() + func("leaf", "0x80010020", 12) + stubs);
  ASSERT_EQ(r.code, 0) << r.out;
  auto cpp = output();
  for (const char *n : names) {
    std::string name = n;
    if (name == "Unknown.Fn") {
      EXPECT_TRUE(has(cpp, def("Unknown_Fn") + " /* PsyQ Stub */ }"));
    } else {
      EXPECT_TRUE(has(cpp, def(name) + " ps1::psyq::hle_" + name + "(ctx); }")) << name;
    }
  }
  EXPECT_TRUE(has(cpp, "recomp_func_table[0x80050000] = VSync;"));
}

TEST_F(RecompCli, HleFunctionTakesPrecedenceOverStubAtSameAddress) {
  auto r = recompile(header() + func("leaf", "0x80010020", 12) +
                     "[[hle_functions]]\naddress = \"0x80060000\"\nhle = true\n"
                     "name = \"libgpu_DrawSync\"\n\n"
                     "[[stubs]]\nname = \"DrawSync\"\naddress = \"0x80060000\"\n");
  ASSERT_EQ(r.code, 0) << r.out;
  auto cpp = output();
  EXPECT_TRUE(has(cpp, "recomp_func_table[0x80060000] = func_80060000;"));
  EXPECT_FALSE(has(cpp, def("DrawSync")));
}

TEST_F(RecompCli, JumpTableEntryCountComesFromBoundsCheck) {
  // switch (a0) over 3 cases: SLTIU bound = 3 limits the table read even
  // though more KSEG0 words follow it in memory.
  constexpr uint32_t f = kBase + 0x40, table = kBase + 0x100;
  std::vector<uint32_t> code = twoFunctionCode();
  code.resize(0x40 / 4, kNop);
  std::vector<uint32_t> sw = {
      0x2C810003,                          // sltiu at, a0, 3
      0x1020000B,                          // beqz  at, default (+11)
      0x00044080,                          // sll   t0, a0, 2
      0x3C090000 | (table >> 16),          // lui   t1, hi(table)
      0x25290000 | (table & 0xFFFF),       // addiu t1, t1, lo(table)
      0x01284821,                          // addu  t1, t1, t0
      0x8D2A0000,                          // lw    t2, 0(t1)
      0x01400008,                          // jr    t2
      kNop,
      kJrRa, kNop,                         // case 0
      kJrRa, kNop,                         // case 1
      kJrRa, kNop,                         // case 2 / default
  };
  code.insert(code.end(), sw.begin(), sw.end());
  code.resize((table - kBase) / 4, kNop);
  for (uint32_t t : {f + 36, f + 44, f + 52, f + 52, f + 52})
    code.push_back(t);
  writeBytes(exe, makePsxExe(kBase, kBase, code));

  auto r = recompile(header() + func("sw", "0x80010040", 60));
  ASSERT_EQ(r.code, 0) << r.out;
  EXPECT_TRUE(has(r.out, "[jump table] func 0x80010040+28: 3 targets (table entries detected)"));
}

TEST_F(RecompCli, OverlayFromPsxExeAutoDetectsPrologues) {
  writeBytes(dir / "ovl.exe", makePsxExe(0x80100000, 0x80100000, twoFunctionCode()));
  auto r = recompile(header() + func("leaf", "0x80010020", 12) +
                     "[[overlays]]\nname = \"STAGE\"\nbinary_path = \"ovl.exe\"\n"
                     "ram_base = 0\nsize = 0\n");
  ASSERT_EQ(r.code, 0) << r.out;
  auto cpp = output();
  // The PS-X EXE header's t_addr overrides ram_base; one ADDIU $sp,-N prologue.
  EXPECT_TRUE(has(r.out, "[STAGE] Auto-detected 1 functions"));
  EXPECT_TRUE(has(cpp, def("overlay_STAGE__80100000")));
  EXPECT_TRUE(has(cpp, "{ \"STAGE\", 0x80100000, 44, 0 },"));
  EXPECT_TRUE(has(cpp, "recomp_func_table[0x80100000] = overlay_STAGE__80100000;"));
}

TEST_F(RecompCli, OverlayWithoutBinaryEmitsStubsForListedFunctions) {
  auto r = recompile(header() + func("leaf", "0x80010020", 12) +
                     "[[overlays]]\nname = \"MENU\"\nram_base = 0x80200000\nsize = 64\n"
                     "functions = [0x80200010, 0x80200000]\n");
  ASSERT_EQ(r.code, 0) << r.out;
  auto cpp = output();
  EXPECT_TRUE(has(cpp, def("overlay_MENU__80200000") + " /* Overlay stub"));
  EXPECT_TRUE(has(cpp, def("overlay_MENU__80200010") + " /* Overlay stub"));
  EXPECT_TRUE(has(cpp, "{ \"MENU\", 0x80200000, 64, 0 },"));
}

TEST_F(RecompCli, OverlayWithUnreadableBinaryIsSkipped) {
  auto r = recompile(header() + func("leaf", "0x80010020", 12) +
                     "[[overlays]]\nname = \"GONE\"\nbinary_path = \"missing.bin\"\n"
                     "ram_base = 0x80200000\nsize = 64\n");
  ASSERT_EQ(r.code, 0) << r.out;
  EXPECT_TRUE(has(r.out, "Warning: Could not open overlay binary"));
  EXPECT_FALSE(has(output(), "GONE"));
}

TEST_F(RecompCli, EmittedSourceCompilesAgainstRuntimeHeaders) {
  auto r = recompile(header() + func("main", "0x80010000", 32) +
                     func("leaf", "0x80010020", 12) +
                     "[[hle_functions]]\naddress = \"0x80030000\"\nhle = true\n"
                     "name = \"libetc_VSync\"\n\n"
                     "[[stubs]]\nname = \"VSync\"\naddress = \"0x80050000\"\n");
  ASSERT_EQ(r.code, 0) << r.out;
  const fs::path src = fs::path(PS1RECOMP_TEST_ROMS).parent_path();
  auto cc = run("c++ -std=c++20 -fsyntax-only -I" + q((src / "ps1Runtime/include").string()) +
                " -I" + q((src / "third_party/fmt/include").string()) + " " + q(out.string()));
  EXPECT_EQ(cc.code, 0) << cc.out;
}

} // namespace
