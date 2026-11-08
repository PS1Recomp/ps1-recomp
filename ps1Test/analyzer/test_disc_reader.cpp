// Tests: Disc image reader.
// Builds a tiny synthetic Mode 2 / 2352-byte BIN image (psx-spx "CDROM Disk
// Format": 12-byte sync, 4-byte header, 8-byte subheader, 2048 data bytes) with
// an ISO 9660 Primary Volume Descriptor at sector 16, and reads it back.

#include <gtest/gtest.h>
#include <unistd.h>
#include <ps1recomp/disc_reader.h>
#include <ps1recomp/overlay_scanner.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace ps1recomp;

namespace {

// Raw-sector image builder. Every sector gets a real sync/header/subheader so
// a reader that gets the 24-byte payload offset wrong reads non-zero garbage.
class BinImage {
public:
  explicit BinImage(uint32_t sectors) : m_raw(sectors * SECTOR_SIZE_RAW, 0xCC) {
    for (uint32_t lba = 0; lba < sectors; ++lba) {
      uint8_t *s = m_raw.data() + lba * SECTOR_SIZE_RAW;
      s[0] = 0x00;
      std::memset(s + 1, 0xFF, 10);
      s[11] = 0x00;
      uint32_t abs = lba + 150; // 2-second lead-in, header MSF is BCD
      auto bcd = [](uint32_t v) { return uint8_t(((v / 10) << 4) | (v % 10)); };
      s[12] = bcd(abs / (60 * 75));
      s[13] = bcd((abs / 75) % 60);
      s[14] = bcd(abs % 75);
      s[15] = 0x02;                          // Mode 2
      const uint8_t sub[4] = {0, 0, 0x08, 0}; // Form 1, data
      std::memcpy(s + 16, sub, 4);
      std::memcpy(s + 20, sub, 4);
      std::memset(s + 24, 0, SECTOR_SIZE_DATA);
    }
  }

  uint8_t *data(uint32_t lba) {
    return m_raw.data() + lba * SECTOR_SIZE_RAW + SECTOR_HEADER_SIZE;
  }

  void put(uint32_t lba, const std::vector<uint8_t> &bytes) {
    // Spread across consecutive sectors, 2048 payload bytes each.
    for (size_t i = 0; i < bytes.size(); ++i)
      data(lba + uint32_t(i / SECTOR_SIZE_DATA))[i % SECTOR_SIZE_DATA] = bytes[i];
  }

  void write(const std::string &path) const {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char *>(m_raw.data()),
            static_cast<std::streamsize>(m_raw.size()));
  }

private:
  std::vector<uint8_t> m_raw;
};

void put32Both(uint8_t *p, uint32_t v) { // ISO 9660 both-endian field
  for (int i = 0; i < 4; ++i) {
    p[i] = uint8_t(v >> (8 * i));
    p[7 - i] = uint8_t(v >> (8 * i));
  }
}

// ISO 9660 directory record; returns the record length written.
size_t dirRecord(uint8_t *out, const std::string &name, uint32_t lba,
                 uint32_t size, bool isDir) {
  size_t nameLen = name.size();
  size_t len = 33 + nameLen + ((nameLen % 2 == 0) ? 1 : 0); // pad to even
  std::memset(out, 0, len);
  out[0] = uint8_t(len);
  put32Both(out + 2, lba);
  put32Both(out + 10, size);
  out[25] = isDir ? 0x02 : 0x00;
  out[28] = 1; // volume sequence number
  out[32] = uint8_t(nameLen);
  std::memcpy(out + 33, name.data(), nameLen);
  return len;
}

struct Entry {
  std::string name;
  uint32_t lba;
  uint32_t size;
  bool isDir;
};

void writeDir(uint8_t *sector, uint32_t self, uint32_t parent,
              const std::vector<Entry> &entries) {
  size_t off = 0;
  off += dirRecord(sector + off, std::string(1, '\0'), self, 2048, true);
  off += dirRecord(sector + off, std::string(1, '\1'), parent, 2048, true);
  for (const auto &e : entries)
    off += dirRecord(sector + off, e.name, e.lba, e.size, e.isDir);
}

void writePvd(BinImage &img, uint32_t rootLba, uint32_t rootSize) {
  uint8_t *pvd = img.data(16);
  pvd[0] = 0x01;
  std::memcpy(pvd + 1, "CD001", 5);
  pvd[6] = 0x01;
  dirRecord(pvd + 156, std::string(1, '\0'), rootLba, rootSize, true);
  uint8_t *term = img.data(17); // Volume Descriptor Set Terminator
  term[0] = 0xFF;
  std::memcpy(term + 1, "CD001", 5);
}

// Little-endian MIPS words: prologue, a LUI/ORI pair, epilogue, repeated.
std::vector<uint8_t> overlayCode(size_t bytes) {
  const uint32_t pattern[] = {
      0x27BDFFE8, // addiu $sp, $sp, -24
      0xAFBF0010, // sw    $ra, 16($sp)
      0x3C028010, // lui   $v0, 0x8010
      0x34420800, // ori   $v0, $v0, 0x0800
      0x8FBF0010, // lw    $ra, 16($sp)
      0x27BD0018, // addiu $sp, $sp, 24
      0x03E00008, // jr    $ra
      0x00000000, // nop
  };
  std::vector<uint8_t> out(bytes);
  for (size_t i = 0; i < bytes / 4; ++i) {
    uint32_t w = pattern[i % 8];
    std::memcpy(out.data() + i * 4, &w, 4);
  }
  return out;
}

std::vector<uint8_t> bootExe() {
  std::vector<uint8_t> exe(3000);
  for (size_t i = 0; i < exe.size(); ++i)
    exe[i] = uint8_t(i * 7);
  std::memcpy(exe.data(), "PS-X EXE", 8);
  return exe;
}

constexpr uint32_t kSectors = 28;

// Root (LBA 18-19, two sectors, the second reached through zero padding):
//   SYSTEM.CNF;1 @22, SCUS_123.45;1 @23 (3000 bytes), DATA/ @21, LATE.TXT;1 @25
// DATA/ (LBA 21): OVL.BIN;1 @26 (512 bytes), MOVIE.STR;1 @27 (512 bytes)
BinImage buildStandardImage(const std::string &systemCnf) {
  BinImage img(kSectors);
  writePvd(img, 18, 2 * SECTOR_SIZE_DATA);
  std::vector<Entry> root = {
      {"SCUS_123.45;1", 23, 3000, false},
      {"DATA", 21, 2048, true},
  };
  if (!systemCnf.empty())
    root.insert(root.begin(),
                {"SYSTEM.CNF;1", 22, uint32_t(systemCnf.size()), false});
  writeDir(img.data(18), 18, 18, root);
  dirRecord(img.data(19), "LATE.TXT;1", 25, 5, false);
  writeDir(img.data(21), 21, 18,
           {{"OVL.BIN;1", 26, 512, false}, {"MOVIE.STR;1", 27, 512, false}});
  img.put(22, std::vector<uint8_t>(systemCnf.begin(), systemCnf.end()));
  img.put(23, bootExe());
  img.put(25, {'h', 'e', 'l', 'l', 'o'});
  img.put(26, overlayCode(512));
  img.put(27, overlayCode(512));
  return img;
}

const std::string kSystemCnf =
    "BOOT = cdrom:\\SCUS_123.45;1\r\nTCB = 4\r\nEVENT = 10\r\n";

class DiscReaderTest : public ::testing::Test {
protected:
  std::string path;

  void SetUp() override {
    // Unique path per test instance to avoid parallel-test races.
    const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
    path = (std::filesystem::temp_directory_path() /
            (std::string("ps1recomp_disc_") + info->test_suite_name() + "_" +
             info->name() + "_" + std::to_string(getpid()) + ".bin"))
               .string();
  }
  void TearDown() override { std::remove(path.c_str()); }

  void writeStandard(const std::string &cnf = kSystemCnf) {
    buildStandardImage(cnf).write(path);
  }
};

const DiscFile *byPath(const DiscReader &r, const std::string &p) {
  for (const auto &f : r.getFiles())
    if (f.path == p)
      return &f;
  return nullptr;
}

} // namespace

// Open / error paths

TEST_F(DiscReaderTest, OpenMissingFileFailsWithPathInError) {
  DiscReader reader;
  EXPECT_FALSE(reader.open(path + ".missing"));
  EXPECT_NE(reader.getError().find(path + ".missing"), std::string::npos);
}

TEST_F(DiscReaderTest, ParseWithoutOpenFails) {
  DiscReader reader;
  EXPECT_FALSE(reader.parseFilesystem());
  EXPECT_EQ(reader.getError(), "No BIN file opened");
}

TEST_F(DiscReaderTest, ReadSectorWithoutOpenFails) {
  DiscReader reader;
  uint8_t buf[SECTOR_SIZE_DATA];
  EXPECT_FALSE(reader.readSectorData(0, buf));
  EXPECT_TRUE(reader.readSectors(0, 2).empty());
}

TEST_F(DiscReaderTest, ImageShorterThanSector16HasNoVolumeDescriptor) {
  BinImage(10).write(path);
  DiscReader reader;
  ASSERT_TRUE(reader.open(path));
  EXPECT_FALSE(reader.parseFilesystem());
  EXPECT_NE(reader.getError().find("sector 16"), std::string::npos);
}

TEST_F(DiscReaderTest, RejectsVolumeDescriptorWithoutCD001) {
  BinImage img(kSectors);
  writePvd(img, 18, 2048);
  std::memcpy(img.data(16) + 1, "CD002", 5);
  img.write(path);
  DiscReader reader;
  ASSERT_TRUE(reader.open(path));
  EXPECT_FALSE(reader.parseFilesystem());
  EXPECT_NE(reader.getError().find("CD001"), std::string::npos);
}

TEST_F(DiscReaderTest, RejectsDescriptorThatIsNotPrimary) {
  // Type 0xFF is the set terminator, not a Primary Volume Descriptor (type 1).
  BinImage img(kSectors);
  writePvd(img, 18, 2048);
  img.data(16)[0] = 0xFF;
  img.write(path);
  DiscReader reader;
  ASSERT_TRUE(reader.open(path));
  EXPECT_FALSE(reader.parseFilesystem());
}

TEST_F(DiscReaderTest, RootDirectoryOutsideImageFails) {
  BinImage img(kSectors);
  writePvd(img, 500, 2048);
  img.write(path);
  DiscReader reader;
  ASSERT_TRUE(reader.open(path));
  EXPECT_FALSE(reader.parseFilesystem());
  EXPECT_NE(reader.getError().find("LBA 500"), std::string::npos);
}

// Sector payload extraction

TEST_F(DiscReaderTest, SectorDataSkipsSyncHeaderAndSubheader) {
  BinImage img(kSectors);
  img.data(3)[0] = 0x5A;
  img.data(3)[2047] = 0xA5;
  img.write(path);
  DiscReader reader;
  ASSERT_TRUE(reader.open(path));
  uint8_t buf[SECTOR_SIZE_DATA];
  ASSERT_TRUE(reader.readSectorData(3, buf));
  EXPECT_EQ(buf[0], 0x5A);
  EXPECT_EQ(buf[2047], 0xA5);
}

TEST_F(DiscReaderTest, ReadSectorsStopsAtEndOfImage) {
  BinImage(kSectors).write(path);
  DiscReader reader;
  ASSERT_TRUE(reader.open(path));
  EXPECT_EQ(reader.readSectors(kSectors - 2, 5).size(), 2 * SECTOR_SIZE_DATA);
  uint8_t buf[SECTOR_SIZE_DATA];
  EXPECT_FALSE(reader.readSectorData(kSectors, buf));
}

// ISO 9660 directory parsing

TEST_F(DiscReaderTest, ParsesRootEntriesWithVersionSuffixStripped) {
  writeStandard();
  DiscReader reader;
  ASSERT_TRUE(reader.open(path));
  ASSERT_TRUE(reader.parseFilesystem()) << reader.getError();

  const DiscFile *exe = byPath(reader, "/SCUS_123.45");
  ASSERT_NE(exe, nullptr);
  EXPECT_EQ(exe->name, "SCUS_123.45");
  EXPECT_EQ(exe->lba, 23u);
  EXPECT_EQ(exe->size, 3000u);
  EXPECT_FALSE(exe->isDirectory);

  const DiscFile *dir = byPath(reader, "/DATA");
  ASSERT_NE(dir, nullptr);
  EXPECT_TRUE(dir->isDirectory);
  EXPECT_EQ(dir->lba, 21u);
}

TEST_F(DiscReaderTest, SkipsSelfAndParentEntries) {
  writeStandard();
  DiscReader reader;
  ASSERT_TRUE(reader.open(path));
  ASSERT_TRUE(reader.parseFilesystem());
  // root: SYSTEM.CNF, SCUS, DATA, LATE.TXT; DATA: OVL.BIN, MOVIE.STR
  EXPECT_EQ(reader.getFiles().size(), 6u);
  for (const auto &f : reader.getFiles()) {
    ASSERT_FALSE(f.name.empty());
    EXPECT_GT(static_cast<unsigned char>(f.name[0]), 0x01u) << f.path;
  }
}

TEST_F(DiscReaderTest, RecursesIntoSubdirectoriesWithFullPaths) {
  writeStandard();
  DiscReader reader;
  ASSERT_TRUE(reader.open(path));
  ASSERT_TRUE(reader.parseFilesystem());
  const DiscFile *ovl = byPath(reader, "/DATA/OVL.BIN");
  ASSERT_NE(ovl, nullptr);
  EXPECT_EQ(ovl->lba, 26u);
  EXPECT_EQ(ovl->size, 512u);
}

TEST_F(DiscReaderTest, ZeroPaddingContinuesInNextDirectorySector) {
  // ISO 9660: records never straddle sectors; the remainder is zero-filled.
  writeStandard();
  DiscReader reader;
  ASSERT_TRUE(reader.open(path));
  ASSERT_TRUE(reader.parseFilesystem());
  const DiscFile *late = byPath(reader, "/LATE.TXT");
  ASSERT_NE(late, nullptr);
  EXPECT_EQ(late->lba, 25u);
  EXPECT_EQ(late->size, 5u);
}

// File lookup and reads

TEST_F(DiscReaderTest, FindFileIsCaseInsensitiveAndIgnoresVersion) {
  writeStandard();
  DiscReader reader;
  ASSERT_TRUE(reader.open(path));
  ASSERT_TRUE(reader.parseFilesystem());
  const DiscFile *f = reader.findFile("scus_123.45;1");
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->path, "/SCUS_123.45");
  EXPECT_EQ(reader.findFile("NOPE.BIN"), nullptr);
}

TEST_F(DiscReaderTest, ReadFileSpansSectorsAndTrimsToSize) {
  writeStandard();
  DiscReader reader;
  ASSERT_TRUE(reader.open(path));
  ASSERT_TRUE(reader.parseFilesystem());
  const DiscFile *exe = reader.findFile("SCUS_123.45");
  ASSERT_NE(exe, nullptr);
  auto data = reader.readFile(*exe);
  EXPECT_EQ(data, bootExe());
}

TEST_F(DiscReaderTest, ReadFileOfSmallFileReturnsExactBytes) {
  writeStandard();
  DiscReader reader;
  ASSERT_TRUE(reader.open(path));
  ASSERT_TRUE(reader.parseFilesystem());
  const DiscFile *late = reader.findFile("LATE.TXT");
  ASSERT_NE(late, nullptr);
  auto data = reader.readFile(*late);
  EXPECT_EQ(std::string(data.begin(), data.end()), "hello");
}

// SYSTEM.CNF boot executable (psx-spx "CDROM File Playstation EXE and
// SYSTEM.CNF")

TEST_F(DiscReaderTest, BootFilenameComesFromSystemCnf) {
  writeStandard();
  DiscReader reader;
  ASSERT_TRUE(reader.open(path));
  ASSERT_TRUE(reader.parseFilesystem());
  EXPECT_EQ(reader.getBootFilename(), "SCUS_123.45");
}

TEST_F(DiscReaderTest, BootFilenameIgnoresCommandLineArgument) {
  writeStandard("BOOT = cdrom:\\SCUS_123.45;1 arg\r\n");
  DiscReader reader;
  ASSERT_TRUE(reader.open(path));
  ASSERT_TRUE(reader.parseFilesystem());
  EXPECT_EQ(reader.getBootFilename(), "SCUS_123.45");
}

TEST_F(DiscReaderTest, BootFilenameWithoutVersionHasTrailingSpacesTrimmed) {
  writeStandard("BOOT = cdrom:\\SCUS_123.45  \r\n");
  DiscReader reader;
  ASSERT_TRUE(reader.open(path));
  ASSERT_TRUE(reader.parseFilesystem());
  EXPECT_EQ(reader.getBootFilename(), "SCUS_123.45");
}

TEST_F(DiscReaderTest, BootLineWithoutPathSeparatorLeavesBootEmpty) {
  writeStandard("BOOT = SCUS_123.45\r\n");
  DiscReader reader;
  ASSERT_TRUE(reader.open(path));
  EXPECT_TRUE(reader.parseFilesystem());
  EXPECT_EQ(reader.getBootFilename(), "");
}

TEST_F(DiscReaderTest, SystemCnfWithoutBootLineLeavesBootEmpty) {
  writeStandard("TCB = 4\r\nEVENT = 10\r\n");
  DiscReader reader;
  ASSERT_TRUE(reader.open(path));
  EXPECT_TRUE(reader.parseFilesystem());
  EXPECT_EQ(reader.getBootFilename(), "");
}

TEST_F(DiscReaderTest, MissingSystemCnfFallsBackToPsxExe) {
  // psx-spx: PSX.EXE is the default when SYSTEM.CNF doesn't exist.
  BinImage img(kSectors);
  writePvd(img, 18, 2048);
  writeDir(img.data(18), 18, 18, {{"PSX.EXE;1", 23, 3000, false}});
  img.write(path);
  DiscReader reader;
  ASSERT_TRUE(reader.open(path));
  ASSERT_TRUE(reader.parseFilesystem());
  EXPECT_EQ(reader.getBootFilename(), "PSX.EXE");
}

TEST_F(DiscReaderTest, NoSystemCnfAndNoPsxExeLeavesBootEmpty) {
  writeStandard("");
  DiscReader reader;
  ASSERT_TRUE(reader.open(path));
  EXPECT_TRUE(reader.parseFilesystem());
  EXPECT_EQ(reader.getBootFilename(), "");
}

TEST_F(DiscReaderTest, ReopenClearsPreviousFileList) {
  writeStandard();
  DiscReader reader;
  ASSERT_TRUE(reader.open(path));
  ASSERT_TRUE(reader.parseFilesystem());
  ASSERT_FALSE(reader.getFiles().empty());
  EXPECT_FALSE(reader.open(path + ".missing"));
  EXPECT_TRUE(reader.getFiles().empty());
  EXPECT_EQ(reader.getBootFilename(), "");
}

// OverlayScanner::scanDisc over a real DiscReader

TEST_F(DiscReaderTest, ScanDiscFindsOverlayAndSkipsBootDirsAndData) {
  writeStandard();
  DiscReader reader;
  ASSERT_TRUE(reader.open(path));
  ASSERT_TRUE(reader.parseFilesystem());

  OverlayScanOptions opts;
  opts.bootExeName = "scus_123.45;1"; // compared case-insensitively
  auto results = OverlayScanner().scanDisc(reader, opts);

  // SCUS (boot exe) and MOVIE.STR (video extension) are excluded; LATE.TXT
  // and SYSTEM.CNF are under minCodeSize; DATA is a directory.
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].discPath, "/DATA/OVL.BIN");
  EXPECT_EQ(results[0].lba, 26u);
  EXPECT_EQ(results[0].ramBase, 0x80100800u);
  EXPECT_FALSE(results[0].functions.empty());
}

TEST_F(DiscReaderTest, ScanDiscWithoutBootNameReportsBootExe) {
  writeStandard();
  DiscReader reader;
  ASSERT_TRUE(reader.open(path));
  ASSERT_TRUE(reader.parseFilesystem());
  auto results = OverlayScanner().scanDisc(reader, {});
  bool sawExe = false;
  for (const auto &c : results)
    sawExe |= (c.discPath == "/SCUS_123.45" && c.hasPsxExeHeader);
  EXPECT_TRUE(sawExe);
}

TEST_F(DiscReaderTest, ScanDiscAllFilesIncludesDataExtensions) {
  writeStandard();
  DiscReader reader;
  ASSERT_TRUE(reader.open(path));
  ASSERT_TRUE(reader.parseFilesystem());
  OverlayScanOptions opts;
  opts.bootExeName = "SCUS_123.45";
  opts.scanAllFiles = true;
  auto results = OverlayScanner().scanDisc(reader, opts);
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[1].discPath, "/DATA/MOVIE.STR");
}

TEST_F(DiscReaderTest, ScanDiscSkipsFilesAboveMaxSize) {
  writeStandard();
  DiscReader reader;
  ASSERT_TRUE(reader.open(path));
  ASSERT_TRUE(reader.parseFilesystem());
  OverlayScanOptions opts;
  opts.bootExeName = "SCUS_123.45";
  opts.maxFileSize = 511;
  EXPECT_TRUE(OverlayScanner().scanDisc(reader, opts).empty());
}
