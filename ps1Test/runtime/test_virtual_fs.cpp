#include "runtime/cdrom/virtual_fs.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

class VirtualFsTest : public ::testing::Test {
protected:
  void SetUp() override {
    // One directory per test: a CUE names its BIN relative to itself.
    static int counter = 0;
    dir = fs::temp_directory_path() /
          ("ps1_vfs_" + std::to_string(getpid()) + "_" +
           std::to_string(counter++));
    fs::create_directories(dir);
    testBinPath = dir / "disc.bin";
    std::ofstream off(testBinPath, std::ios::binary);

    std::vector<uint8_t> dummyData(ps1::cdrom::SECTOR_SIZE_RAW * 20, 0);

    // Standard ISO9660 PVD
    size_t pvdOffset = 16 * ps1::cdrom::SECTOR_SIZE_RAW + 24;
    dummyData[pvdOffset + 0] = 1;
    std::memcpy(&dummyData[pvdOffset + 1], "CD001", 5);

    uint32_t rootLba = 17;
    uint32_t rootLen = 2048;
    std::memcpy(&dummyData[pvdOffset + 156 + 2], &rootLba, 4);
    std::memcpy(&dummyData[pvdOffset + 156 + 10], &rootLen, 4);

    // Setup Root Directory at sector 17
    size_t rootOffset = 17 * ps1::cdrom::SECTOR_SIZE_RAW + 24;

    // SYSTEM.CNF record
    std::string sysCnfBody = "BOOT = cdrom:\\SLUS_123.45;1\r\nTCB = 4\r\n";
    uint8_t rec1_len = 34 + 10;
    dummyData[rootOffset + 0] = rec1_len;
    uint32_t sysCnfLba = 18;
    uint32_t sysCnfLen = sysCnfBody.length();
    std::memcpy(&dummyData[rootOffset + 2], &sysCnfLba, 4);
    std::memcpy(&dummyData[rootOffset + 10], &sysCnfLen, 4);
    dummyData[rootOffset + 25] = 0x00;
    dummyData[rootOffset + 32] = 10;
    std::memcpy(&dummyData[rootOffset + 33], "SYSTEM.CNF", 10);

    // SLUS_123.45 record
    size_t rootOffset2 = rootOffset + rec1_len;
    uint8_t rec2_len = 34 + 11;
    dummyData[rootOffset2 + 0] = rec2_len;
    uint32_t slusLba = 19;
    uint32_t slusLen = 8; // dummy exe length
    std::memcpy(&dummyData[rootOffset2 + 2], &slusLba, 4);
    std::memcpy(&dummyData[rootOffset2 + 10], &slusLen, 4);
    dummyData[rootOffset2 + 25] = 0x00;
    dummyData[rootOffset2 + 32] = 11;
    std::memcpy(&dummyData[rootOffset2 + 33], "SLUS_123.45", 11);

    // Write SYSTEM.CNF body at sector 18
    size_t sysCnfOffset = 18 * ps1::cdrom::SECTOR_SIZE_RAW + 24;
    std::memcpy(&dummyData[sysCnfOffset], sysCnfBody.data(),
                sysCnfBody.length());

    // Write SLUS_123.45 body at sector 19
    size_t slusOffset = 19 * ps1::cdrom::SECTOR_SIZE_RAW + 24;
    std::memcpy(&dummyData[slusOffset], "PS-X EXE", 8);

    off.write(reinterpret_cast<const char *>(dummyData.data()),
              dummyData.size());
  }

  void TearDown() override { fs::remove_all(dir); }

  fs::path writeText(const std::string &name, const std::string &text) {
    fs::path p = dir / name;
    std::ofstream(p) << text;
    return p;
  }
  // A raw image with no ISO9660 volume descriptor.
  fs::path writeBlankImage(const std::string &name, uint32_t sectors) {
    fs::path p = dir / name;
    std::vector<char> zeros(ps1::cdrom::SECTOR_SIZE_RAW * sectors, 0);
    std::ofstream(p, std::ios::binary).write(zeros.data(), zeros.size());
    return p;
  }

  fs::path dir;
  fs::path testBinPath;
};

TEST_F(VirtualFsTest, LoadAndReadInfo) {
  ps1::cdrom::VirtualFs vfs;
  EXPECT_TRUE(vfs.loadDisc(testBinPath));

  auto bootPath = vfs.getBootPath();
  ASSERT_TRUE(bootPath.has_value());
  EXPECT_EQ(bootPath.value(), "SLUS_123.45");

  auto exeData = vfs.readFile(bootPath.value());
  ASSERT_TRUE(exeData.has_value());
  EXPECT_EQ(exeData->size(), 8);
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(exeData->data()), 8),
            "PS-X EXE");
}

// Raw images are accepted under the extensions a dump tool writes.
class VirtualFsExtensionTest
    : public VirtualFsTest,
      public ::testing::WithParamInterface<const char *> {};

TEST_P(VirtualFsExtensionTest, RawImageMountsTheFilesystem) {
  fs::path image = dir / (std::string("disc") + GetParam());
  fs::rename(testBinPath, image);
  ps1::cdrom::VirtualFs vfs;

  ASSERT_TRUE(vfs.loadDisc(image));

  EXPECT_EQ(vfs.getBootPath().value_or(""), "SLUS_123.45");
}

INSTANTIATE_TEST_SUITE_P(RawExtensions, VirtualFsExtensionTest,
                         ::testing::Values(".bin", ".BIN", ".iso", ".ISO"));

TEST_F(VirtualFsTest, UnsupportedImageFormatIsRejected) {
  fs::path chd = writeText("disc.chd", "MComprHD");
  ps1::cdrom::VirtualFs vfs;

  EXPECT_FALSE(vfs.loadDisc(chd));
  EXPECT_FALSE(vfs.readSector(0).has_value());
}

TEST_F(VirtualFsTest, MissingRawImageFailsToLoad) {
  ps1::cdrom::VirtualFs vfs;

  EXPECT_FALSE(vfs.loadDisc(dir / "absent.bin"));
}

TEST_F(VirtualFsTest, NothingIsReadableBeforeADiscIsLoaded) {
  ps1::cdrom::VirtualFs vfs;

  EXPECT_FALSE(vfs.readSector(16).has_value());
  EXPECT_FALSE(vfs.readFile("SYSTEM.CNF").has_value());
  EXPECT_FALSE(vfs.getBootPath().has_value());
}

TEST_F(VirtualFsTest, CueSheetMountsTheBinOfItsFirstTrack) {
  fs::path cue = writeText("game.cue", "FILE \"disc.bin\" BINARY\n"
                                       "  TRACK 01 MODE2/2352\n"
                                       "    INDEX 01 00:00:00\n");
  ps1::cdrom::VirtualFs vfs;

  ASSERT_TRUE(vfs.loadDisc(cue));

  EXPECT_EQ(vfs.getBootPath().value_or(""), "SLUS_123.45");
  auto exe = vfs.readFile("SLUS_123.45");
  ASSERT_TRUE(exe.has_value());
  EXPECT_EQ(std::string(exe->begin(), exe->end()), "PS-X EXE");
}

TEST_F(VirtualFsTest, UppercaseCueExtensionIsRecognised) {
  fs::path cue = writeText("GAME.CUE", "FILE \"disc.bin\" BINARY\n"
                                       "  TRACK 01 MODE2/2352\n");
  ps1::cdrom::VirtualFs vfs;

  EXPECT_TRUE(vfs.loadDisc(cue));
}

TEST_F(VirtualFsTest, CueSheetWithoutTracksFailsToLoad) {
  fs::path cue = writeText("empty.cue", "REM nothing here\n");
  ps1::cdrom::VirtualFs vfs;

  EXPECT_FALSE(vfs.loadDisc(cue));
}

TEST_F(VirtualFsTest, CueSheetNamingAMissingBinFailsToLoad) {
  fs::path cue = writeText("game.cue", "FILE \"gone.bin\" BINARY\n"
                                       "  TRACK 01 MODE2/2352\n");
  ps1::cdrom::VirtualFs vfs;

  EXPECT_FALSE(vfs.loadDisc(cue));
}

// An audio CD (or any image without ISO9660) still mounts for raw sector
// reads; only the file layer is absent.
TEST_F(VirtualFsTest, CueOfADiscWithoutFilesystemReadsSectorsButNoFiles) {
  writeBlankImage("audio.bin", 20);
  fs::path cue = writeText("audio.cue", "FILE \"audio.bin\" BINARY\n"
                                        "  TRACK 01 AUDIO\n");
  ps1::cdrom::VirtualFs vfs;

  ASSERT_TRUE(vfs.loadDisc(cue));

  EXPECT_TRUE(vfs.readSector(19).has_value());
  EXPECT_FALSE(vfs.getBootPath().has_value());
  EXPECT_FALSE(vfs.readFile("SYSTEM.CNF").has_value());
}

TEST_F(VirtualFsTest, ReadingAMissingFileFindsNothing) {
  ps1::cdrom::VirtualFs vfs;
  ASSERT_TRUE(vfs.loadDisc(testBinPath));

  EXPECT_FALSE(vfs.readFile("NOT_THERE.DAT").has_value());
}

TEST_F(VirtualFsTest, FileWhoseExtentRunsPastTheImageEndCannotBeRead) {
  // Truncate the image so SLUS_123.45's sector (19) is gone.
  fs::resize_file(testBinPath, 19 * ps1::cdrom::SECTOR_SIZE_RAW);
  ps1::cdrom::VirtualFs vfs;
  ASSERT_TRUE(vfs.loadDisc(testBinPath));

  EXPECT_FALSE(vfs.readFile("SLUS_123.45").has_value());
}

TEST_F(VirtualFsTest, SwapDiscMountsTheNewImageAndTracksTheDiscIndex) {
  fs::path blank = writeBlankImage("disc2.bin", 20);
  ps1::cdrom::VirtualFs vfs;
  ASSERT_TRUE(vfs.loadDisc(testBinPath));

  ASSERT_TRUE(vfs.swapDisc(blank));
  EXPECT_EQ(vfs.getDiscCount(), 1u);
  EXPECT_EQ(vfs.getCurrentDisc(), 0u);
  EXPECT_FALSE(vfs.getBootPath().has_value()); // disc 2 has no filesystem

  ASSERT_TRUE(vfs.swapDisc(testBinPath));
  EXPECT_EQ(vfs.getDiscCount(), 2u);
  EXPECT_EQ(vfs.getCurrentDisc(), 1u);
  EXPECT_EQ(vfs.getBootPath().value_or(""), "SLUS_123.45");
}

TEST_F(VirtualFsTest, SwapToAMissingImageFailsAndLeavesNoDiscMounted) {
  ps1::cdrom::VirtualFs vfs;
  ASSERT_TRUE(vfs.loadDisc(testBinPath));

  EXPECT_FALSE(vfs.swapDisc(dir / "absent.bin"));

  EXPECT_FALSE(vfs.readSector(16).has_value());
}
