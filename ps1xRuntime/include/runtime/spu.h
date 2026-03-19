#pragma once

// ps1xRuntime — SPU Stub
// PS1 SPU (24 voice channels, 512KB sound RAM, ADSR envelopes)
// This is a stub for future incremental implementation.

#include <cstdint>
#include <cstring>

namespace ps1 {

class SPU {
public:
  static constexpr uint32_t NUM_VOICES = 24;
  static constexpr uint32_t SOUND_RAM_SIZE = 512 * 1024; // 512KB

  struct Voice {
    uint32_t volumeLeft;
    uint32_t volumeRight;
    uint16_t pitch;
    uint16_t startAddr;
    uint16_t adsrLo;
    uint16_t adsrHi;
    uint16_t currentVolume;
    uint16_t repeatAddr;
  };

  SPU() { reset(); }

  void reset() {
    std::memset(voices_, 0, sizeof(voices_));
    std::memset(soundRam_, 0, sizeof(soundRam_));
    spuCtrl_ = 0;
    spuStat_ = 0;
  }

  // Register access — stub
  void writeRegister(uint32_t addr, uint16_t val) {
    (void)addr;
    (void)val;
  }
  uint16_t readRegister(uint32_t addr) const {
    (void)addr;
    return 0;
  }

  // Sound RAM
  uint8_t *soundRamPtr() { return soundRam_; }

  // Control
  uint16_t spuCtrl() const { return spuCtrl_; }
  uint16_t spuStat() const { return spuStat_; }

private:
  Voice voices_[NUM_VOICES];
  uint8_t soundRam_[SOUND_RAM_SIZE];
  uint16_t spuCtrl_;
  uint16_t spuStat_;
};

} // namespace ps1
