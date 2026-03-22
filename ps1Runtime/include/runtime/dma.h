#pragma once

// ps1Runtime — DMA Stub
// PS1 DMA controller (7 channels: MDECin, MDECout, GPU, CDROM, SPU, PIO, OTC)
// This is a stub for future incremental implementation.

#include <cstdint>
#include <cstring>

namespace ps1 {

class DMA {
public:
  static constexpr uint32_t NUM_CHANNELS = 7;

  enum Channel : uint8_t {
    MDEC_IN = 0,
    MDEC_OUT = 1,
    GPU_CH = 2,
    CDROM_CH = 3,
    SPU_CH = 4,
    PIO = 5,
    OTC = 6,
  };

  struct ChannelRegs {
    uint32_t baseAddr;
    uint32_t blockControl;
    uint32_t channelControl;
  };

  DMA() { reset(); }

  void reset() {
    std::memset(channels_, 0, sizeof(channels_));
    dpcr_ = 0x07654321; // Default channel priorities
    dicr_ = 0;
  }

  // Register access — stub
  void writeRegister(uint32_t addr, uint32_t val) {
    (void)addr;
    (void)val;
  }
  uint32_t readRegister(uint32_t addr) const {
    (void)addr;
    return 0;
  }

  // DMA control registers
  uint32_t dpcr() const { return dpcr_; }
  uint32_t dicr() const { return dicr_; }

private:
  ChannelRegs channels_[NUM_CHANNELS];
  uint32_t dpcr_; // DMA Priority Control Register
  uint32_t dicr_; // DMA Interrupt Control Register
};

} // namespace ps1
