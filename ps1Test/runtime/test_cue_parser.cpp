#include "runtime/cdrom/cue_parser.h"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

class CueParserTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Create a dummy CUE file for testing
    static int counter = 0;
    testCuePath = fs::temp_directory_path() /
                  ("ps1_cue_" + std::to_string(getpid()) + "_" +
                   std::to_string(counter++) + ".cue");
    std::ofstream off(testCuePath);
    off << "FILE \"dummy.bin\" BINARY\n"
        << "  TRACK 01 MODE2/2352\n"
        << "    INDEX 01 00:00:00\n"
        << "  TRACK 02 AUDIO\n"
        << "    INDEX 01 05:12:34\n";
  }

  void TearDown() override {
    if (fs::exists(testCuePath)) {
      fs::remove(testCuePath);
    }
  }

  fs::path testCuePath;
};

TEST_F(CueParserTest, ParseValidCueFile) {
  auto result = ps1::cdrom::CueParser::parse(testCuePath);
  ASSERT_TRUE(result.has_value());
  const auto &tracks = result.value();

  // Check Track Data
  ASSERT_EQ(tracks.size(), 2);
  EXPECT_EQ(tracks[0].number, 1);
  EXPECT_EQ(tracks[0].type, ps1::cdrom::TrackType::Data);
  EXPECT_EQ(tracks[0].file, "dummy.bin");
  EXPECT_EQ(tracks[0].msf_start, 0); // 00:00:00 = 0

  EXPECT_EQ(tracks[1].number, 2);
  EXPECT_EQ(tracks[1].type, ps1::cdrom::TrackType::Audio);
  EXPECT_EQ(tracks[1].file, "dummy.bin");

  // Check MSF parsing (05:12:34) -> (5*60*75) + (12*75) + 34
  uint32_t expectedMSF = (5 * 60 * 75) + (12 * 75) + 34;
  EXPECT_EQ(tracks[1].msf_start, expectedMSF);
}

TEST_F(CueParserTest, MissingFileYieldsError) {
  fs::path missingPath = fs::temp_directory_path() / "does_not_exist.cue";
  auto result = ps1::cdrom::CueParser::parse(missingPath);
  EXPECT_FALSE(result.has_value());
}

TEST_F(CueParserTest, SheetWithoutTracksYieldsNothing) {
  std::ofstream(testCuePath) << "FILE \"dummy.bin\" BINARY\n"
                             << "INDEX 01 00:02:00\n";

  auto result = ps1::cdrom::CueParser::parse(testCuePath);

  EXPECT_FALSE(result.has_value());
}

TEST_F(CueParserTest, EachTrackTakesTheMostRecentFile) {
  std::ofstream(testCuePath) << "FILE \"track1.bin\" BINARY\n"
                             << "  TRACK 01 MODE2/2352\n"
                             << "FILE \"track2.bin\" BINARY\n"
                             << "  TRACK 02 AUDIO\n"
                             << "    INDEX 00 00:00:00\n"
                             << "    INDEX 01 00:02:00\n";

  auto tracks = ps1::cdrom::CueParser::parse(testCuePath);

  ASSERT_TRUE(tracks.has_value());
  ASSERT_EQ(tracks->size(), 2u);
  EXPECT_EQ((*tracks)[0].file, "track1.bin");
  EXPECT_EQ((*tracks)[1].file, "track2.bin");
  EXPECT_EQ((*tracks)[1].msf_start, 150u); // INDEX 01, not the 00 pregap
}

