#include "runtime/cdrom/exe_extractor.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <gtest/gtest.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path uniqueImagePath(const char *tag) {
  static int counter = 0;
  return fs::temp_directory_path() /
         (std::string("ps1_exe_") + tag + "_" + std::to_string(getpid()) +
          "_" + std::to_string(counter++) + ".bin");
}

// Raw 2352-byte-sector image: PVD at 16, root directory at 17, files after.
struct DiscBuilder {
  std::vector<uint8_t> bytes =
      std::vector<uint8_t>(24 * ps1::cdrom::SECTOR_SIZE_RAW, 0);
  size_t rootOff = 0;

  DiscBuilder() {
    uint8_t *pvd = data(16);
    pvd[0] = 1;
    std::memcpy(pvd + 1, "CD001", 5);
    uint32_t rootLba = 17, rootLen = 2048;
    std::memcpy(pvd + 156 + 2, &rootLba, 4);
    std::memcpy(pvd + 156 + 10, &rootLen, 4);
  }
  uint8_t *data(uint32_t lba) {
    return &bytes[lba * ps1::cdrom::SECTOR_SIZE_RAW + 24];
  }
  void entry(uint32_t dirLba, size_t &off, const std::string &name,
             uint32_t extent, uint32_t size, bool dir = false) {
    uint8_t *d = data(dirLba) + off;
    d[0] = static_cast<uint8_t>(34 + name.size());
    std::memcpy(d + 2, &extent, 4);
    std::memcpy(d + 10, &size, 4);
    d[25] = dir ? 0x02 : 0x00;
    d[32] = static_cast<uint8_t>(name.size());
    std::memcpy(d + 33, name.data(), name.size());
    off += d[0];
  }
  // A root file whose body is stored at `lba` (pass lba >= 24 to point the
  // record past the end of the image).
  void file(const std::string &name, uint32_t lba, const std::string &body) {
    entry(17, rootOff, name, lba, static_cast<uint32_t>(body.size()));
    if (lba < 24)
      std::memcpy(data(lba), body.data(), body.size());
  }
  void write(const fs::path &p) const {
    std::ofstream(p, std::ios::binary)
        .write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  }
};

class ExeExtractorDiscTest : public ::testing::Test {
protected:
  fs::path path = uniqueImagePath("disc");
  ps1::cdrom::BinReader reader;
  std::unique_ptr<ps1::cdrom::Iso9660Parser> parser;
  std::unique_ptr<ps1::cdrom::ExeExtractor> extractor;

  void TearDown() override { fs::remove(path); }
  void mount(const DiscBuilder &disc) {
    disc.write(path);
    ASSERT_TRUE(reader.open(path));
    parser = std::make_unique<ps1::cdrom::Iso9660Parser>(reader);
    ASSERT_TRUE(parser->initialize());
    extractor = std::make_unique<ps1::cdrom::ExeExtractor>(reader, *parser);
  }
};

} // namespace

class ExeExtractorTest : public ::testing::Test {
protected:
  void SetUp() override {
    testBinPath = uniqueImagePath("fixture");
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

  void TearDown() override {
    if (fs::exists(testBinPath)) {
      fs::remove(testBinPath);
    }
  }

  fs::path testBinPath;
};

TEST_F(ExeExtractorTest, ExtractBootPath) {
  ps1::cdrom::BinReader reader;
  EXPECT_TRUE(reader.open(testBinPath));

  ps1::cdrom::Iso9660Parser parser(reader);
  EXPECT_TRUE(parser.initialize());

  ps1::cdrom::ExeExtractor extractor(reader, parser);
  auto path = extractor.getBootExecutablePath();

  ASSERT_TRUE(path.has_value());
  EXPECT_EQ(path.value(), "SLUS_123.45");
}

TEST_F(ExeExtractorTest, ExtractExecutableBinary) {
  ps1::cdrom::BinReader reader;
  EXPECT_TRUE(reader.open(testBinPath));

  ps1::cdrom::Iso9660Parser parser(reader);
  EXPECT_TRUE(parser.initialize());

  ps1::cdrom::ExeExtractor extractor(reader, parser);
  auto exeData = extractor.extractExecutable();

  ASSERT_TRUE(exeData.has_value());
  EXPECT_EQ(exeData->size(), 8);
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(exeData->data()), 8),
            "PS-X EXE");
}

TEST_F(ExeExtractorDiscTest, DiscWithoutSystemCnfHasNoBootPath) {
  DiscBuilder disc;
  disc.file("PSX.EXE", 18, "PS-X EXE");
  mount(disc);

  EXPECT_FALSE(extractor->getBootExecutablePath().has_value());
  EXPECT_FALSE(extractor->extractExecutable().has_value());
}

TEST_F(ExeExtractorDiscTest, SystemCnfStoredPastTheImageEndHasNoBootPath) {
  DiscBuilder disc;
  disc.file("SYSTEM.CNF", 50, "BOOT = cdrom:\\SLUS_000.01;1\n");
  mount(disc);

  EXPECT_FALSE(extractor->getBootExecutablePath().has_value());
}

TEST_F(ExeExtractorDiscTest, SystemCnfWithoutABootLineHasNoBootPath) {
  DiscBuilder disc;
  disc.file("SYSTEM.CNF", 18, "TCB = 4\nEVENT = 10\n");
  mount(disc);

  EXPECT_FALSE(extractor->getBootExecutablePath().has_value());
}

TEST_F(ExeExtractorDiscTest, BootLineWithoutVersionHasItsCarriageReturnStripped) {
  DiscBuilder disc;
  disc.file("SYSTEM.CNF", 18, "boot=cdrom:\\SCUS_949.00\r\nTCB = 4\r\n");
  mount(disc);

  auto path = extractor->getBootExecutablePath();

  ASSERT_TRUE(path.has_value());
  EXPECT_EQ(*path, "SCUS_949.00");
}

TEST_F(ExeExtractorDiscTest, ExecutableInASubdirectoryIsFoundThroughDosPath) {
  DiscBuilder disc;
  disc.file("SYSTEM.CNF", 18, "BOOT = cdrom:\\BIN\\MAIN.EXE;1\n");
  disc.entry(17, disc.rootOff, "BIN", 19, 2048, /*dir=*/true);
  size_t binOff = 0;
  disc.entry(19, binOff, "MAIN.EXE;1", 20, 8);
  std::memcpy(disc.data(20), "PS-X EXE", 8);
  mount(disc);

  auto exe = extractor->extractExecutable();

  ASSERT_TRUE(exe.has_value());
  EXPECT_EQ(std::string(exe->begin(), exe->end()), "PS-X EXE");
}

TEST_F(ExeExtractorDiscTest, BootPathNamingAMissingFileExtractsNothing) {
  DiscBuilder disc;
  disc.file("SYSTEM.CNF", 18, "BOOT = cdrom:\\GONE.EXE;1\n");
  mount(disc);

  EXPECT_TRUE(extractor->getBootExecutablePath().has_value());
  EXPECT_FALSE(extractor->extractExecutable().has_value());
}

TEST_F(ExeExtractorDiscTest, ExecutableStoredPastTheImageEndExtractsNothing) {
  DiscBuilder disc;
  disc.file("SYSTEM.CNF", 18, "BOOT = cdrom:\\MAIN.EXE;1\n");
  disc.file("MAIN.EXE", 60, "PS-X EXE");
  mount(disc);

  EXPECT_FALSE(extractor->extractExecutable().has_value());
}
