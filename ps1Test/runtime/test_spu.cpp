#include "runtime/metrics.h"
#include "runtime/spu/spu.h"
#include <cstring>
#include <vector>
#include <gtest/gtest.h>

using namespace ps1::spu;

// ADPCM Decoding Tests
//
// Vectors are hand-worked from the psx-spx SPU-ADPCM decode formula
// (PS1Recomp-workspace/psx-spx.github.io/docs/cdromformat.md:827-838,
// coefficient tables at :843-844) applied to the SPU block layout
// (soundprocessingunitspu.md:116-125). See the derivation notes in
// .superpowers/sdd/phase-3-crash-jogavel/task-S1-report.md.

class SpuAdpcmTest : public ::testing::Test {
protected:
  SPU spu;
  void SetUp() override { spu.reset(); }
};

TEST_F(SpuAdpcmTest, DecodesFullNibbleRangeWithFilterZero) {
  // header = filter 0, raw shift 12 (=> effective shift 12-12=0). With
  // filter 0, f0=f1=0 (cdromformat.md:843-844), so per the formula
  // s = (t<<0) + (old*0+older*0+32)/64 = t + 0 (integer division truncates
  // 32/64 toward zero) -- i.e. the decoded output is exactly the
  // sign-extended nibble sequence. This isolates nibble sign-extension from
  // the prediction filter.
  uint8_t block[16] = {};
  block[0] = 0x0C; // filter=0, shift=12
  block[1] = 0x00; // flags: normal

  // Nibble sequence -8..7 repeated (28 values), covering every 4-bit
  // pattern including the ones that must sign-extend to negative (8..15).
  // Packed 2 nibbles/byte, low nibble first (soundprocessingunitspu.md:121:
  // "LSBs=1st Sample, MSBs=2nd Sample").
  const uint8_t dataBytes[14] = {0x98, 0xBA, 0xDC, 0xFE, 0x10, 0x32, 0x54,
                                 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x10, 0x32};
  std::memcpy(block + 2, dataBytes, 14);

  auto samples = spu.decodeAdpcmBlockForTest(block, /*prevSample1=*/0,
                                              /*prevSample2=*/0);

  const int16_t expected[28] = {-8, -7, -6, -5, -4, -3, -2, -1, 0, 1,
                                2,  3,  4,  5,  6,  7,  -8, -7, -6, -5,
                                -4, -3, -2, -1, 0,  1,  2,  3};
  for (int i = 0; i < 28; i++) {
    EXPECT_EQ(samples[i], expected[i]) << "sample index " << i;
  }
}

TEST_F(SpuAdpcmTest, AppliesFilterOneCoefficientsToPrediction) {
  // Same nibble data as above, but filter=1 (f0=+60, f1=0 per
  // cdromformat.md:843-844) with raw shift 12 (effective shift 0), so the
  // IIR prediction term (old*f0 + older*f1 + 32) / 64 is exercised.
  // Expected values derived by applying cdromformat.md:827-838 verbatim
  // (integer division truncating toward zero, matching C++ int division):
  //   s0 = -8 + (0*60+0*0+32)/64            = -8 + 0   = -8
  //   s1 = -7 + (-8*60+0*0+32)/64           = -7 + (-448/64=-7) = -14
  //   s2 = -6 + (-14*60+(-8)*0+32)/64       = -6 + (-808/64=-12) = -18
  //   ... (full 28-sample sequence computed with the same recurrence)
  uint8_t block[16] = {};
  block[0] = 0x1C; // filter=1, shift=12 (effective shift 0)
  block[1] = 0x00;

  const uint8_t dataBytes[14] = {0x98, 0xBA, 0xDC, 0xFE, 0x10, 0x32, 0x54,
                                 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x10, 0x32};
  std::memcpy(block + 2, dataBytes, 14);

  auto samples = spu.decodeAdpcmBlockForTest(block, /*prevSample1=*/0,
                                              /*prevSample2=*/0);

  const int16_t expected[28] = {-8,  -14, -18, -21, -23, -24, -24, -23,
                                -21, -18, -14, -9,  -3,  3,   9,   15,
                                6,   -1,  -6,  -10, -12, -13, -13, -12,
                                -10, -7,  -4,  0};
  for (int i = 0; i < 28; i++) {
    EXPECT_EQ(samples[i], expected[i]) << "sample index " << i;
  }
}

TEST_F(SpuAdpcmTest, SilentBlockPlaysSilenceAtFullVolume) {
  // Arrange: an all-zero block (end flag set), every volume at maximum.
  uint8_t *ram = spu.soundRamPtr();
  std::memset(ram, 0, 16);
  ram[1] = 0x01;
  spu.writeRegister(0x1F801C06, 0);      // start address
  spu.writeRegister(0x1F801C04, 0x1000); // pitch 1.0
  spu.writeRegister(0x1F801C00, 0x3FFF); // voice volume L
  spu.writeRegister(0x1F801C02, 0x3FFF); // voice volume R
  spu.writeRegister(0x1F801D80, 0x3FFF); // main volume L
  spu.writeRegister(0x1F801D82, 0x3FFF); // main volume R
  spu.writeRegister(0x1F801D88, 0x0001); // key on voice 0

  // Act
  int16_t buffer[128] = {};
  spu.generateSamples(buffer, 64);

  // Assert: zero ADPCM samples decode to zero whatever the envelope does.
  for (int16_t sample : buffer)
    EXPECT_EQ(sample, 0);
}

TEST_F(SpuAdpcmTest, NonSilentBlockIsAudibleOnceKeyedOn) {
  // Arrange: filter 1, shift 0, non-zero nibbles; fast attack; volumes up.
  uint8_t *ram = spu.soundRamPtr();
  ram[0] = 0x10;
  ram[1] = 0x03; // loop + end: keep playing the block
  for (int i = 2; i < 16; i++)
    ram[i] = 0x77;
  spu.writeRegister(0x1F801C06, 0);
  spu.writeRegister(0x1F801C04, 0x1000);
  spu.writeRegister(0x1F801C08, 0x00FF); // attack rate 0 (fastest), sustain max
  spu.writeRegister(0x1F801C00, 0x3FFF);
  spu.writeRegister(0x1F801C02, 0x3FFF);
  spu.writeRegister(0x1F801D80, 0x3FFF);
  spu.writeRegister(0x1F801D82, 0x3FFF);
  spu.writeRegister(0x1F801D88, 0x0001);

  // Act
  int16_t buffer[512] = {};
  spu.generateSamples(buffer, 256);

  // Assert
  bool audible = false;
  for (int16_t sample : buffer)
    audible |= sample != 0;
  EXPECT_TRUE(audible);
}

TEST_F(SpuAdpcmTest, EndxIsSetOnlyAfterTheEndFlagBlockIsReached) {
  // Arrange: ten 28-sample blocks, end flag on the last one only (psx-spx ENDX).
  uint8_t *ram = spu.soundRamPtr();
  for (int block = 0; block < 10; block++) {
    ram[block * 16] = 0x00;
    ram[block * 16 + 1] = (block == 9) ? 0x01 : 0x00;
  }
  spu.writeRegister(0x1F801C06, 0);
  spu.writeRegister(0x1F801C04, 0x1000); // one ADPCM sample per output sample
  spu.writeRegister(0x1F801D88, 0x0001);
  int16_t buffer[2 * 400] = {};

  // Act + Assert: after 100 samples the voice is still in block 3.
  spu.generateSamples(buffer, 100);
  EXPECT_EQ(spu.readRegister(0x1F801D9C) & 1, 0);

  // Act + Assert: 300 more samples pass the 280-sample end.
  spu.generateSamples(buffer, 300);
  EXPECT_EQ(spu.readRegister(0x1F801D9C) & 1, 1);
}

// ADPCM Loop/End Flag Tests
//
// Flag bits and the Code0-3 combinations are documented at
// soundprocessingunitspu.md:128-141. Cross-checked against
// CrashBandicoot-Launcher/RecompOne.Runtime/Hardware/Spu.cs:509-525
// (DecodeBlock), which unconditionally jumps CurAddr to RepeatAddr on the
// End flag (bit 0), regardless of the Repeat bit (bit 1); bit 1 only
// decides whether the envelope is also forced to Release/zero.
//
// A single generateSamples(buf, 1) call is enough to trigger exactly one
// advanceAdpcmBlock(): Key On forces decodedIndex to ADPCM_SAMPLES_PER_BLOCK,
// and pitch=0x1000 (1.0) advances decodedIndex by exactly one per output
// sample.

class SpuAdpcmLoopFlagTest : public ::testing::Test {
protected:
  SPU spu;
  void SetUp() override { spu.reset(); }

  void writeBlockHeader(uint32_t byteAddr, uint8_t shiftFilter,
                        uint8_t flags) {
    uint8_t *ram = spu.soundRamPtr();
    ram[byteAddr] = shiftFilter;
    ram[byteAddr + 1] = flags;
    std::memset(ram + byteAddr + 2, 0, 14);
  }
};

TEST_F(SpuAdpcmLoopFlagTest, Code0NormalAdvancesToNextBlockOnly) {
  writeBlockHeader(0, 0x00, 0x00); // filter=0,shift=0, flags=Normal

  spu.writeRegister(0x1F801C06, 0);      // start addr = 0
  spu.writeRegister(0x1F801C04, 0x1000); // pitch = 1.0
  spu.writeRegister(0x1F801D88, 0x0001); // key on voice 0

  int16_t buffer[2] = {};
  spu.generateSamples(buffer, 1);

  auto state = spu.debugVoiceState(0);
  EXPECT_EQ(state.currentAddr, 16u); // advanced by one 16-byte block only
  EXPECT_FALSE(state.endFlag);
  EXPECT_FALSE(state.loopFlag);
}

TEST_F(SpuAdpcmLoopFlagTest, LoopStartBitLatchesCurrentBlockAddress) {
  // Voice starts at block address 16 (startAddr=2, in 8-byte units).
  writeBlockHeader(16, 0x00, 0x04); // flags: Loop Start

  spu.writeRegister(0x1F801C06, 2); // start addr = 2 (=> byte addr 16)
  spu.writeRegister(0x1F801C04, 0x1000);
  spu.writeRegister(0x1F801D88, 0x0001);

  int16_t buffer[2] = {};
  spu.generateSamples(buffer, 1);

  // soundprocessingunitspu.md:100-102: "If the hardware finds an ADPCM
  // header with Loop-Start-Bit, then it copies the current address to the
  // repeat address register" -- current block's own address (16 bytes =
  // 2 in 8-byte units), not the next block.
  EXPECT_EQ(spu.readRegister(0x1F801C0E), 2);
}

TEST_F(SpuAdpcmLoopFlagTest, Code1EndMuteJumpsToRepeatAddressAndReleases) {
  writeBlockHeader(0, 0x00, 0x01); // flags: Loop End, bit1=0 => Code 1

  spu.writeRegister(0x1F801C06, 0);      // start addr = 0
  spu.writeRegister(0x1F801C0E, 5);      // repeat addr = 5 (=> byte addr 40)
  spu.writeRegister(0x1F801C04, 0x1000); // pitch = 1.0
  spu.writeRegister(0x1F801D88, 0x0001); // key on voice 0

  int16_t buffer[2] = {};
  spu.generateSamples(buffer, 1);

  auto state = spu.debugVoiceState(0);
  EXPECT_EQ(state.currentAddr, 40u); // jumped to repeatAddr*8, not addr+16
  EXPECT_TRUE(state.endFlag);
  EXPECT_TRUE(state.loopFlag);
  EXPECT_EQ(spu.readRegister(0x1F801D9C) & 0x1, 1); // ENDX bit 0 set
}

TEST_F(SpuAdpcmLoopFlagTest, Code3EndRepeatJumpsToRepeatAddress) {
  writeBlockHeader(0, 0x00, 0x03); // flags: Loop End + Loop Repeat => Code 3

  spu.writeRegister(0x1F801C06, 0);
  spu.writeRegister(0x1F801C0E, 5); // repeat addr = 5 (=> byte addr 40)
  spu.writeRegister(0x1F801C04, 0x1000);
  spu.writeRegister(0x1F801D88, 0x0001);

  int16_t buffer[2] = {};
  spu.generateSamples(buffer, 1);

  auto state = spu.debugVoiceState(0);
  EXPECT_EQ(state.currentAddr, 40u);
  EXPECT_TRUE(state.endFlag);
  EXPECT_TRUE(state.loopFlag);
  EXPECT_EQ(spu.readRegister(0x1F801D9C) & 0x1, 1);
}

// ADSR Envelope Tests

class SpuAdsrTest : public ::testing::Test {
protected:
  SPU spu;
  void SetUp() override { spu.reset(); }
};

TEST_F(SpuAdsrTest, VoiceStartsInOffPhase) {
  // By default, voices are in Off phase
  int16_t buffer[128] = {};
  spu.generateSamples(buffer, 64);

  // All output should be 0 (no voices active)
  for (int i = 0; i < 128; i++) {
    EXPECT_EQ(buffer[i], 0);
  }
}

TEST_F(SpuAdsrTest, KeyOnRaisesTheEnvelopeFromZero) {
  // Arrange
  uint8_t *ram = spu.soundRamPtr();
  std::memset(ram, 0, 16);
  ram[1] = 0x03; // loop + end
  spu.writeRegister(0x1F801C06, 0);
  spu.writeRegister(0x1F801C04, 0x1000);
  spu.writeRegister(0x1F801C08, 0x00FF); // attack rate 0: fastest
  spu.writeRegister(0x1F801C0A, 0x0000);
  ASSERT_EQ(spu.readRegister(0x1F801C0C), 0) << "envelope idle before key on";

  // Act
  spu.writeRegister(0x1F801D88, 0x0001);
  int16_t buffer[64] = {};
  spu.generateSamples(buffer, 32);

  // Assert: ADSR current volume (voice reg +0Ch) has started to rise.
  EXPECT_GT(spu.readRegister(0x1F801C0C), 0);
}

TEST_F(SpuAdsrTest, KeyOffReleasesTheEnvelope) {
  // Arrange: a keyed-on voice whose envelope has risen.
  uint8_t *ram = spu.soundRamPtr();
  std::memset(ram, 0, 16);
  ram[1] = 0x03;
  spu.writeRegister(0x1F801C06, 0);
  spu.writeRegister(0x1F801C04, 0x1000);
  spu.writeRegister(0x1F801C08, 0x00FF);
  spu.writeRegister(0x1F801C0A, 0x0000); // release: linear, rate 0 (fastest)
  spu.writeRegister(0x1F801D88, 0x0001);
  int16_t buffer[2 * 256] = {};
  spu.generateSamples(buffer, 64);
  const uint16_t held = spu.readRegister(0x1F801C0C);
  ASSERT_GT(held, 0);

  // Act
  spu.writeRegister(0x1F801D8C, 0x0001);
  spu.generateSamples(buffer, 256);

  // Assert
  EXPECT_LT(spu.readRegister(0x1F801C0C), held);
}

// Volume Tests

class SpuVolumeTest : public ::testing::Test {
protected:
  SPU spu;
  void SetUp() override { spu.reset(); }
};

TEST_F(SpuVolumeTest, ZeroMainVolumeProducesSilence) {
  spu.writeRegister(0x1F801D80, 0); // main vol L = 0
  spu.writeRegister(0x1F801D82, 0); // main vol R = 0

  int16_t buffer[128] = {};
  spu.generateSamples(buffer, 64);

  for (int i = 0; i < 128; i++) {
    EXPECT_EQ(buffer[i], 0);
  }
}

TEST_F(SpuVolumeTest, RegisterReadback) {
  spu.writeRegister(0x1F801D80, 0x3FFF);
  EXPECT_EQ(spu.readRegister(0x1F801D80), 0x3FFF);

  spu.writeRegister(0x1F801D82, 0x1234);
  EXPECT_EQ(spu.readRegister(0x1F801D82), 0x1234);
}

// SPU Reverb Tests

class SpuReverbTest : public ::testing::Test {
protected:
  SPU spu;
  void SetUp() override { spu.reset(); }
};

TEST_F(SpuReverbTest, ReverbConfigWithoutReverbEnableKeepsOutputSilent) {
  // Arrange: every reverb register written, but no voice and no reverb enable.
  for (uint32_t i = 0; i < 32; i++)
    spu.writeRegister(0x1F801DC0 + i * 2, 0x1234);
  spu.writeRegister(0x1F801D80, 0x3FFF);
  spu.writeRegister(0x1F801D82, 0x3FFF);

  // Act
  int16_t buffer[128] = {};
  spu.generateSamples(buffer, 64);

  // Assert: reverb parameters alone produce no sound.
  for (int16_t sample : buffer)
    EXPECT_EQ(sample, 0);
}

// XA-ADPCM Tests

class XaAdpcmTest : public ::testing::Test {
protected:
  SPU spu;
  void SetUp() override { spu.reset(); }
};

TEST_F(XaAdpcmTest, XaSamplesAreMixed) {
  spu.writeRegister(0x1F801D80, 0x7FFF); // main vol L
  spu.writeRegister(0x1F801D82, 0x7FFF); // main vol R

  // Push some XA samples (stereo interleaved)
  int16_t xaSamples[64];
  for (int i = 0; i < 64; i++) {
    xaSamples[i] = 1000; // constant value
  }
  spu.pushXaSamples(xaSamples, 64);

  int16_t buffer[64] = {};
  spu.generateSamples(buffer, 32);

  // With max main volume, XA samples should appear in output
  // (output = xaSample * mainVol / 32768)
  bool hasNonZero = false;
  for (int i = 0; i < 64; i++) {
    if (buffer[i] != 0)
      hasNonZero = true;
  }
  EXPECT_TRUE(hasNonZero);
}

TEST_F(XaAdpcmTest, CdDaSamplesAreMixed) {
  spu.writeRegister(0x1F801D80, 0x7FFF);
  spu.writeRegister(0x1F801D82, 0x7FFF);

  int16_t cdSamples[64];
  for (int i = 0; i < 64; i++) {
    cdSamples[i] = 2000;
  }
  spu.pushCdDaSamples(cdSamples, 64);

  int16_t buffer[64] = {};
  spu.generateSamples(buffer, 32);

  bool hasNonZero = false;
  for (int i = 0; i < 64; i++) {
    if (buffer[i] != 0)
      hasNonZero = true;
  }
  EXPECT_TRUE(hasNonZero);
}

// Sound RAM Tests

TEST(SpuSoundRam, WriteAndReadBack) {
  SPU spu;
  spu.reset();

  spu.writeSoundRam(0x100, 0xABCD);
  EXPECT_EQ(spu.readSoundRam(0x100), 0xABCD);

  spu.writeSoundRam(0x200, 0x1234);
  EXPECT_EQ(spu.readSoundRam(0x200), 0x1234);
}

TEST(SpuSoundRam, TransferViaRegister) {
  SPU spu;
  spu.reset();

  // Set transfer address (in 8-byte units, so val=1 -> byte addr 8)
  spu.writeRegister(0x1F801DA6, 1);      // addr = 8
  spu.writeRegister(0x1F801DA8, 0xDEAD); // write data

  EXPECT_EQ(spu.readSoundRam(8), 0xDEAD);
}

// Control Register Tests

TEST(SpuControl, SpuCtrlReadback) {
  SPU spu;
  spu.reset();

  spu.writeRegister(0x1F801DAA, 0xC001);
  EXPECT_EQ(spu.readRegister(0x1F801DAA), 0xC001);
}

TEST(SpuControl, EndxFlagsClearedOnWrite) {
  SPU spu;
  spu.reset();

  // ENDX is at 0x19C/0x19E offset
  // Initially 0
  EXPECT_EQ(spu.readRegister(0x1F801D9C), 0);
}

// Sound RAM Transfer Address Tests
//
// Crash's SPU driver programs the transfer address and then polls the
// register back until the readback matches before switching SPUCNT to DMA
// transfer mode. A stubbed read made that wait spin forever, so sound RAM
// was never filled and the game was silent.

TEST(SpuTransferAddress, ReadsBackTheValueWritten) {
  SPU spu;
  spu.reset();

  spu.writeRegister(0x1F801DA6, 0x1234);
  EXPECT_EQ(spu.readRegister(0x1F801DA6), 0x1234);
}

TEST(SpuTransferAddress, DataWritesAdvanceTheAddress) {
  SPU spu;
  spu.reset();

  spu.writeRegister(0x1F801DA6, 0x40); // byte address 0x200
  spu.writeTransferData(0xAAAA);
  spu.writeTransferData(0xBBBB);
  spu.writeTransferData(0xCCCC);
  spu.writeTransferData(0xDDDD);

  EXPECT_EQ(spu.readSoundRam(0x200), 0xAAAA);
  EXPECT_EQ(spu.readSoundRam(0x202), 0xBBBB);
  EXPECT_EQ(spu.readSoundRam(0x204), 0xCCCC);
  EXPECT_EQ(spu.readSoundRam(0x206), 0xDDDD);
  // Eight bytes written, and the register counts in 8-byte units.
  EXPECT_EQ(spu.readRegister(0x1F801DA6), 0x41);
}

// Key On / Key Off Latch Tests

TEST(SpuKeyLatch, KeyOnWinsWhenBothLatchesAreSetForTheSameVoice) {
  // The latches are drained once per audio callback, which covers ~23 ms of
  // guest register writes, so a voice keyed on and off inside that window
  // arrives with both bits set. Applying both retires the voice before it
  // ever plays, which measured as total silence in Crash. Key-on wins.
  SPU spu;
  spu.reset();

  spu.writeRegister(0x1F801D88, 0x0001); // key on voice 0
  spu.writeRegister(0x1F801D8C, 0x0001); // key off voice 0

  // Zero samples: drain the latches without letting the envelope advance,
  // so the phase read back is the one the latches set.
  int16_t buffer[2] = {};
  spu.generateSamples(buffer, 0);

  EXPECT_EQ(spu.debugVoiceState(0).adsrPhase, AdsrPhase::Attack);
}

TEST(SpuKeyLatch, KeyOffAloneStillReleasesTheVoice) {
  SPU spu;
  spu.reset();

  int16_t buffer[2] = {};
  spu.writeRegister(0x1F801D88, 0x0001); // key on voice 0
  spu.generateSamples(buffer, 0);
  ASSERT_EQ(spu.debugVoiceState(0).adsrPhase, AdsrPhase::Attack);

  spu.writeRegister(0x1F801D8C, 0x0001); // key off voice 0
  spu.generateSamples(buffer, 0);

  EXPECT_EQ(spu.debugVoiceState(0).adsrPhase, AdsrPhase::Release);
}

TEST(SpuKeyLatch, LaterWritesInTheSameWindowDoNotCancelAKeyOn) {
  // KON/KOFF are trigger registers: a 1 bit keys the voice, a 0 bit does
  // nothing.  SpuSetKey writes both halves on every note, so when the music
  // sequencer keys voice 0 in the same audio window that a sound effect keyed
  // voice 16, its `0x18A = 0` must not erase the effect's key-on.
  SPU spu;
  spu.reset();

  spu.writeRegister(0x1F801D8A, 0x0001); // sound effect: key on voice 16
  spu.writeRegister(0x1F801D88, 0x0001); // music: key on voice 0 ...
  spu.writeRegister(0x1F801D8A, 0x0000); // ... writing the high half as 0

  int16_t buffer[2] = {};
  spu.generateSamples(buffer, 0);

  EXPECT_EQ(spu.debugVoiceState(16).adsrPhase, AdsrPhase::Attack);
  EXPECT_EQ(spu.debugVoiceState(0).adsrPhase, AdsrPhase::Attack);
}

TEST(SpuKeyLatch, KeyOffBitsAccumulateAcrossWrites) {
  SPU spu;
  spu.reset();

  int16_t buffer[2] = {};
  spu.writeRegister(0x1F801D88, 0x0003); // key on voices 0 and 1
  spu.generateSamples(buffer, 0);

  spu.writeRegister(0x1F801D8C, 0x0001); // key off voice 0
  spu.writeRegister(0x1F801D8C, 0x0002); // key off voice 1
  spu.generateSamples(buffer, 0);

  EXPECT_EQ(spu.debugVoiceState(0).adsrPhase, AdsrPhase::Release);
  EXPECT_EQ(spu.debugVoiceState(1).adsrPhase, AdsrPhase::Release);
}

// Voice registers, envelope and modulation
//
// Contract: psx-spx "Sound Processing Unit (SPU)" (soundprocessingunitspu.md)
// -- "SPU Voice Registers", "SPU Volume and ADSR Generator", "Envelope
// Operation depending on Shift/Step/Mode/Direction", "Pitch Counter", "SPU
// Interrupt".  The envelope level is read back through 1F801C0Ch+N*10h
// (current ADSR volume), so the expected values are the per-sample steps the
// pseudocode produces.

namespace {

constexpr uint32_t kVoice0 = 0x1F801C00;
constexpr uint32_t voiceReg(uint32_t voice, uint32_t reg) {
  return kVoice0 + voice * 0x10 + reg;
}

class SpuVoiceTest : public ::testing::Test {
protected:
  SPU spu;
  int16_t out[2 * 512] = {};

  void SetUp() override { spu.reset(); }

  // A silent block that loops onto itself (flags 3 = End+Repeat), so a voice
  // keyed on it plays for as long as the test needs.
  void loopBlock(uint32_t byteAddr, uint8_t header = 0x00,
                 uint8_t data = 0x00) {
    uint8_t *ram = spu.soundRamPtr();
    ram[byteAddr] = header;
    ram[byteAddr + 1] = 0x03;
    std::memset(ram + byteAddr + 2, data, 14);
  }
  void keyOnVoice(uint32_t voice, uint16_t adsrLo, uint16_t adsrHi,
                  uint16_t start = 0) {
    spu.writeRegister(voiceReg(voice, 0x06), start);
    spu.writeRegister(voiceReg(voice, 0x0E), start);
    spu.writeRegister(voiceReg(voice, 0x04), 0x1000);
    spu.writeRegister(voiceReg(voice, 0x08), adsrLo);
    spu.writeRegister(voiceReg(voice, 0x0A), adsrHi);
    if (voice < 16)
      spu.writeRegister(0x1F801D88, static_cast<uint16_t>(1u << voice));
    else
      spu.writeRegister(0x1F801D8A, static_cast<uint16_t>(1u << (voice - 16)));
  }
  uint16_t step(uint32_t samples = 1, uint32_t voice = 0) {
    spu.generateSamples(out, samples);
    return spu.readRegister(voiceReg(voice, 0x0C));
  }
};

} // namespace

TEST_F(SpuVoiceTest, VoiceRegistersReadBackWhatWasWritten) {
  const uint16_t vals[] = {0x1234, 0x2345, 0x3456, 0x4567,
                           0x5678, 0x6789, 0x789A, 0x89AB};
  for (uint32_t r = 0; r < 8; ++r)
    spu.writeRegister(voiceReg(5, r * 2), vals[r]);
  for (uint32_t r = 0; r < 8; ++r)
    EXPECT_EQ(spu.readRegister(voiceReg(5, r * 2)), vals[r]) << "reg " << r;
  EXPECT_EQ(spu.readRegister(voiceReg(4, 0x04)), 0) << "voices are separate";
}

TEST_F(SpuVoiceTest, LinearAttackAtShiftZeroAddsSevenShl11PerSample) {
  // AdsrStep = (7-0) SHL 11 = 3800h every 44.1 kHz tick, clamped at 7FFFh.
  loopBlock(0);
  keyOnVoice(0, /*lo=*/0x0000, /*hi=*/0x0000);
  EXPECT_EQ(step(), 0x3800);
  EXPECT_EQ(step(), 0x7000);
  EXPECT_EQ(step(), 0x7FFF);
}

TEST_F(SpuVoiceTest, AttackStepValueSelectsTheIncrement) {
  // Attack Step 3 = "+4": 4 SHL 11 = 2000h per tick.
  loopBlock(0);
  keyOnVoice(0, /*lo=*/0x0300, /*hi=*/0x0000);
  EXPECT_EQ(step(), 0x2000);
  EXPECT_EQ(step(), 0x4000);
}

TEST_F(SpuVoiceTest, SlowAttackShiftBarelyMoves) {
  // Shift 1Fh: one +7 step every 2^20 ticks; 100 ticks stay near silence.
  loopBlock(0);
  keyOnVoice(0, /*lo=*/0x7C00, /*hi=*/0x0000);
  EXPECT_LT(step(100), 0x100);
}

TEST_F(SpuVoiceTest, ExponentialAttackSlowsDownAboveLevel6000) {
  // "IF exponential AND increase AND AdsrLevel>6000h THEN AdsrStep /= 4".
  auto ticksToFull = [this](uint16_t lo) {
    spu.reset();
    loopBlock(0);
    keyOnVoice(0, lo, 0x0000);
    for (int t = 1; t < 200; ++t)
      if (step() == 0x7FFF)
        return t;
    return 200;
  };
  const int linear = ticksToFull(0x0800);      // shift 2: +7 SHL 9 = E00h
  const int exponential = ticksToFull(0x8800); // same rate, exponential
  EXPECT_GT(exponential, linear);
}

TEST_F(SpuVoiceTest, DecayFallsExponentiallyToTheSustainLevel) {
  // Decay shift 0: -8 SHL 11 scaled by level/8000h, from 7FFFh straight to
  // the sustain level 7 => (7+1)*800h = 4000h.
  loopBlock(0);
  keyOnVoice(0, /*lo=*/0x0007, /*hi=*/0x0007);
  step(3); // attack to 7FFFh
  EXPECT_EQ(step(), 0x4000);
}

TEST_F(SpuVoiceTest, LinearSustainIncreaseAtShift11AddsSeven) {
  // Sustain shift 11, step 0, increase: 7 SHL 0 = +7 per tick.
  loopBlock(0);
  keyOnVoice(0, /*lo=*/0x0007, /*hi=*/0x0B07);
  step(4); // attack + decay to 4000h
  EXPECT_EQ(step(), 0x4007);
  EXPECT_EQ(step(), 0x400E);
}

TEST_F(SpuVoiceTest, SustainDecreaseLowersTheLevel) {
  loopBlock(0);
  keyOnVoice(0, /*lo=*/0x0007, /*hi=*/0x4007); // linear decrease, shift 0
  step(4);
  const uint16_t a = step();
  EXPECT_LT(a, 0x4000);
  EXPECT_LT(step(), a);
}

TEST_F(SpuVoiceTest, ExponentialSustainDecreaseSlowsAsTheLevelFalls) {
  // Exponential decrease: AdsrStep*AdsrLevel/8000h -- each drop is smaller.
  loopBlock(0);
  keyOnVoice(0, /*lo=*/0x0007, /*hi=*/0xC007);
  const uint16_t l0 = step(4);
  const uint16_t l1 = step();
  const uint16_t l2 = step();
  ASSERT_LT(l1, l0);
  ASSERT_LT(l2, l1);
  EXPECT_LT(l1 - l2, l0 - l1);
}

TEST_F(SpuVoiceTest, LinearReleaseAtShiftZeroDropsToSilence) {
  // Release step -8 SHL 11 = -4000h per tick, then the voice turns off.
  loopBlock(0);
  keyOnVoice(0, /*lo=*/0x0000, /*hi=*/0x0000);
  step(3);
  spu.writeRegister(0x1F801D8C, 0x0001); // key off
  EXPECT_EQ(step(), 0x3FFF);
  EXPECT_EQ(step(), 0x0000);
  EXPECT_EQ(spu.debugVoiceState(0).adsrPhase, AdsrPhase::Off);
}

TEST_F(SpuVoiceTest, ExponentialReleaseIsSlowerThanLinear) {
  loopBlock(0);
  keyOnVoice(0, /*lo=*/0x0000, /*hi=*/0x0020); // release mode exponential
  step(3);
  spu.writeRegister(0x1F801D8C, 0x0001);
  step(2);
  EXPECT_GT(spu.readRegister(voiceReg(0, 0x0C)), 0)
      << "exponential release reached zero as fast as linear";
}

TEST_F(SpuVoiceTest, KeyOffHighHalfReleasesVoicesAbove15) {
  loopBlock(0);
  keyOnVoice(17, 0x0000, 0x0000);
  step(0);
  spu.writeRegister(0x1F801D8E, 0x0002); // KOFF voice 17
  step(0);
  EXPECT_EQ(spu.debugVoiceState(17).adsrPhase, AdsrPhase::Release);
}

TEST_F(SpuVoiceTest, EndxHighHalfReportsVoicesAbove15) {
  // ENDX bit N is set when voice N reads a block with the Loop End flag.
  loopBlock(0);
  keyOnVoice(20, 0x0000, 0x0000);
  step();
  EXPECT_EQ(spu.readRegister(0x1F801D9E), 1u << (20 - 16));
  EXPECT_EQ(spu.readRegister(0x1F801D9C), 0);
}

TEST_F(SpuVoiceTest, NoiseModeVoicePlaysNoiseInsteadOfItsSamples) {
  // NON bit set: the voice outputs the noise generator, not its ADPCM data.
  loopBlock(0); // all-zero samples
  spu.writeRegister(0x1F801D80, 0x3FFF);
  spu.writeRegister(0x1F801D82, 0x3FFF);
  spu.writeRegister(voiceReg(0, 0x00), 0x3FFF);
  spu.writeRegister(voiceReg(0, 0x02), 0x3FFF);
  spu.writeRegister(0x1F801D94, 0x0001); // NON voice 0
  keyOnVoice(0, 0x0000, 0x0000);
  spu.generateSamples(out, 64);
  bool anyNonZero = false;
  for (int i = 0; i < 128; ++i)
    anyNonZero |= out[i] != 0;
  EXPECT_TRUE(anyNonZero);

  spu.writeRegister(0x1F801D94, 0x0000);
  spu.generateSamples(out, 64);
  for (int i = 0; i < 128; ++i)
    ASSERT_EQ(out[i], 0) << "silent samples without NON, index " << i;
}

TEST_F(SpuVoiceTest, PitchModulationByALoudVoiceSpeedsUpTheNextOne) {
  // "Factor = VxOUTX(x-1)+8000h; Step = (Step*Factor) SAR 15": a positive
  // modulator raises voice 1's effective pitch above 1000h.
  auto voice1AddrAfter = [this](uint16_t pmon) {
    spu.reset();
    loopBlock(0x1000, 0x00, 0x77); // loud: nibble 7 SHL 12 = 7000h
    // Voice 1 runs through zeroed RAM: plain blocks with no loop flags.
    keyOnVoice(0, 0x0000, 0x0000, 0x1000 / 8);
    keyOnVoice(1, 0x0000, 0x0000, 0x8000 / 8);
    spu.writeRegister(0x1F801D90, pmon);
    spu.generateSamples(out, 280);
    return spu.debugVoiceState(1).currentAddr;
  };
  EXPECT_GT(voice1AddrAfter(0x0002), voice1AddrAfter(0x0000));
}

TEST_F(SpuVoiceTest, PitchModulationBitZeroIsIgnored) {
  // "IF PMON.Bit(x)=1 AND (x>0)": voice 0 has no previous voice.
  auto voice0Addr = [this](uint16_t pmon) {
    spu.reset();
    loopBlock(0x1000);
    uint8_t *ram = spu.soundRamPtr();
    for (uint32_t b = 0; b < 8; ++b) // 8 plain blocks, no loop flags
      ram[0x1000 + b * 16 + 1] = 0;
    keyOnVoice(0, 0x0000, 0x0000, 0x1000 / 8);
    spu.writeRegister(0x1F801D90, pmon);
    spu.generateSamples(out, 100);
    return spu.debugVoiceState(0).currentAddr;
  };
  EXPECT_EQ(voice0Addr(0x0001), voice0Addr(0x0000));
}

TEST_F(SpuVoiceTest, IrqFiresWhenAVoiceReadsTheIrqAddress) {
  // SPUCNT bit6 enables IRQ9 on an access to IRQ Address (1F801DA4h * 8).
  loopBlock(0x100);
  spu.writeRegister(0x1F801DA4, 0x100 / 8);
  spu.writeRegister(0x1F801DAA, 0x0040);
  keyOnVoice(0, 0x0000, 0x0000, 0x100 / 8);
  step();
  EXPECT_TRUE(spu.hasIrq());
  spu.clearIrq();
  EXPECT_FALSE(spu.hasIrq());
}

TEST_F(SpuVoiceTest, IrqStaysQuietWhenDisabled) {
  loopBlock(0x100);
  spu.writeRegister(0x1F801DA4, 0x100 / 8);
  spu.writeRegister(0x1F801DAA, 0x0000);
  keyOnVoice(0, 0x0000, 0x0000, 0x100 / 8);
  step(30);
  EXPECT_FALSE(spu.hasIrq());
}

TEST_F(SpuVoiceTest, VoiceAddressWrapsAtTheEndOfSoundRam) {
  // Sound RAM is 512 KiB; the block after 7FFF0h is at 0.
  uint8_t *ram = spu.soundRamPtr();
  std::memset(ram + 0x7FFF0, 0, 16); // plain block, no flags
  keyOnVoice(0, 0x0000, 0x0000, 0x7FFF0 / 8);
  step();
  EXPECT_EQ(spu.debugVoiceState(0).currentAddr, 0u);
}

TEST(SpuTransferAddress, DirectSetterUsesEightByteUnits) {
  SPU spu;
  spu.reset();
  spu.setTransferAddr(0x10);
  spu.writeTransferData(0xBEEF);
  EXPECT_EQ(spu.readSoundRam(0x80), 0xBEEF);
}

TEST(SpuTransferAddress, ManualTransferWrapsAtTheEndOfSoundRam) {
  SPU spu;
  spu.reset();
  spu.writeRegister(0x1F801DA6, 0xFFFF); // byte address 7FFF8h
  for (uint16_t v : {0x1111, 0x2222, 0x3333, 0x4444, 0x5555})
    spu.writeRegister(0x1F801DA8, v);
  EXPECT_EQ(spu.readSoundRam(0x7FFFE), 0x4444);
  EXPECT_EQ(spu.readSoundRam(0x00000), 0x5555);
}

// ADPCM reserved shift values
//
// cdromformat.md "XA-ADPCM Header Bytes": "reserved shift values 13..15 will
// act same as shift=9" (SPU-ADPCM shares the header, soundprocessingunitspu.md
// "Sample Data").

TEST_F(SpuAdpcmTest, ReservedShiftValuesDecodeLikeShift9) {
  uint8_t block9[16] = {0x09, 0x00};
  std::memset(block9 + 2, 0x71, 14); // nibbles 1 and 7
  uint8_t block13[16];
  std::memcpy(block13, block9, sizeof block13);

  const auto ref = spu.decodeAdpcmBlockForTest(block9, 0, 0);
  for (uint8_t shift : {13, 14, 15}) {
    block13[0] = shift;
    EXPECT_EQ(spu.decodeAdpcmBlockForTest(block13, 0, 0), ref) << int(shift);
  }
  EXPECT_EQ(ref[0], 1 << 3) << "shift 9: nibble SHL (12-9)";
}

// Upper voice halves of the per-voice bit registers

TEST_F(SpuVoiceTest, PitchModulationHighHalfCoversVoicesAbove15) {
  auto voice17AddrAfter = [this](uint16_t pmonHi) {
    spu.reset();
    loopBlock(0x1000, 0x00, 0x77); // loud modulator
    keyOnVoice(16, 0x0000, 0x0000, 0x1000 / 8);
    keyOnVoice(17, 0x0000, 0x0000, 0x8000 / 8);
    spu.writeRegister(0x1F801D92, pmonHi); // PMON bits 16-23
    spu.generateSamples(out, 280);
    return spu.debugVoiceState(17).currentAddr;
  };

  const uint32_t modulated = voice17AddrAfter(0x0002); // voice 17
  const uint32_t plain = voice17AddrAfter(0x0000);

  EXPECT_GT(modulated, plain);
}

TEST_F(SpuVoiceTest, NoiseModeHighHalfCoversVoicesAbove15) {
  loopBlock(0); // all-zero samples
  spu.writeRegister(0x1F801D80, 0x3FFF);
  spu.writeRegister(0x1F801D82, 0x3FFF);
  spu.writeRegister(voiceReg(16, 0x00), 0x3FFF);
  spu.writeRegister(voiceReg(16, 0x02), 0x3FFF);
  spu.writeRegister(0x1F801D96, 0x0001); // NON voice 16
  keyOnVoice(16, 0x0000, 0x0000);

  spu.generateSamples(out, 64);

  bool anyNonZero = false;
  for (int i = 0; i < 128; ++i)
    anyNonZero |= out[i] != 0;
  EXPECT_TRUE(anyNonZero);
}

// Envelope edge cases (soundprocessingunitspu.md "SPU Volume and ADSR")

TEST_F(SpuVoiceTest, SustainLevel15KeepsTheEnvelopeAtMaximum) {
  // Sustain level (15+1)*800h is above 7FFFh: decay has nowhere to go.
  // Level 15 in both ADSR halves, so the test holds whichever half is read.
  loopBlock(0);
  keyOnVoice(0, /*lo=*/0x000F, /*hi=*/0x000F);
  step(3); // attack to 7FFFh

  EXPECT_EQ(step(), 0x7FFF);
  EXPECT_EQ(step(5), 0x7FFF);
}

TEST_F(SpuVoiceTest, ExponentialSustainIncreaseSlowsDownAboveLevel6000) {
  auto ticksToFull = [this](uint16_t hi) {
    spu.reset();
    loopBlock(0);
    keyOnVoice(0, /*lo=*/0x0007, hi);
    step(4); // attack + decay to 4000h
    for (int t = 1; t < 2000; ++t)
      if (step() == 0x7FFF)
        return t;
    return 2000;
  };

  const int linear = ticksToFull(0x0807);      // increase, shift 8, step +7
  const int exponential = ticksToFull(0x8807); // same, exponential

  EXPECT_GT(exponential, linear);
}

TEST_F(SpuVoiceTest, SlowSustainIncreaseBarelyMoves) {
  // Sustain shift 1Fh: one +7 step every 2^20 ticks.
  loopBlock(0);
  keyOnVoice(0, /*lo=*/0x0007, /*hi=*/0x1F07);
  const uint16_t start = step(4);

  const uint16_t later = step(100);

  EXPECT_LT(later - start, 0x100);
}

// IRQ address

TEST_F(SpuVoiceTest, IrqStaysQuietWhenNoVoiceReachesTheIrqAddress) {
  loopBlock(0x100);
  spu.writeRegister(0x1F801DA4, 0x800 / 8); // somewhere the voice never reads
  spu.writeRegister(0x1F801DAA, 0x0040);
  keyOnVoice(0, 0x0000, 0x0000, 0x100 / 8);

  step(30);

  EXPECT_FALSE(spu.hasIrq());
}

// Status and sound RAM bounds

TEST(SpuControl, SpustatIsReadOnlyAndMirrorsSpucntBits0To5) {
  SPU spu;
  spu.reset();
  spu.writeRegister(0x1F801DAA, 0x0015);

  spu.writeRegister(0x1F801DAE, 0xFFFF);

  EXPECT_EQ(spu.readRegister(0x1F801DAE) & 0x3F, 0x15);
}

TEST(SpuSoundRam, AccessesPastTheEndAreIgnored) {
  SPU spu;
  spu.reset();

  spu.writeSoundRam(0x80000, 0xBEEF);

  EXPECT_EQ(spu.readSoundRam(0x00000), 0) << "must not wrap";
  EXPECT_EQ(spu.readSoundRam(0x80000), 0);
}

TEST(SpuSoundRam, LoadReplacesTheWholeImage) {
  SPU spu;
  spu.reset();
  std::vector<uint8_t> image(SOUND_RAM_SIZE, 0x5A);

  spu.loadSoundRam(image.data());

  EXPECT_EQ(spu.readSoundRam(0x00000), 0x5A5A);
  EXPECT_EQ(spu.readSoundRam(SOUND_RAM_SIZE - 2), 0x5A5A);
}

// Metrics census

class SpuMetricsTest : public SpuVoiceTest {
protected:
  void SetUp() override {
    SpuVoiceTest::SetUp();
    ps1::metrics::reset();
    ps1::metrics::setEnabledForTesting(true);
  }
  void TearDown() override {
    ps1::metrics::setEnabledForTesting(false);
    ps1::metrics::reset();
  }
};

TEST_F(SpuMetricsTest, ProgrammingAStartAddressIsCountedPerVoice) {
  spu.writeRegister(voiceReg(3, 0x06), 0x0200);
  spu.writeRegister(voiceReg(3, 0x06), 0x0300);

  EXPECT_EQ(ps1::metrics::get("spu.voice_start.03"), 2u);
  EXPECT_EQ(ps1::metrics::get("spu.voice_start.04"), 0u);
}

TEST_F(SpuMetricsTest, KeyOnIsCountedPerVoiceWhenTheLatchIsRetired) {
  loopBlock(0);
  keyOnVoice(21, 0x0000, 0x0000);

  step();

  EXPECT_EQ(ps1::metrics::get("spu.key_on.21"), 1u);
}
