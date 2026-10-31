#include "runtime/spu/spu.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "runtime/metrics.h"
#include <fmt/format.h>
#include <string>
#include <vector>

namespace ps1::spu {

// ADPCM Filter Coefficients
constexpr int32_t SPU::ADPCM_FILTER_POS[5];
constexpr int32_t SPU::ADPCM_FILTER_NEG[5];

// Constructor / Destructor

SPU::SPU() { reset(); }

SPU::~SPU() = default;

void SPU::reset() {
  for (auto &v : voices_) {
    v = Voice{};
  }
  std::memset(soundRam_, 0, sizeof(soundRam_));
  spuCtrl_ = 0;
  spuStat_ = 0;
  mainVolL_ = 0;
  mainVolR_ = 0;
  irqAddr_ = 0;
  irqPending_ = false;
  keyOnLatch_ = 0;
  keyOffLatch_ = 0;
  noiseOn_ = 0;
  pitchModOn_ = 0;
  reverbOn_ = 0;
  endxFlags_ = 0;
  transferAddr_ = 0;
  transferCtrl_ = 0;
  reverb_ = ReverbConfig{};
  noiseLevel_ = -1;
  noiseTimer_ = 0;
  noiseStep_ = 0;
  xaBuffer_.clear();
  cdDaBuffer_.clear();
  xaReadPos_ = 0;
  cdDaReadPos_ = 0;
}

// Register I/O

// Per-voice census.  Sound effects and music use disjoint voice ranges in
// Crash, so "which voices ever get programmed, and which ever get keyed on" is
// the one measurement that separates a music driver that plays nothing from a
// music driver that never runs.  Names are built once, off the hot path.
namespace {
const std::string &voiceStartMetric(uint32_t voice) {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> v;
    for (uint32_t i = 0; i < NUM_VOICES; i++)
      v.push_back(fmt::format("spu.voice_start.{:02}", i));
    return v;
  }();
  return names[voice];
}

const std::string &voiceKeyOnMetric(uint32_t voice) {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> v;
    for (uint32_t i = 0; i < NUM_VOICES; i++)
      v.push_back(fmt::format("spu.key_on.{:02}", i));
    return v;
  }();
  return names[voice];
}
} // namespace

void SPU::writeRegister(uint32_t addr, uint16_t val) {
  std::lock_guard<std::mutex> lock(mutex_);
  uint32_t offset = addr - 0x1F801C00;

  // Per-voice registers: 0x00-0x17F (24 voices x 0x10 each)
  if (offset < 0x180) {
    uint32_t voiceIdx = offset / 0x10;
    uint32_t reg = offset % 0x10;
    Voice &v = voices_[voiceIdx];

    switch (reg) {
    case 0x00:
      v.volumeLeft = static_cast<int16_t>(val);
      break;
    case 0x02:
      v.volumeRight = static_cast<int16_t>(val);
      break;
    case 0x04:
      v.pitch = val;
      break;
    case 0x06:
      v.startAddr = val;
      // Per-voice census.  Sound effects and music use disjoint voice ranges
      // in Crash, so "which voices ever get programmed, and which ever get
      // keyed on" is the one measurement that separates a silent music driver
      // from a music driver that never runs at all.
      if (ps1::metrics::enabled())
        ps1::metrics::count(voiceStartMetric(voiceIdx));
      break;
    case 0x08:
      v.adsrLo = val;
      break;
    case 0x0A:
      v.adsrHi = val;
      break;
    case 0x0C:
      v.currentVolume = val;
      break;
    case 0x0E:
      v.repeatAddr = val;
      break;
    }
    return;
  }

  // Global registers: 0x180+
  switch (offset) {
  case 0x180:
    mainVolL_ = static_cast<int16_t>(val);
    break;
  case 0x182:
    mainVolR_ = static_cast<int16_t>(val);
    break;
  case 0x184:
    reverb_.volumeLeft = static_cast<int16_t>(val);
    break;
  case 0x186:
    reverb_.volumeRight = static_cast<int16_t>(val);
    break;

  // Key On / Key Off (Lo/Hi) -- trigger registers: a 1 bit keys the voice,
  // a 0 bit does nothing.  The latches accumulate until the mixer retires
  // them, because SpuSetKey writes both halves on every note: overwriting
  // made the music sequencer's `0x18A = 0` erase a sound effect keyed on a
  // high voice moments earlier in the same audio window.
  case 0x188:
    keyOnLatch_ |= val;
    break;
  case 0x18A:
    keyOnLatch_ |= static_cast<uint32_t>(val) << 16;
    break;
  case 0x18C:
    keyOffLatch_ |= val;
    break;
  case 0x18E:
    keyOffLatch_ |= static_cast<uint32_t>(val) << 16;
    break;

  // Pitch Mod Enable
  case 0x190:
    pitchModOn_ = (pitchModOn_ & 0xFFFF0000) | val;
    break;
  case 0x192:
    pitchModOn_ =
        (pitchModOn_ & 0x0000FFFF) | (static_cast<uint32_t>(val) << 16);
    break;

  // Noise Mode Enable
  case 0x194:
    noiseOn_ = (noiseOn_ & 0xFFFF0000) | val;
    break;
  case 0x196:
    noiseOn_ = (noiseOn_ & 0x0000FFFF) | (static_cast<uint32_t>(val) << 16);
    break;

  // Reverb On
  case 0x198:
    reverbOn_ = (reverbOn_ & 0xFFFF0000) | val;
    break;
  case 0x19A:
    reverbOn_ = (reverbOn_ & 0x0000FFFF) | (static_cast<uint32_t>(val) << 16);
    break;

  // ENDX -- read-only, writing clears
  case 0x19C:
    endxFlags_ &= ~(static_cast<uint32_t>(val));
    break;
  case 0x19E:
    endxFlags_ &= ~(static_cast<uint32_t>(val) << 16);
    break;

  // Reverb work area start
  case 0x1A2:
    reverb_.workAreaStart = val;
    break;

  // IRQ address
  case 0x1A4:
    irqAddr_ = val;
    break;

  // Transfer address
  case 0x1A6:
    transferAddr_ = static_cast<uint32_t>(val) * 8;
    break;

  // Transfer data (FIFO)
  case 0x1A8:
    writeTransferData(val);
    break;

  // SPU Control
  case 0x1AA:
    spuCtrl_ = val;
    // Immediately update SPUSTAT to mirror control bits 0-5.
    // On real hardware this happens within a few CPU cycles.
    // Without this, PsyQ SpuInit busy-waits on SPUSTAT forever
    // because generateSamples() (audio callback) hasn't fired yet.
    spuStat_ = val & 0x3F;
    // Noise frequency step from bits 8-13
    noiseStep_ = (val >> 8) & 0x3F;
    break;

  // Transfer Control
  case 0x1AC:
    transferCtrl_ = val;
    break;

  // SPU Status -- read only, ignore writes
  case 0x1AE:
    break;

  // CD Volume L/R
  case 0x1B0:
    break; // CD audio volume left
  case 0x1B2:
    break; // CD audio volume right

  // External Audio Volume
  case 0x1B4:
    break;
  case 0x1B6:
    break;

  // Current main volume (read-only)
  case 0x1B8:
    break;
  case 0x1BA:
    break;

  default:
    // Reverb registers (0x1C0-0x1FF)
    if (offset >= 0x1C0 && offset < 0x200) {
      uint32_t reverbIdx = (offset - 0x1C0) / 2;
      if (reverbIdx < 32) {
        reverb_.regs[reverbIdx] = val;
      }
    }
    break;
  }
}

uint16_t SPU::readRegister(uint32_t addr) const {
  std::lock_guard<std::mutex> lock(mutex_);
  uint32_t offset = addr - 0x1F801C00;

  // Per-voice registers
  if (offset < 0x180) {
    uint32_t voiceIdx = offset / 0x10;
    uint32_t reg = offset % 0x10;
    const Voice &v = voices_[voiceIdx];

    switch (reg) {
    case 0x00:
      return static_cast<uint16_t>(v.volumeLeft);
    case 0x02:
      return static_cast<uint16_t>(v.volumeRight);
    case 0x04:
      return v.pitch;
    case 0x06:
      return v.startAddr;
    case 0x08:
      return v.adsrLo;
    case 0x0A:
      return v.adsrHi;
    case 0x0C:
      return v.currentVolume;
    case 0x0E:
      return v.repeatAddr;
    }
    return 0;
  }

  switch (offset) {
  case 0x180:
    return static_cast<uint16_t>(mainVolL_);
  case 0x182:
    return static_cast<uint16_t>(mainVolR_);
  case 0x19C:
    return endxFlags_ & 0xFFFF;
  case 0x19E:
    return (endxFlags_ >> 16) & 0xFFFF;
  // Transfer address, in 8-byte units -- the same encoding the write takes.
  //
  // Crash's SPU driver writes this register and then polls it back until the
  // readback matches before it asks for DMA transfer mode.  Returning 0 here
  // made that wait time out (3841 spins), so the driver never set SPUCNT's
  // transfer-mode bits, the DMA channel was never programmed, and sound RAM
  // stayed empty -- 16 non-zero bytes out of 512 KB, and total silence.
  case 0x1A6:
    return static_cast<uint16_t>(transferAddr_ / 8);
  case 0x1AA:
    return spuCtrl_;
  case 0x1AE:
    return spuStat_;
  }
  return 0;
}

// Sound RAM Access

void SPU::writeSoundRam(uint32_t addr, uint16_t val) {
  if (addr + 1 < SOUND_RAM_SIZE) {
    soundRam_[addr] = val & 0xFF;
    soundRam_[addr + 1] = (val >> 8) & 0xFF;
  }
}

uint16_t SPU::readSoundRam(uint32_t addr) const {
  if (addr + 1 < SOUND_RAM_SIZE) {
    return soundRam_[addr] | (soundRam_[addr + 1] << 8);
  }
  return 0;
}

void SPU::loadSoundRam(const uint8_t *data) {
  std::memcpy(soundRam_, data, SOUND_RAM_SIZE);
}

void SPU::setTransferAddr(uint16_t val) {
  transferAddr_ = static_cast<uint32_t>(val) * 8;
}

void SPU::writeTransferData(uint16_t val) {
  if (transferAddr_ + 1 < SOUND_RAM_SIZE) {
    soundRam_[transferAddr_] = val & 0xFF;
    soundRam_[transferAddr_ + 1] = (val >> 8) & 0xFF;
    transferAddr_ += 2;
    if (transferAddr_ >= SOUND_RAM_SIZE) {
      transferAddr_ = 0;
    }
  }
}

// XA / CD-DA

void SPU::pushXaSamples(const int16_t *samples, uint32_t count) {
  std::lock_guard<std::mutex> lock(mutex_);
  xaBuffer_.insert(xaBuffer_.end(), samples, samples + count);
}

void SPU::pushCdDaSamples(const int16_t *samples, uint32_t count) {
  std::lock_guard<std::mutex> lock(mutex_);
  cdDaBuffer_.insert(cdDaBuffer_.end(), samples, samples + count);
}

// Key On / Off

void SPU::keyOnVoice(uint32_t idx) {
  Voice &v = voices_[idx];
  v.currentAddr = static_cast<uint32_t>(v.startAddr) * 8;
  v.pitchCounter = 0;
  v.prevSample1 = 0;
  v.prevSample2 = 0;
  v.decodedIndex = ADPCM_SAMPLES_PER_BLOCK; // force decode on first access
  v.adsrPhase = AdsrPhase::Attack;
  v.adsrVolume = 0;
  v.keyOn = false;
  v.endFlag = false;
  v.noiseMode = (noiseOn_ >> idx) & 1;
  v.pitchMod = (idx > 0) && ((pitchModOn_ >> idx) & 1);

  endxFlags_ &= ~(1 << idx);
}

void SPU::keyOffVoice(uint32_t idx) {
  Voice &v = voices_[idx];
  v.adsrPhase = AdsrPhase::Release;
  v.keyOff = false;
}

// ADPCM Decoding
//
// Block layout and the decode formula are documented in psx-spx
// (PS1Recomp-workspace/psx-spx.github.io/docs/):
//   soundprocessingunitspu.md:116-125  16-byte block: shift/filter, flags,
//                                       14 bytes of 2-nibbles-per-byte data
//   soundprocessingunitspu.md:119      "Shift/Filter (reportedly same as for
//                                       CD-XA)"
//   cdromformat.md:827-838             decode_28_nibbles(): shift = 12 -
//                                       header_shift, t = signed4bit(nibble),
//                                       s = (t<<shift) + (old*f0+older*f1+32)
//                                       /64, clamped to -8000h..+7FFFh
//   cdromformat.md:843-844,846-847     pos/neg coefficient tables; SPU-ADPCM
//                                       uses all 5 filters (0..4), XA only 4
//
// Cross-checked against the semantic reference runtime
// (CrashBandicoot-Launcher/RecompOne.Runtime/Hardware/Spu.cs:483-538,
// DecodeBlock/DecodeSample).

namespace {

// Decode the 14 compressed data bytes of one SPU-ADPCM block into 28 PCM
// samples, updating the running prediction state (prevSample1/2 = the two
// most recently decoded samples, "old"/"older" in the psx-spx pseudocode).
void decodeAdpcmBlockData(const uint8_t *dataBytes14, uint8_t shiftRaw,
                          int32_t f0, int32_t f1, int16_t &prevSample1,
                          int16_t &prevSample2,
                          int16_t out[ADPCM_SAMPLES_PER_BLOCK]) {
  // cdromformat.md:780,788 -- reserved shift values 13..15 act as shift=9.
  uint8_t clampedShiftRaw = (shiftRaw > 12) ? 9 : shiftRaw;
  int32_t shift = 12 - clampedShiftRaw;

  for (int i = 0; i < 14; i++) {
    uint8_t dataByte = dataBytes14[i];
    for (int nibble = 0; nibble < 2; nibble++) {
      // Sign-extend the 4-bit nibble (cdromformat.md:834, signed4bit()):
      // values 8..15 represent -8..-1. Casting a 0..15 value straight to
      // int8_t and *then* shifting does not sign-extend, because the shift
      // operand is promoted to `int` first -- the shift must place the
      // nibble's top bit at bit 7 of an 8-bit value before the cast.
      uint8_t raw = (dataByte >> (nibble * 4)) & 0x0F;
      int32_t t = (raw < 8) ? static_cast<int32_t>(raw)
                            : static_cast<int32_t>(raw) - 16;

      int32_t sample = (t << shift);
      sample += (prevSample1 * f0 + prevSample2 * f1 + 32) / 64;
      sample = std::clamp(sample, -32768, 32767);

      prevSample2 = prevSample1;
      prevSample1 = static_cast<int16_t>(sample);
      out[i * 2 + nibble] = static_cast<int16_t>(sample);
    }
  }
}

} // namespace

void SPU::advanceAdpcmBlock(Voice &v) {
  // Read the header byte of the 16-byte ADPCM block
  uint8_t header = soundRam_[v.currentAddr % SOUND_RAM_SIZE];
  uint8_t flags = soundRam_[(v.currentAddr + 1) % SOUND_RAM_SIZE];

  uint8_t shift = header & 0x0F;
  uint8_t filter = (header >> 4) & 0x07;
  if (filter > 4)
    filter = 4;

  uint8_t dataBytes[14];
  for (int i = 0; i < 14; i++) {
    dataBytes[i] = soundRam_[(v.currentAddr + 2 + i) % SOUND_RAM_SIZE];
  }

  decodeAdpcmBlockData(dataBytes, shift, ADPCM_FILTER_POS[filter],
                       ADPCM_FILTER_NEG[filter], v.prevSample1,
                       v.prevSample2, v.decodedSamples);

  v.decodedIndex = 0;

  // Check IRQ
  uint32_t irqByteAddr = static_cast<uint32_t>(irqAddr_) * 8;
  if ((spuCtrl_ & (1 << 6)) && v.currentAddr == irqByteAddr) {
    irqPending_ = true;
  }

  // Process flags
  if (flags & 0x04) { // Loop start
    v.repeatAddr = static_cast<uint16_t>(v.currentAddr / 8);
  }

  // Advance to next block
  v.currentAddr += ADPCM_BLOCK_SIZE;
  if (v.currentAddr >= SOUND_RAM_SIZE) {
    v.currentAddr = 0;
  }

  if (flags & 0x01) { // Loop End: set ENDX and jump, regardless of bit 1.
    // soundprocessingunitspu.md:130,135-141: Code 1 (End+Mute, bit1=0) and
    // Code 3 (End+Repeat, bit1=1) both "jump to Loop-address, set ENDX
    // flag"; only Code 1 additionally forces Release with envelope zeroed.
    v.endFlag = true;
    endxFlags_ |= (1u << (&v - voices_)); // set ENDX bit
    v.currentAddr = static_cast<uint32_t>(v.repeatAddr) * 8;
    v.loopFlag = true;

    if (!(flags & 0x02)) {
      // Code 1 = End+Mute: force Release phase, envelope to zero.
      v.adsrPhase = AdsrPhase::Release;
      v.adsrVolume = 0;
    }
    // Code 3 (flags & 0x02) = End+Repeat: jump only, envelope untouched.
  }
}

int16_t SPU::decodeAdpcmSample(Voice &v) {
  if (v.decodedIndex >= ADPCM_SAMPLES_PER_BLOCK) {
    advanceAdpcmBlock(v);
  }
  return v.decodedSamples[v.decodedIndex];
}

std::array<int16_t, ADPCM_SAMPLES_PER_BLOCK>
SPU::decodeAdpcmBlockForTest(const uint8_t block[ADPCM_BLOCK_SIZE],
                             int16_t prevSample1, int16_t prevSample2) const {
  uint8_t header = block[0];
  uint8_t shift = header & 0x0F;
  uint8_t filter = (header >> 4) & 0x07;
  if (filter > 4)
    filter = 4;

  std::array<int16_t, ADPCM_SAMPLES_PER_BLOCK> out{};
  decodeAdpcmBlockData(block + 2, shift, ADPCM_FILTER_POS[filter],
                       ADPCM_FILTER_NEG[filter], prevSample1, prevSample2,
                       out.data());
  return out;
}

SPU::VoiceDebugState SPU::debugVoiceState(uint32_t voiceIdx) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const Voice &v = voices_[voiceIdx];
  return VoiceDebugState{v.currentAddr, v.repeatAddr, v.loopFlag, v.endFlag,
                         v.adsrPhase};
}

// ADSR

void SPU::tickAdsr(Voice &v) {
  if (v.adsrPhase == AdsrPhase::Off) {
    v.adsrVolume = 0;
    return;
  }

  // Decode ADSR parameters from registers
  // adsrLo: Sustain Mode | Sustain Dir | unused | Attack Mode | Attack Shift |
  // Attack Step | Decay Shift adsrHi: Sustain Level | Release Mode | Release
  // Shift

  // Simplified ADSR -- use parameterized rates
  int32_t step = 0;

  switch (v.adsrPhase) {
  case AdsrPhase::Attack: {
    // Attack: ramp up to 0x7FFF
    uint8_t attackShift = (v.adsrLo >> 10) & 0x1F;
    uint8_t attackStep = (v.adsrLo >> 8) & 0x03;
    bool attackMode = (v.adsrLo >> 15) & 1; // 0=linear, 1=exponential

    step = 7 - (attackStep & 3);
    if (attackShift < 11) {
      step <<= (11 - attackShift);
    } else {
      step >>= (attackShift - 11);
    }
    if (step < 1)
      step = 1;

    if (attackMode && v.adsrVolume >= 0x6000) {
      step >>= 2; // Exponential: slow down near peak
    }

    v.adsrVolume += step;
    if (v.adsrVolume >= 0x7FFF) {
      v.adsrVolume = 0x7FFF;
      v.adsrPhase = AdsrPhase::Decay;
    }
    break;
  }
  case AdsrPhase::Decay: {
    // Decay: ramp down to sustain level
    uint8_t decayShift = (v.adsrLo >> 4) & 0x0F;
    uint16_t sustainLevel = ((v.adsrHi >> 0) & 0x0F);
    int32_t target = (sustainLevel + 1) * 0x800;
    if (target > 0x7FFF)
      target = 0x7FFF;

    step = -8;
    if (decayShift < 11) {
      step <<= (11 - decayShift);
    } else {
      step >>= (decayShift - 11);
    }
    if (step > -1)
      step = -1;

    // Exponential decay
    step = (step * v.adsrVolume) / 0x8000;
    if (step > -1)
      step = -1;

    v.adsrVolume += step;
    if (v.adsrVolume <= target) {
      v.adsrVolume = target;
      v.adsrPhase = AdsrPhase::Sustain;
    }
    break;
  }
  case AdsrPhase::Sustain: {
    // Sustain: hold or slowly decrease
    bool sustainDir = (v.adsrHi >> 14) & 1; // 0=increase, 1=decrease
    uint8_t sustainShift = (v.adsrHi >> 8) & 0x1F;
    uint8_t sustainStep = (v.adsrHi >> 6) & 0x03;
    bool sustainMode = (v.adsrHi >> 15) & 1; // exponential

    step = 7 - (sustainStep & 3);
    if (sustainShift < 11) {
      step <<= (11 - sustainShift);
    } else {
      step >>= (sustainShift - 11);
    }
    if (step < 1)
      step = 1;

    if (sustainDir) {
      step = -step;
      if (sustainMode) {
        step = (step * v.adsrVolume) / 0x8000;
        if (step > -1)
          step = -1;
      }
    } else {
      if (sustainMode && v.adsrVolume >= 0x6000) {
        step >>= 2;
      }
    }

    v.adsrVolume += step;
    v.adsrVolume = std::clamp(v.adsrVolume, 0, 0x7FFF);
    break;
  }
  case AdsrPhase::Release: {
    // Release: ramp down to 0
    uint8_t releaseShift = (v.adsrHi >> 4) & 0x1F;
    bool releaseMode = (v.adsrHi >> 5) & 1; // exponential

    step = -8;
    if (releaseShift < 11) {
      step <<= (11 - releaseShift);
    } else {
      step >>= (releaseShift - 11);
    }
    if (step > -1)
      step = -1;

    if (releaseMode) {
      step = (step * v.adsrVolume) / 0x8000;
      if (step > -1)
        step = -1;
    }

    v.adsrVolume += step;
    if (v.adsrVolume <= 0) {
      v.adsrVolume = 0;
      v.adsrPhase = AdsrPhase::Off;
    }
    break;
  }
  case AdsrPhase::Off:
    break;
  }

  v.currentVolume = static_cast<uint16_t>(v.adsrVolume);
}

// Noise Generator

void SPU::tickNoiseGenerator() {
  // PS1 noise generator is a 15-bit LFSR
  noiseTimer_++;
  if (noiseTimer_ >= noiseStep_ + 4) {
    noiseTimer_ = 0;
    // LFSR: bit15 = bit0 XOR bit1, then shift right
    uint32_t bit = (noiseLevel_ ^ (noiseLevel_ >> 1)) & 1;
    noiseLevel_ = (noiseLevel_ >> 1) | (bit << 14);
  }
}

// Per-Voice Processing

void SPU::processVoice(uint32_t voiceIdx, int32_t &outL, int32_t &outR,
                       int16_t prevVoiceOutput) {
  Voice &v = voices_[voiceIdx];

  if (v.adsrPhase == AdsrPhase::Off) {
    return;
  }

  // Get sample
  int16_t sample;
  if (v.noiseMode) {
    sample = static_cast<int16_t>(noiseLevel_);
  } else {
    // Pitch modulation
    uint16_t pitch = v.pitch;
    if (v.pitchMod && voiceIdx > 0) {
      int32_t factor = prevVoiceOutput + 0x8000;
      pitch =
          static_cast<uint16_t>((static_cast<int32_t>(pitch) * factor) >> 15);
    }
    pitch = std::min<uint16_t>(pitch, 0x3FFF); // Max pitch

    // Advance pitch counter
    v.pitchCounter += pitch;

    // Number of samples to advance
    while (v.pitchCounter >= 0x1000) {
      v.pitchCounter -= 0x1000;
      v.decodedIndex++;
      if (v.decodedIndex >= ADPCM_SAMPLES_PER_BLOCK) {
        advanceAdpcmBlock(v);
      }
    }

    sample = v.decodedSamples[v.decodedIndex % ADPCM_SAMPLES_PER_BLOCK];
  }

  // Apply ADSR envelope
  tickAdsr(v);
  int32_t envelopedSample = (static_cast<int32_t>(sample) * v.adsrVolume) >> 15;

  // Apply per-voice volume
  int32_t left = (envelopedSample * v.volumeLeft) >> 15;
  int32_t right = (envelopedSample * v.volumeRight) >> 15;

  outL += left;
  outR += right;
}

// Main Sample Generation

void SPU::generateSamples(int16_t *outputBuffer, uint32_t numSamples) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Process key on/off latches.
  //
  // Hardware retires KON/KOFF at the 44.1 kHz sample clock; we only drain them
  // once per audio callback (~23 ms of guest writes), so both bits are often
  // set for the same voice.  Applying both killed the voice at birth -- total
  // silence, measured.  Key-on wins, and the release is dropped.
  //
  // This is not faithful: notes sustain longer than they should, which is
  // audible as overlap.  Deferring the release by one callback instead was
  // worse (the voice lived 23 ms), and applying both at write time also came
  // out silent.  The real fix is per-sample retirement inside the mixer; until
  // then this is the variant that actually produces sound.
  for (uint32_t i = 0; i < NUM_VOICES; i++) {
    if (keyOnLatch_ & (1u << i)) {
      keyOnVoice(i);
      if (ps1::metrics::enabled())
        ps1::metrics::count(voiceKeyOnMetric(i));
    }
    else if (keyOffLatch_ & (1u << i))
      keyOffVoice(i);
  }
  keyOnLatch_ = 0;
  keyOffLatch_ = 0;

  // Update noise/pitch-mod flags
  for (uint32_t i = 0; i < NUM_VOICES; i++) {
    voices_[i].noiseMode = (noiseOn_ >> i) & 1;
    voices_[i].pitchMod = (i > 0) && ((pitchModOn_ >> i) & 1);
  }

  for (uint32_t s = 0; s < numSamples; s++) {
    int32_t mixL = 0, mixR = 0;
    int16_t prevOutput = 0;

    tickNoiseGenerator();

    // Mix all 24 voices
    for (uint32_t i = 0; i < NUM_VOICES; i++) {
      int32_t voiceL = 0, voiceR = 0;
      processVoice(i, voiceL, voiceR, prevOutput);
      mixL += voiceL;
      mixR += voiceR;

      // Store output for pitch modulation of next voice
      if (voices_[i].adsrPhase != AdsrPhase::Off) {
        prevOutput = voices_[i].decodedSamples[voices_[i].decodedIndex %
                                               ADPCM_SAMPLES_PER_BLOCK];
      } else {
        prevOutput = 0;
      }
    }

    // Mix XA-ADPCM audio (stereo interleaved)
    if (xaReadPos_ + 1 < xaBuffer_.size()) {
      mixL += xaBuffer_[xaReadPos_];
      mixR += xaBuffer_[xaReadPos_ + 1];
      xaReadPos_ += 2;
    }

    // Mix CD-DA audio (stereo interleaved)
    if (cdDaReadPos_ + 1 < cdDaBuffer_.size()) {
      mixL += cdDaBuffer_[cdDaReadPos_];
      mixR += cdDaBuffer_[cdDaReadPos_ + 1];
      cdDaReadPos_ += 2;
    }

    // Apply master volume
    mixL = (mixL * mainVolL_) >> 15;
    mixR = (mixR * mainVolR_) >> 15;

    // Clamp to 16-bit
    outputBuffer[s * 2] = static_cast<int16_t>(std::clamp(mixL, -32768, 32767));
    outputBuffer[s * 2 + 1] =
        static_cast<int16_t>(std::clamp(mixR, -32768, 32767));
  }

  // Clean up consumed XA/CD-DA buffers
  if (xaReadPos_ > 0 && xaReadPos_ <= xaBuffer_.size()) {
    xaBuffer_.erase(xaBuffer_.begin(), xaBuffer_.begin() + xaReadPos_);
    xaReadPos_ = 0;
  }
  if (cdDaReadPos_ > 0 && cdDaReadPos_ <= cdDaBuffer_.size()) {
    cdDaBuffer_.erase(cdDaBuffer_.begin(), cdDaBuffer_.begin() + cdDaReadPos_);
    cdDaReadPos_ = 0;
  }

  // Update SPU status
  spuStat_ = spuCtrl_ & 0x3F; // Mirror control bits to status
}

} // namespace ps1::spu
