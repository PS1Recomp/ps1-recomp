#include "runtime/cdrom/bin_reader.h"
#include "runtime/cdrom/iso9660.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Unique per process and per call, so parallel ctest and --gtest_repeat
// never share an image file.
fs::path uniqueImagePath(const char *tag) {
  static int counter = 0;
  return fs::temp_directory_path() /
         (std::string("ps1_iso_") + tag + "_" + std::to_string(getpid()) +
          "_" + std::to_string(counter++) + ".bin");
}

// A raw (2352-byte sector) image whose user data starts at +24 (Mode 2
// Form 1), built sector by sector.
struct RawImage {
  std::vector<uint8_t> bytes;
  explicit RawImage(uint32_t sectors)
      : bytes(sectors * ps1::cdrom::SECTOR_SIZE_RAW, 0) {}

  uint8_t *data(uint32_t lba) {
    return &bytes[lba * ps1::cdrom::SECTOR_SIZE_RAW + 24];
  }
  // ECMA-119 volume descriptor: type, "CD001", root record at +156.
  void volumeDescriptor(uint32_t lba, uint8_t type, uint32_t rootLba,
                        uint32_t rootLen) {
    uint8_t *d = data(lba);
    d[0] = type;
    std::memcpy(d + 1, "CD001", 5);
    std::memcpy(d + 156 + 2, &rootLba, 4);
    std::memcpy(d + 156 + 10, &rootLen, 4);
  }
  // ECMA-119 directory record (len, extent LSB @2, size LSB @10, flags @25,
  // name len @32, name @33). Returns the offset just past the record.
  size_t record(uint32_t lba, size_t off, const std::string &name,
                uint32_t extent, uint32_t size, bool dir = false) {
    uint8_t *d = data(lba) + off;
    const uint8_t len = static_cast<uint8_t>(33 + name.size() + 1);
    d[0] = len;
    std::memcpy(d + 2, &extent, 4);
    std::memcpy(d + 10, &size, 4);
    d[25] = dir ? 0x02 : 0x00;
    d[32] = static_cast<uint8_t>(name.size());
    std::memcpy(d + 33, name.data(), name.size());
    return off + len;
  }
  void write(const fs::path &p) const {
    std::ofstream(p, std::ios::binary)
        .write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  }
};

class Iso9660ImageTest : public ::testing::Test {
protected:
  fs::path path = uniqueImagePath("img");
  ps1::cdrom::BinReader reader;
  void TearDown() override { fs::remove(path); }
  void load(const RawImage &img) {
    img.write(path);
    ASSERT_TRUE(reader.open(path));
  }
};

} // namespace

class Iso9660Test : public ::testing::Test {
protected:
  void SetUp() override {
    testBinPath = uniqueImagePath("fixture");
    std::ofstream off(testBinPath, std::ios::binary);

    // We prepare a dummy file 20 sectors long
    std::vector<uint8_t> dummyData(ps1::cdrom::SECTOR_SIZE_RAW * 20, 0);

    // Write "CD001" at sector 16 payload
    size_t pvdOffset = 16 * ps1::cdrom::SECTOR_SIZE_RAW + 24;
    dummyData[pvdOffset + 0] = 1; // Type = PVD
    std::memcpy(&dummyData[pvdOffset + 1], "CD001", 5);

    // Write Root Directory Record at PVD offset 156
    uint32_t rootLba = 17;
    uint32_t rootLen = 2048;
    std::memcpy(&dummyData[pvdOffset + 156 + 2], &rootLba, 4);  // LBA (LSB)
    std::memcpy(&dummyData[pvdOffset + 156 + 10], &rootLen, 4); // Length (LSB)

    // Setup Root Directory at sector 17
    size_t rootOffset = 17 * ps1::cdrom::SECTOR_SIZE_RAW + 24;

    // Record 1: SYSTEM.CNF
    uint8_t rec1_len = 34 + 10;
    dummyData[rootOffset + 0] = rec1_len;
    uint32_t sysCnfLba = 18;
    uint32_t sysCnfLen = 123;
    std::memcpy(&dummyData[rootOffset + 2], &sysCnfLba, 4);
    std::memcpy(&dummyData[rootOffset + 10], &sysCnfLen, 4);
    dummyData[rootOffset + 25] = 0x00; // File
    dummyData[rootOffset + 32] = 10;   // Name len
    std::memcpy(&dummyData[rootOffset + 33], "SYSTEM.CNF", 10);

    // Next record offset
    size_t rootOffset2 = rootOffset + rec1_len;

    // Record 2: DIRECTORY
    uint8_t rec2_len = 34 + 3;
    dummyData[rootOffset2 + 0] = rec2_len;
    uint32_t dirLba = 19;
    uint32_t dirLen = 2048;
    std::memcpy(&dummyData[rootOffset2 + 2], &dirLba, 4);
    std::memcpy(&dummyData[rootOffset2 + 10], &dirLen, 4);
    dummyData[rootOffset2 + 25] = 0x02; // Directory
    dummyData[rootOffset2 + 32] = 3;    // Name len
    std::memcpy(&dummyData[rootOffset2 + 33], "DIR", 3);

    // EOF record marker
    dummyData[rootOffset2 + rec2_len] = 0;

    off.write(reinterpret_cast<const char *>(dummyData.data()),
              dummyData.size());
  }

  void TearDown() override {
    if (fs::exists(testBinPath)) {
      fs::remove(testBinPath);
    }
  }

  fs::path testBinPath;
};

TEST_F(Iso9660Test, InitializeAndReadPVD) {
  ps1::cdrom::BinReader reader;
  EXPECT_TRUE(reader.open(testBinPath));

  ps1::cdrom::Iso9660Parser parser(reader);
  EXPECT_TRUE(parser.initialize());

  EXPECT_EQ(parser.getRootLba(), 17);
  EXPECT_EQ(parser.getRootLength(), 2048);
}

TEST_F(Iso9660Test, FindFileRoot) {
  ps1::cdrom::BinReader reader;
  EXPECT_TRUE(reader.open(testBinPath));

  ps1::cdrom::Iso9660Parser parser(reader);
  EXPECT_TRUE(parser.initialize());

  auto file = parser.findFile("SYSTEM.CNF");
  ASSERT_TRUE(file.has_value());
  EXPECT_EQ(file->name, "SYSTEM.CNF");
  EXPECT_EQ(file->extent_lba, 18);
  EXPECT_EQ(file->data_length, 123);
  EXPECT_FALSE(file->is_directory);
}

TEST_F(Iso9660Test, FindDirectory) {
  ps1::cdrom::BinReader reader;
  EXPECT_TRUE(reader.open(testBinPath));

  ps1::cdrom::Iso9660Parser parser(reader);
  EXPECT_TRUE(parser.initialize());

  auto file = parser.findFile("DIR");
  ASSERT_TRUE(file.has_value());
  EXPECT_EQ(file->name, "DIR");
  EXPECT_EQ(file->extent_lba, 19);
  EXPECT_EQ(file->data_length, 2048);
  EXPECT_TRUE(file->is_directory);
}

TEST_F(Iso9660ImageTest, InitializeFailsWhenNoVolumeDescriptorHasTheCd001Id) {
  load(RawImage(40));
  ps1::cdrom::Iso9660Parser parser(reader);

  bool ok = parser.initialize();

  EXPECT_FALSE(ok);
  EXPECT_EQ(parser.getRootLba(), 0u);
}

TEST_F(Iso9660ImageTest, InitializeFailsWhenTheImageEndsBeforeTheDescriptors) {
  load(RawImage(16)); // sector 16 does not exist
  ps1::cdrom::Iso9660Parser parser(reader);

  EXPECT_FALSE(parser.initialize());
}

TEST_F(Iso9660ImageTest, InitializeSkipsNonPrimaryDescriptors) {
  RawImage img(40);
  img.volumeDescriptor(16, 2, 99, 2048); // supplementary: ignored
  img.volumeDescriptor(17, 1, 20, 4096); // primary
  load(img);
  ps1::cdrom::Iso9660Parser parser(reader);

  ASSERT_TRUE(parser.initialize());

  EXPECT_EQ(parser.getRootLba(), 20u);
  EXPECT_EQ(parser.getRootLength(), 4096u);
}

TEST_F(Iso9660ImageTest, FindFileBeforeInitializeFindsNothing) {
  load(RawImage(20));
  ps1::cdrom::Iso9660Parser parser(reader);

  EXPECT_FALSE(parser.findFile("SYSTEM.CNF").has_value());
}

TEST_F(Iso9660ImageTest, FindFileWalksSubdirectoriesWithEitherSeparator) {
  RawImage img(24);
  img.volumeDescriptor(16, 1, 17, 2048);
  img.record(17, 0, "DATA", 18, 2048, /*dir=*/true);
  img.record(18, 0, "LEVEL1.NSF;1", 21, 65536);
  load(img);
  ps1::cdrom::Iso9660Parser parser(reader);
  ASSERT_TRUE(parser.initialize());

  auto slash = parser.findFile("/DATA/LEVEL1.NSF");
  auto backslash = parser.findFile("\\DATA\\LEVEL1.NSF");

  ASSERT_TRUE(slash.has_value());
  EXPECT_EQ(slash->name, "LEVEL1.NSF"); // ";1" version suffix stripped
  EXPECT_EQ(slash->extent_lba, 21u);
  EXPECT_EQ(slash->data_length, 65536u);
  ASSERT_TRUE(backslash.has_value());
  EXPECT_EQ(backslash->extent_lba, 21u);
}

TEST_F(Iso9660ImageTest, FindFileFailsWhenAPathComponentIsMissing) {
  RawImage img(24);
  img.volumeDescriptor(16, 1, 17, 2048);
  img.record(17, 0, "DATA", 18, 2048, /*dir=*/true);
  load(img);
  ps1::cdrom::Iso9660Parser parser(reader);
  ASSERT_TRUE(parser.initialize());

  EXPECT_FALSE(parser.findFile("DATA/MISSING.BIN").has_value());
  EXPECT_FALSE(parser.findFile("NOPE/LEVEL1.NSF").has_value());
}

TEST_F(Iso9660ImageTest, FindFileOfAnEmptyPathFindsNothing) {
  RawImage img(24);
  img.volumeDescriptor(16, 1, 17, 2048);
  img.record(17, 0, "DATA", 18, 2048, /*dir=*/true);
  load(img);
  ps1::cdrom::Iso9660Parser parser(reader);
  ASSERT_TRUE(parser.initialize());

  EXPECT_FALSE(parser.findFile("").has_value());
  EXPECT_FALSE(parser.findFile("/").has_value());
}

// ECMA-119 9.1.11: identifier 00h is the directory itself, 01h its parent.
TEST_F(Iso9660ImageTest, SelfAndParentRecordsAreNamedDotAndDotDot) {
  RawImage img(24);
  size_t off = img.record(17, 0, std::string(1, '\0'), 17, 2048, true);
  img.record(17, off, std::string(1, '\1'), 17, 2048, true);
  load(img);
  ps1::cdrom::Iso9660Parser parser(reader);

  auto entries = parser.readDirectory(17, 2048);

  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0].name, ".");
  EXPECT_EQ(entries[1].name, "..");
  EXPECT_TRUE(entries[0].is_directory);
}

TEST_F(Iso9660ImageTest, RecordRunningPastTheSectorEndStopsTheListing) {
  RawImage img(24);
  img.record(17, 0, "A.BIN", 30, 10);
  img.data(17)[2040] = 40; // a 40-byte record that would cross 2048
  load(img);
  ps1::cdrom::Iso9660Parser parser(reader);

  auto entries = parser.readDirectory(17, 2048);

  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].name, "A.BIN");
}

TEST_F(Iso9660ImageTest, DirectoryExtentPastTheImageEndKeepsTheSectorsRead) {
  RawImage img(18); // directory claims sectors 17 and 18; only 17 exists
  img.record(17, 0, "A.BIN", 30, 10);
  load(img);
  ps1::cdrom::Iso9660Parser parser(reader);

  auto entries = parser.readDirectory(17, 4096);

  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].name, "A.BIN");
}
