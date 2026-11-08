#include "runtime/input/input.h"
#include "runtime/metrics.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
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

  input.writeRegister(0x1F801040, 0x01);
  for (int i = 0; i < 4; ++i)
    input.writeRegister(0x1F801040, i == 0 ? 0x42 : 0x00); // cmd, TAP, lo, hi
  std::vector<uint32_t> sticks;
  for (int i = 0; i < 4; ++i) {
    input.writeRegister(0x1F801040, 0x00);
    sticks.push_back(input.readRegister(0x1F801040));
  }

  // psx-spx: RightX, RightY, LeftX, LeftY.
  EXPECT_EQ(sticks, (std::vector<uint32_t>{0x40, 0xC0, 0x00, 0xFF}));
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

TEST(InputSIO, JoyCtrlResetAbortsAnExchange) {
  InputController input;
  input.writeRegister(0x1F801040, 0x01); // pad selected, waiting for 42h

  input.writeRegister16(0x1F80104A, 0x0040); // JOY_CTRL.6 reset
  input.writeRegister(0x1F801040, 0x42);

  EXPECT_EQ(input.readRegister(0x1F801040), 0xFFu) << "42h without 01h is ignored";
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

// The stick and the d-pad can hold the same direction.  Releasing one must not
// release what the other still holds.
TEST(InputHeldButtons, ReleaseOnOneSourceKeepsTheOther) {
  InputController ic;
  HeldButtons held;
  held.set(HeldButtons::Stick, BTN_LEFT, true, ic);
  held.set(HeldButtons::PadButtons, BTN_LEFT, true, ic);
  held.set(HeldButtons::Stick, BTN_LEFT, false, ic);
  ic.latchFrame();
  EXPECT_EQ(ic.buttonState() & BTN_LEFT, 0) << "left released while d-pad holds it";
  held.set(HeldButtons::PadButtons, BTN_LEFT, false, ic);
  ic.latchFrame();
  EXPECT_EQ(ic.buttonState() & BTN_LEFT, BTN_LEFT);
}

// Lockstep recording reads what the host is holding without feeding the game.
TEST(InputHeldButtons, MaskIsTheUnionOfAllSources) {
  InputController ic;
  HeldButtons held;
  held.set(HeldButtons::Keyboard, BTN_CROSS, true, ic);
  held.set(HeldButtons::Stick, BTN_UP, true, ic);
  EXPECT_EQ(held.mask(), BTN_CROSS | BTN_UP);
  held.set(HeldButtons::Keyboard, BTN_CROSS, false, ic);
  EXPECT_EQ(held.mask(), BTN_UP);
}

// Controller SIO exchange
//
// Contract: psx-spx controllersandmemorycards.md "Controller Communication
// Sequence": 01h/Hi-Z, 42h/ID-lo, TAP/5Ah, then the button halves (active
// low), then for analog pads RightX, RightY, LeftX, LeftY.

namespace {
std::vector<uint8_t> padExchange(InputController &input, size_t replies) {
  std::vector<uint8_t> got;
  const uint8_t send[] = {0x01, 0x42};
  for (size_t i = 0; i < replies; ++i) {
    input.writeRegister(0x1F801040, i < 2 ? send[i] : 0x00);
    got.push_back(static_cast<uint8_t>(input.readRegister(0x1F801040)));
  }
  return got;
}
} // namespace

TEST(InputSIO, DigitalPadAnswersIdThenButtons) {
  InputController input;
  input.press(BTN_CROSS);
  input.press(BTN_UP);

  const auto got = padExchange(input, 5);

  const uint16_t buttons = static_cast<uint16_t>(~(BTN_CROSS | BTN_UP));
  EXPECT_EQ(got, (std::vector<uint8_t>{0xFF, 0x41, 0x5A,
                                       static_cast<uint8_t>(buttons & 0xFF),
                                       static_cast<uint8_t>(buttons >> 8)}));
}

TEST(InputSIO, DigitalPadReturnsToIdleAfterTheButtons) {
  InputController input;
  padExchange(input, 5);

  const auto again = padExchange(input, 3);

  EXPECT_EQ(again, (std::vector<uint8_t>{0xFF, 0x41, 0x5A}));
}

TEST(InputSIO, AnalogPadReportsItsIdAndCenteredSticks) {
  InputController input;
  input.setPadType(0, PadType::DualShock);

  const auto got = padExchange(input, 9);

  EXPECT_EQ(got[1], 0x79);
  EXPECT_EQ((std::vector<uint8_t>(got.begin() + 5, got.end())),
            (std::vector<uint8_t>{0x80, 0x80, 0x80, 0x80}));
}

TEST(InputSIO, AnUnknownDeviceCommandAbortsTheExchange) {
  InputController input;
  input.writeRegister(0x1F801040, 0x01);

  input.writeRegister(0x1F801040, 0x77);
  const uint32_t reply = input.readRegister(0x1F801040);
  const auto next = padExchange(input, 2);

  EXPECT_EQ(reply, 0xFFu);
  EXPECT_EQ(next[1], 0x41) << "a fresh 01h starts over";
}

TEST(InputSIO, BytesOtherThanAnAddressAreIgnoredWhenIdle) {
  InputController input;

  input.writeRegister(0x1F801040, 0x42);

  EXPECT_EQ(input.readRegister(0x1F801040), 0xFFu);
}

TEST(InputSIO, JoyCtrlBit13SelectsTheSecondPort) {
  InputController input;
  input.press(BTN_START, 1);
  input.writeRegister16(0x1F80104A, 1u << 13);

  const auto got = padExchange(input, 4);

  EXPECT_EQ(got[3], static_cast<uint8_t>(~BTN_START & 0xFF));
}

TEST(InputSIO, EachByteRaisesTheAckInterruptUntilAcknowledged) {
  InputController input;

  input.writeRegister(0x1F801040, 0x01);
  const bool raised = input.hasInterrupt();
  input.writeRegister16(0x1F80104A, 1u << 4); // JOY_CTRL.4 acknowledge

  EXPECT_TRUE(raised);
  EXPECT_FALSE(input.hasInterrupt());
}

TEST(InputSIO, ModeControlAndBaudRegistersReadBack) {
  InputController input;

  input.writeRegister16(0x1F801048, 0x000D);
  input.writeRegister16(0x1F80104A, 0x1003);
  input.writeRegister16(0x1F80104E, 0x0088);

  EXPECT_EQ(input.readRegister16(0x1F801048), 0x000D);
  EXPECT_EQ(input.readRegister16(0x1F80104A), 0x1003);
  EXPECT_EQ(input.readRegister16(0x1F80104E), 0x0088);
  EXPECT_EQ(input.readRegister16(0x1F801044), input.readRegister(0x1F801044));
}

TEST(InputSIO, UnmappedRegistersReadZero) {
  InputController input;

  EXPECT_EQ(input.readRegister16(0x1F80104C), 0);
  EXPECT_EQ(input.readRegister(0x1F801048), 0u);
}

TEST(InputSIO, MemoryCardIsReachedThroughTheSioPort) {
  InputController input;
  std::vector<uint8_t> got;

  for (uint8_t b : {0x81, 0x53, 0x00, 0x00}) {
    input.writeRegister(0x1F801040, b);
    got.push_back(static_cast<uint8_t>(input.readRegister(0x1F801040)));
  }

  // 81h/Hi-Z, 53h(GetID)/FLAG, then the card IDs 5Ah 5Dh.
  EXPECT_EQ(got, (std::vector<uint8_t>{0xFF, 0x08, 0x5A, 0x5D}));
}

TEST(InputSIO, PortReturnsToThePadOnceTheCardCommandEnds) {
  InputController input;
  for (int i = 0; i < 2 + 8; ++i) // GetID: address, command, 8 replies
    input.writeRegister(0x1F801040, i == 0 ? 0x81 : i == 1 ? 0x53 : 0x00);

  const auto pad = padExchange(input, 2);

  EXPECT_EQ(pad[1], 0x41);
}

// Port bounds

TEST(InputPorts, OutOfRangePortsAreIgnored) {
  InputController input;

  input.press(BTN_CROSS, 2);
  input.release(BTN_CROSS, -1);
  input.setAnalog(2, 0, 0, 0, 0);
  input.setPadType(-1, PadType::Analog);

  EXPECT_EQ(input.buttonState(2), 0xFFFF);
  EXPECT_EQ(input.getPadType(2), PadType::None);
  EXPECT_EQ(input.buttonState(0), 0xFFFF);
  EXPECT_EQ(input.getPadType(0), PadType::Digital);
}

// Memory card backing file

namespace {
std::string uniqueCardPath(const char *tag) {
  static int counter = 0;
  return (std::filesystem::temp_directory_path() /
          ("ps1_test_card_" + std::to_string(::getpid()) + "_" +
           std::to_string(++counter) + "_" + tag + ".mcd"))
      .string();
}

// Writes `fill` to `sector` through the SIO write command, with a good
// checksum.  Returns the final status byte.
uint8_t writeSectorViaSio(MemoryCard &card, uint16_t sector, uint8_t fill) {
  std::vector<uint8_t> send{0x81, 0x57, 0x00, 0x00,
                            static_cast<uint8_t>(sector >> 8),
                            static_cast<uint8_t>(sector & 0xFF)};
  uint8_t chk = static_cast<uint8_t>((sector >> 8) ^ (sector & 0xFF));
  for (int i = 0; i < 128; ++i) {
    send.push_back(fill);
    chk ^= fill;
  }
  send.push_back(chk);
  send.insert(send.end(), {0x00, 0x00, 0x00});
  uint8_t last = 0;
  for (uint8_t b : send)
    last = card.transfer(b);
  return last;
}
} // namespace

TEST(MemoryCardFile, AttachingAMissingFileCreatesAFormattedImage) {
  const std::string path = uniqueCardPath("create");
  MemoryCard card;

  const bool ok = card.attachFile(path);

  EXPECT_TRUE(ok);
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  EXPECT_EQ(static_cast<uint32_t>(f.tellg()), MemoryCard::CARD_SIZE);
  std::filesystem::remove(path);
}

TEST(MemoryCardFile, AttachingAnExistingFileLoadsIt) {
  const std::string path = uniqueCardPath("load");
  {
    MemoryCard writer;
    writer.image()[0x2000] = 0xAB;
    ASSERT_TRUE(writer.saveToFile(path));
  }
  MemoryCard card;

  ASSERT_TRUE(card.attachFile(path));

  EXPECT_EQ(card.image()[0x2000], 0xAB);
  std::filesystem::remove(path);
}

TEST(MemoryCardFile, ASioWriteIsFlushedToTheBackingFile) {
  const std::string path = uniqueCardPath("flush");
  MemoryCard card;
  ASSERT_TRUE(card.attachFile(path));

  ASSERT_EQ(writeSectorViaSio(card, 0x40, 0x5E), 0x47);

  MemoryCard reloaded;
  ASSERT_TRUE(reloaded.loadFromFile(path));
  EXPECT_EQ(reloaded.image()[0x40 * 128], 0x5E);
  std::filesystem::remove(path);
}

TEST(MemoryCardProtocol, WriteToAnOutOfRangeSectorEndsWithBadSector) {
  // psx-spx "Writing Data to Memory Card": the end byte is FFh for a bad
  // sector (valid sectors are 0..3FFh).
  MemoryCard card;

  EXPECT_EQ(writeSectorViaSio(card, 0x0400, 0x11), 0xFF);
}

TEST(MemoryCardFile, LoadingAMissingFileFails) {
  MemoryCard card;

  EXPECT_FALSE(card.loadFromFile(uniqueCardPath("missing")));
}

TEST(MemoryCardFile, SavingIntoAMissingDirectoryFails) {
  MemoryCard card;

  EXPECT_FALSE(card.saveToFile("/nonexistent_dir_ps1_test/card.mcd"));
}

TEST(MemoryCardFile, FlushWithoutABackingFileKeepsTheImage) {
  MemoryCard card;
  uint8_t sector[128];
  std::memset(sector, 0x33, sizeof sector);

  card.writeSector(5, sector);
  card.flush();

  uint8_t back[128];
  card.readSector(5, back);
  EXPECT_EQ(back[0], 0x33);
  EXPECT_EQ(back[127], 0x33);
}

// Metrics

TEST(MemoryCardMetrics, CommandsAreCountedWhenMetricsAreOn) {
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(true);
  MemoryCard card;

  for (uint8_t cmd : {0x52, 0x57, 0x53, 0x99}) {
    card.transfer(0x81);
    card.transfer(cmd);
    card.reset(); // abandon the command
  }

  EXPECT_EQ(ps1::metrics::get("memcard.cmd.read"), 1u);
  EXPECT_EQ(ps1::metrics::get("memcard.cmd.write"), 1u);
  EXPECT_EQ(ps1::metrics::get("memcard.cmd.getid"), 1u);
  EXPECT_EQ(ps1::metrics::get("memcard.cmd.unknown"), 1u);
  ps1::metrics::setEnabledForTesting(false);
  ps1::metrics::reset();
}
