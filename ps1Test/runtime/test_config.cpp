#include "runtime/config.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

using namespace ps1;

class ConfigTest : public ::testing::Test {
protected:
  std::string tmpPath;

  void SetUp() override {
    tmpPath = std::filesystem::temp_directory_path() / "ps1_test_config.toml";
  }
  void TearDown() override { std::remove(tmpPath.c_str()); }
};

TEST_F(ConfigTest, DefaultValues) {
  ConfigReader reader;
  EXPECT_EQ(reader.config().video.windowWidth, 640);
  EXPECT_EQ(reader.config().video.windowHeight, 480);
  EXPECT_FALSE(reader.config().video.fullscreen);
  EXPECT_TRUE(reader.config().video.vsync);
  EXPECT_EQ(reader.config().audio.sampleRate, 44100);
  EXPECT_EQ(reader.config().audio.volume, 100);
  EXPECT_FALSE(reader.config().audio.mute);
}

TEST_F(ConfigTest, CreateDefault) {
  ASSERT_TRUE(ConfigReader::createDefault(tmpPath));
  EXPECT_TRUE(std::filesystem::exists(tmpPath));

  // Should be loadable
  ConfigReader reader;
  ASSERT_TRUE(reader.load(tmpPath));
  EXPECT_EQ(reader.config().input.keyMap["z"], "cross");
  EXPECT_EQ(reader.config().input.keyMap["x"], "circle");
}

TEST_F(ConfigTest, LoadAndParse) {
  // Write a test config
  {
    std::ofstream f(tmpPath);
    f << "# Test config\n";
    f << "[video]\n";
    f << "width = 1280\n";
    f << "height = 720\n";
    f << "fullscreen = true\n";
    f << "vsync = false\n\n";
    f << "[audio]\n";
    f << "sample_rate = 48000\n";
    f << "volume = 75\n";
    f << "mute = false\n\n";
    f << "[input]\n";
    f << "z = \"cross\"\n";
    f << "analog = true\n\n";
    f << "[paths]\n";
    f << "disc = \"/games/crash.bin\"\n";
    f << "memcard_dir = \"./saves\"\n";
  }

  ConfigReader reader;
  ASSERT_TRUE(reader.load(tmpPath));

  EXPECT_EQ(reader.config().video.windowWidth, 1280);
  EXPECT_EQ(reader.config().video.windowHeight, 720);
  EXPECT_TRUE(reader.config().video.fullscreen);
  EXPECT_FALSE(reader.config().video.vsync);
  EXPECT_EQ(reader.config().audio.sampleRate, 48000);
  EXPECT_EQ(reader.config().audio.volume, 75);
  EXPECT_TRUE(reader.config().input.enableAnalog);
  EXPECT_EQ(reader.config().input.keyMap["z"], "cross");
  EXPECT_EQ(reader.config().paths.discImage, "/games/crash.bin");
  EXPECT_EQ(reader.config().paths.memcardDir, "./saves");
}

TEST_F(ConfigTest, SaveAndReload) {
  ConfigReader writer;
  writer.config().video.windowWidth = 1920;
  writer.config().video.windowHeight = 1080;
  writer.config().audio.volume = 50;
  writer.config().paths.discImage = "/my/game.iso";

  ASSERT_TRUE(writer.save(tmpPath));

  ConfigReader reader;
  ASSERT_TRUE(reader.load(tmpPath));
  EXPECT_EQ(reader.config().video.windowWidth, 1920);
  EXPECT_EQ(reader.config().video.windowHeight, 1080);
  EXPECT_EQ(reader.config().audio.volume, 50);
  EXPECT_EQ(reader.config().paths.discImage, "/my/game.iso");
}

TEST_F(ConfigTest, MissingFileReturnsError) {
  ConfigReader reader;
  EXPECT_FALSE(reader.load("/nonexistent/config.toml"));
  EXPECT_FALSE(reader.error().empty());
}

TEST_F(ConfigTest, CommentsAndBlankLinesIgnored) {
  {
    std::ofstream f(tmpPath);
    f << "# Full line comment\n";
    f << "\n";
    f << "  \n";
    f << "[video]\n";
    f << "# Width setting\n";
    f << "width = 800\n";
  }

  ConfigReader reader;
  ASSERT_TRUE(reader.load(tmpPath));
  EXPECT_EQ(reader.config().video.windowWidth, 800);
}

// Game TOMLs write addresses and counts as integers or strings ("0x8005494C").
// A string must be a whole number that fits in 32 bits: a typo must not
// silently become 0 or a truncated prefix.
TEST(ParseConfigU32, AcceptsHexAndDecimalStrings) {
  EXPECT_EQ(ps1::parseConfigU32("0x8005494C"), 0x8005494Cu);
  EXPECT_EQ(ps1::parseConfigU32("16"), 16u);
}

TEST(ParseConfigU32, RejectsTyposAndPartialNumbers) {
  EXPECT_EQ(ps1::parseConfigU32("0x80O5"), std::nullopt);
  EXPECT_EQ(ps1::parseConfigU32("16 ticks"), std::nullopt);
  EXPECT_EQ(ps1::parseConfigU32(""), std::nullopt);
  EXPECT_EQ(ps1::parseConfigU32(" 16"), std::nullopt);
  EXPECT_EQ(ps1::parseConfigU32("-1"), std::nullopt);
}

TEST(ParseConfigU32, RejectsValuesWiderThan32Bits) {
  EXPECT_EQ(ps1::parseConfigU32("0x100000000"), std::nullopt);
  EXPECT_EQ(ps1::parseConfigU32("0xFFFFFFFF"), 0xFFFFFFFFu);
}
