#include "runtime/config.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <unistd.h>

using namespace ps1;

class ConfigTest : public ::testing::Test {
protected:
  std::string tmpPath;

  void SetUp() override {
    // Unique per process and test, so parallel ctest runs never share a file.
    auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
    tmpPath = (std::filesystem::temp_directory_path() /
               ("ps1_test_config_" + std::to_string(::getpid()) + "_" +
                info->name() + ".toml"))
                  .string();
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

TEST(ParseConfigU32, AcceptsAnUppercaseHexPrefix) {
  EXPECT_EQ(ps1::parseConfigU32("0X1F"), 0x1Fu);
}

TEST(ParseConfigU32, RejectsABareHexPrefix) {
  EXPECT_EQ(ps1::parseConfigU32("0x"), std::nullopt);
}

TEST_F(ConfigTest, ParsesTheRemainingKeys) {
  {
    std::ofstream f(tmpPath);
    f << "[video]\nres_scale = 3\n";
    f << "[audio]\nbuffer_size = 512\nmute = true\n";
    f << "[paths]\nbios = \"scph1001.bin\"\nsavestate_dir = \"./st\"\n";
  }
  ConfigReader reader;

  ASSERT_TRUE(reader.load(tmpPath));

  EXPECT_EQ(reader.config().video.internalResScale, 3);
  EXPECT_EQ(reader.config().audio.bufferSize, 512);
  EXPECT_TRUE(reader.config().audio.mute);
  EXPECT_EQ(reader.config().paths.biosPath, "scph1001.bin");
  EXPECT_EQ(reader.config().paths.saveStateDir, "./st");
}

TEST_F(ConfigTest, UnquotedPathsAreTakenVerbatim) {
  {
    std::ofstream f(tmpPath);
    f << "[paths]\ndisc = /games/a.cue\n";
  }
  ConfigReader reader;

  ASSERT_TRUE(reader.load(tmpPath));

  EXPECT_EQ(reader.config().paths.discImage, "/games/a.cue");
}

TEST_F(ConfigTest, LinesWithoutAnEqualsSignAreIgnored) {
  {
    std::ofstream f(tmpPath);
    f << "[video]\nwidth 1024\n[broken\nheight = 600\n";
  }
  ConfigReader reader;

  ASSERT_TRUE(reader.load(tmpPath));

  EXPECT_EQ(reader.config().video.windowWidth, 640) << "default kept";
  EXPECT_EQ(reader.config().video.windowHeight, 600)
      << "an unterminated header does not leave [video]";
}

TEST_F(ConfigTest, KeysOfUnknownSectionsAreIgnored) {
  {
    std::ofstream f(tmpPath);
    f << "[timing]\nwidth = 1024\n";
  }
  ConfigReader reader;

  ASSERT_TRUE(reader.load(tmpPath));

  EXPECT_EQ(reader.config().video.windowWidth, 640);
}

TEST_F(ConfigTest, SaveAndReloadKeepsFlagsAndEveryPath) {
  ConfigReader writer;
  auto &c = writer.config();
  c.video.fullscreen = true;
  c.video.vsync = false;
  c.audio.mute = true;
  c.input.enableAnalog = true;
  c.paths = {"/d.bin", "/b.bin", "/mc", "/ss"};
  ASSERT_TRUE(writer.save(tmpPath));
  ConfigReader reader;

  ASSERT_TRUE(reader.load(tmpPath));

  const auto &r = reader.config();
  EXPECT_TRUE(r.video.fullscreen);
  EXPECT_FALSE(r.video.vsync);
  EXPECT_TRUE(r.audio.mute);
  EXPECT_TRUE(r.input.enableAnalog);
  EXPECT_EQ(r.paths.discImage, "/d.bin");
  EXPECT_EQ(r.paths.biosPath, "/b.bin");
  EXPECT_EQ(r.paths.memcardDir, "/mc");
  EXPECT_EQ(r.paths.saveStateDir, "/ss");
}

TEST_F(ConfigTest, SaveOmitsEmptyPaths) {
  ConfigReader writer;
  ASSERT_TRUE(writer.save(tmpPath));

  std::ifstream f(tmpPath);
  std::stringstream text;
  text << f.rdbuf();

  EXPECT_EQ(text.str().find("disc ="), std::string::npos);
  EXPECT_EQ(text.str().find("bios ="), std::string::npos);
  EXPECT_NE(text.str().find("[paths]"), std::string::npos);
}

TEST_F(ConfigTest, SaveToAnUnwritablePathFails) {
  ConfigReader writer;

  EXPECT_FALSE(writer.save("/nonexistent_dir_ps1_test/config.toml"));
}
