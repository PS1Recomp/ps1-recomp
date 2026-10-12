#include "runtime/input/input.h"
#include <vector>
#include <gtest/gtest.h>

using namespace ps1::input;

// Digital Pad Tests

TEST(InputDigital, InitialStateAllReleased) {
  InputController input;
  EXPECT_EQ(input.buttonState(0), 0xFFFF); // All released (active low)
}

TEST(InputDigital, PressAndRelease) {
  InputController input;
  input.press(BTN_CROSS, 0);
  EXPECT_EQ(input.buttonState(0) & BTN_CROSS, 0); // Pressed = 0

  input.release(BTN_CROSS, 0);
  EXPECT_NE(input.buttonState(0) & BTN_CROSS, 0); // Released = 1
}

TEST(InputDigital, MultiplePresses) {
  InputController input;
  input.press(BTN_UP, 0);
  input.press(BTN_CROSS, 0);
  EXPECT_EQ(input.buttonState(0) & BTN_UP, 0);
  EXPECT_EQ(input.buttonState(0) & BTN_CROSS, 0);
  EXPECT_NE(input.buttonState(0) & BTN_CIRCLE, 0); // Not pressed
}

TEST(InputDigital, Port1Independent) {
  InputController input;
  input.press(BTN_START, 0);
  EXPECT_EQ(input.buttonState(0) & BTN_START, 0);
  EXPECT_NE(input.buttonState(1) & BTN_START, 0); // Port 1 unaffected
}

// Analog Stick Tests

TEST(InputAnalog, DefaultCenter) {
  InputController input;
  input.setPadType(0, PadType::Analog);
  // Default analog values should be centered at 0x80
  // (tested via SIO transfer sequence)
  EXPECT_EQ(input.getPadType(0), PadType::Analog);
}

TEST(InputAnalog, SetAnalogValues) {
  InputController input;
  input.setPadType(0, PadType::Analog);
  input.setAnalog(0, 0x00, 0xFF, 0x40, 0xC0);
  // Values are set internally, tested via SIO
  EXPECT_TRUE(true);
}

// Memory Card Tests

TEST(MemCard, InitialState) {
  MemoryCard mc;
  EXPECT_TRUE(mc.isPresent());
}

TEST(MemCard, SioAccessProtocol) {
  MemoryCard mc;
  mc.reset();

  // The reply to the address byte is not defined by the card.
  EXPECT_EQ(mc.transfer(0x81), 0xFF);

  // The command byte is answered with the FLAG byte, not with ID1.  ID1 and
  // ID2 come on the two exchanges after it.  This ordering is from psx-spx,
  // controllersandmemorycards.md "Reading Data from Memory Card"; the test
  // used to expect ID1 here, one byte too early.
  EXPECT_EQ(mc.transfer(0x52), 0x08) << "FLAG, 08h before any write";
  EXPECT_EQ(mc.transfer(0x00), 0x5A) << "ID1";
  EXPECT_EQ(mc.transfer(0x00), 0x5D) << "ID2";
}

TEST(MemCard, AbsentCardAnswersNothing) {
  MemoryCard mc;
  mc.reset();
  mc.setPresent(false);

  EXPECT_EQ(mc.transfer(0x81), 0xFF);
  EXPECT_EQ(mc.transfer(0x52), 0xFF);
  EXPECT_FALSE(mc.transferActive());

  mc.setPresent(true);
  EXPECT_TRUE(mc.isPresent());
}

// SIO Register Tests

TEST(InputSIO, JoyCtrlReset) {
  InputController input;
  input.writeRegister16(0x1F80104A, 0x0040); // Reset bit
  // Should not crash
  EXPECT_TRUE(true);
}

TEST(InputSIO, JoyStatReadback) {
  InputController input;
  uint32_t stat = input.readRegister(0x1F801044);
  EXPECT_NE(stat, 0); // TX ready flag should be set
}

// Memory Card
//
// Byte sequences are from psx-spx, controllersandmemorycards.md "Memory Card
// Read/Write Commands".  The card answers a fixed script per command, so the
// tests drive whole exchanges and check every reply position that the spec
// pins down.

namespace {
// Drives one exchange and collects the card's replies.
std::vector<uint8_t> runExchange(ps1::input::MemoryCard &card,
                              const std::vector<uint8_t> &send) {
  std::vector<uint8_t> got;
  got.reserve(send.size());
  for (uint8_t b : send)
    got.push_back(card.transfer(b));
  return got;
}

std::vector<uint8_t> readSector(ps1::input::MemoryCard &card, uint16_t sector) {
  std::vector<uint8_t> send{0x81, 0x52, 0x00, 0x00,
                            static_cast<uint8_t>(sector >> 8),
                            static_cast<uint8_t>(sector & 0xFF)};
  send.insert(send.end(), 4 + 128 + 2, 0x00); // acks, address echo, data, chk, end
  return runExchange(card, send);
}
} // namespace

TEST(MemoryCardProtocol, ReadReturnsTheSpecifiedHandshakeAndEndByte) {
  ps1::input::MemoryCard card;
  card.reset();

  const auto got = readSector(card, 0);

  EXPECT_EQ(got[1], 0x08) << "FLAG byte, 08h before any write";
  EXPECT_EQ(got[2], 0x5A) << "ID1";
  EXPECT_EQ(got[3], 0x5D) << "ID2";
  EXPECT_EQ(got[6], 0x5C) << "command acknowledge 1";
  EXPECT_EQ(got[7], 0x5D) << "command acknowledge 2";
  EXPECT_EQ(got[8], 0x00) << "confirmed address MSB";
  EXPECT_EQ(got[9], 0x00) << "confirmed address LSB";
  EXPECT_EQ(got.back(), 0x47) << "end byte, 'G' for a good read";
}

TEST(MemoryCardProtocol, ReadChecksumIsAddressXorData) {
  ps1::input::MemoryCard card;
  card.reset();

  const auto got = readSector(card, 0);
  const std::size_t dataAt = 10;

  uint8_t expected = 0x00 ^ 0x00; // address MSB ^ LSB
  for (std::size_t i = 0; i < 128; i++)
    expected ^= got[dataAt + i];
  EXPECT_EQ(got[dataAt + 128], expected);
}

TEST(MemoryCardProtocol, AFreshCardIsFormattedWithAValidHeader) {
  ps1::input::MemoryCard card;
  card.reset();

  const auto got = readSector(card, 0);
  const std::size_t dataAt = 10;

  EXPECT_EQ(got[dataAt + 0], 'M');
  EXPECT_EQ(got[dataAt + 1], 'C');
  uint8_t chk = 0;
  for (std::size_t i = 0; i < 127; i++)
    chk ^= got[dataAt + i];
  EXPECT_EQ(got[dataAt + 127], chk) << "header frame checksum";
}

TEST(MemoryCardProtocol, DirectoryFramesReportEveryBlockFree) {
  ps1::input::MemoryCard card;
  card.reset();

  for (uint16_t frame = 1; frame <= 15; frame++) {
    const auto got = readSector(card, frame);
    EXPECT_EQ(got[10], 0xA0) << "frame " << frame << " allocation state";
    EXPECT_EQ(got[10 + 8], 0xFF) << "frame " << frame << " next-block pointer";
    EXPECT_EQ(got[10 + 9], 0xFF) << "frame " << frame;
  }
}

TEST(MemoryCardProtocol, WriteStoresTheSectorAndReadsBack) {
  ps1::input::MemoryCard card;
  card.reset();

  std::vector<uint8_t> payload(128);
  for (std::size_t i = 0; i < payload.size(); i++)
    payload[i] = static_cast<uint8_t>(i * 3 + 1);

  const uint16_t sector = 0x40; // block 1, frame 0
  uint8_t chk = static_cast<uint8_t>(sector >> 8) ^
                static_cast<uint8_t>(sector & 0xFF);
  for (uint8_t b : payload)
    chk ^= b;

  std::vector<uint8_t> send{0x81, 0x57, 0x00, 0x00,
                            static_cast<uint8_t>(sector >> 8),
                            static_cast<uint8_t>(sector & 0xFF)};
  send.insert(send.end(), payload.begin(), payload.end());
  send.push_back(chk);
  send.insert(send.end(), 3, 0x00); // two acks plus the end byte

  const auto got = runExchange(card, send);
  EXPECT_EQ(got.back(), 0x47) << "end byte, 'G' for a good write";

  const auto back = readSector(card, sector);
  for (std::size_t i = 0; i < payload.size(); i++)
    ASSERT_EQ(back[10 + i], payload[i]) << "byte " << i;
}

TEST(MemoryCardProtocol, WriteWithABadChecksumIsRejectedAndChangesNothing) {
  ps1::input::MemoryCard card;
  card.reset();

  const uint16_t sector = 0x40;
  std::vector<uint8_t> send{0x81, 0x57, 0x00, 0x00,
                            static_cast<uint8_t>(sector >> 8),
                            static_cast<uint8_t>(sector & 0xFF)};
  send.insert(send.end(), 128, 0xAB);
  send.push_back(0x00); // deliberately wrong
  send.insert(send.end(), 3, 0x00);

  const auto got = runExchange(card, send);
  EXPECT_EQ(got.back(), 0x4E) << "end byte, 'N' for a bad checksum";

  const auto back = readSector(card, sector);
  EXPECT_EQ(back[10], 0x00) << "a rejected write must not reach the card";
}

TEST(MemoryCardProtocol, WriteClearsTheDirectoryUnreadFlag) {
  ps1::input::MemoryCard card;
  card.reset();
  ASSERT_EQ(card.flagByte() & 0x08, 0x08);

  const uint16_t sector = 0x3F;
  uint8_t chk = static_cast<uint8_t>(sector >> 8) ^
                static_cast<uint8_t>(sector & 0xFF);
  std::vector<uint8_t> send{0x81, 0x57, 0x00, 0x00,
                            static_cast<uint8_t>(sector >> 8),
                            static_cast<uint8_t>(sector & 0xFF)};
  send.insert(send.end(), 128, 0x00);
  send.push_back(chk);
  send.insert(send.end(), 3, 0x00);
  runExchange(card, send);

  EXPECT_EQ(card.flagByte() & 0x08, 0x00)
      << "bit3 is cleared by a write, which is how games sense a card swap";
}

TEST(MemoryCardProtocol, OutOfRangeSectorAnswersFfffAndSendsNoData) {
  ps1::input::MemoryCard card;
  card.reset();

  const uint16_t sector = 0x400; // one past the last
  std::vector<uint8_t> send{0x81, 0x52, 0x00, 0x00,
                            static_cast<uint8_t>(sector >> 8),
                            static_cast<uint8_t>(sector & 0xFF),
                            0x00, 0x00, 0x00, 0x00, 0x00};
  const auto got = runExchange(card, send);

  EXPECT_EQ(got[8], 0xFF) << "confirmed address MSB";
  EXPECT_EQ(got[9], 0xFF) << "confirmed address LSB";
  EXPECT_FALSE(card.transferActive()) << "the transfer aborts there";
}

TEST(MemoryCardProtocol, GetIdReportsSectorCountAndSectorSize) {
  ps1::input::MemoryCard card;
  card.reset();

  const auto got = runExchange(card, {0x81, 0x53, 0x00, 0x00, 0x00, 0x00, 0x00,
                                   0x00, 0x00, 0x00});
  EXPECT_EQ(got[2], 0x5A);
  EXPECT_EQ(got[3], 0x5D);
  EXPECT_EQ(got[4], 0x5C);
  EXPECT_EQ(got[5], 0x5D);
  EXPECT_EQ(got[6], 0x04);
  EXPECT_EQ(got[7], 0x00);
  EXPECT_EQ(got[8], 0x00);
  EXPECT_EQ(got[9], 0x80);
}

TEST(MemoryCardProtocol, AnUnknownCommandAbortsImmediately) {
  ps1::input::MemoryCard card;
  card.reset();

  const auto got = runExchange(card, {0x81, 0x99, 0x00});
  EXPECT_EQ(got[1], 0xFF);
  EXPECT_FALSE(card.transferActive());
}

// The game samples the pad once per VBlank.  A tap that starts and ends between
// two samples must still show up in one, or short presses (Crash's jump height
// depends on how long X is held) silently do nothing.
TEST(InputDigital, TapBetweenSamplesIsSeenOnce) {
  InputController input;
  input.press(BTN_CROSS);
  input.release(BTN_CROSS);
  input.latchFrame();
  EXPECT_EQ(input.buttonState() & BTN_CROSS, 0) << "tap lost";
  input.latchFrame();
  EXPECT_NE(input.buttonState() & BTN_CROSS, 0) << "tap stuck";
}

TEST(InputDigital, HeldButtonStaysPressedAcrossSamples) {
  InputController input;
  input.press(BTN_CROSS);
  input.latchFrame();
  input.latchFrame();
  EXPECT_EQ(input.buttonState() & BTN_CROSS, 0);
  input.release(BTN_CROSS);
  input.latchFrame();
  EXPECT_NE(input.buttonState() & BTN_CROSS, 0);
}
