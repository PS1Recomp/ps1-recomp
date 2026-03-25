#pragma once
/**
 * @file config.h
 * @brief Runtime configuration structures and TOML config reader.
 *
 * `ps1::ConfigReader` loads a TOML file and populates the `ps1::Config`
 * aggregate (video, audio, input, paths). This is separate from the per-game
 * `psyq_addresses` config which is parsed directly by `main_host.cpp`.
 *
 * Typical usage:
 * @code
 * ps1::ConfigReader reader;
 * if (!reader.load("configs/settings.toml"))
 *     fmt::print(stderr, "Config error: {}\n", reader.error());
 * auto& cfg = reader.config();
 * @endcode
 */

#include <cstdint>
#include <string>
#include <unordered_map>

namespace ps1 {

struct VideoConfig {
  int windowWidth = 640;
  int windowHeight = 480;
  bool fullscreen = false;
  bool vsync = true;
  int internalResScale = 1; // 1x = native 320×240
};

struct AudioConfig {
  int sampleRate = 44100;
  int bufferSize = 1024;
  int volume = 100; // 0-100
  bool mute = false;
};

struct InputBinding {
  std::string key;    // SDL key name (e.g., "z", "x", "up")
  std::string button; // PS1 button name (e.g., "cross", "circle", "up")
};

struct InputConfig {
  std::unordered_map<std::string, std::string> keyMap; // key → ps1 button
  bool enableAnalog = false;
};

struct PathConfig {
  std::string discImage;    // Path to .bin/.cue/.iso
  std::string biosPath;     // Path to BIOS ROM (optional for HLE)
  std::string memcardDir;   // Directory for memory card files
  std::string saveStateDir; // Directory for save states
};

struct Config {
  VideoConfig video;
  AudioConfig audio;
  InputConfig input;
  PathConfig paths;
};

class ConfigReader {
public:
  ConfigReader() = default;

  // Load config from TOML file
  bool load(const std::string &path);

  // Save current config to TOML file
  bool save(const std::string &path) const;

  // Create a default config file
  static bool createDefault(const std::string &path);

  // Access config
  const Config &config() const { return config_; }
  Config &config() { return config_; }

  // Get last error message
  const std::string &error() const { return error_; }

private:
  Config config_;
  std::string error_;
};

} // namespace ps1
