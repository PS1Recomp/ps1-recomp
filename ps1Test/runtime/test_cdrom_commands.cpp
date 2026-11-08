// Tests: CD-ROM controller commands.
//
// Contract: psx-spx "CDROM Drive" (cdromdrive.md) -- the register banks
// (HSTS, RESULT, HINTMSK/HINTSTS, HCLRCTL), the command summary table
// (INT3 first response, INT2 second response) and "CDROM Controller Status
// Byte" (bit1 motor, bit5 read, bit6 seek, bit7 play).
#include "runtime/cdrom/cdrom_controller.h"
#include <cstring>
#include <gtest/gtest.h>
#include <initializer_list>
#include <optional>
#include <vector>

using namespace ps1::cdrom;

namespace {

constexpr uint32_t kAddr = 0x1F801800;
constexpr uint32_t kCmd = 0x1F801801;
constexpr uint32_t kParam = 0x1F801802;
constexpr uint32_t kFlag = 0x1F801803;

// Status byte bits (psx-spx "CDROM Controller Status Byte").
constexpr uint8_t kStatMotor = 1u << 1;
constexpr uint8_t kStatRead = 1u << 5;
constexpr uint8_t kStatSeek = 1u << 6;
constexpr uint8_t kStatPlay = 1u << 7;

class FilledFs : public VirtualFs {
public:
  std::optional<Sector> readSector(uint32_t lba) override {
    Sector s{};
    std::memset(s.raw, static_cast<int>(lba & 0xFF), SECTOR_SIZE_RAW);
    lastLba = lba;
    return s;
  }
  uint32_t lastLba = 0xFFFFFFFF;
};

class CdromCommandTest : public ::testing::Test {
protected:
  CdromController cd;
  std::vector<uint8_t> irqs; // every interrupt type raised, in order

  void SetUp() override {
    cd.reset();
    cd.setInterruptCallback([this](uint8_t t) { irqs.push_back(t); });
  }

  void send(uint8_t cmd, std::initializer_list<uint8_t> params = {}) {
    cd.writeRegister(kAddr, 0);
    for (uint8_t p : params)
      cd.writeRegister(kParam, p);
    cd.writeRegister(kCmd, cmd);
  }
  // HINTSTS bits 0-2, read through bank 1.
  uint8_t intType() {
    cd.writeRegister(kAddr, 1);
    const uint8_t v = cd.readRegister(kFlag);
    cd.writeRegister(kAddr, 0);
    return v & 7;
  }
  // Drain RESULT while HSTS.RSLRRDY says it holds data.
  std::vector<uint8_t> response() {
    std::vector<uint8_t> out;
    while (cd.readRegister(kAddr) & (1u << 5))
      out.push_back(cd.readRegister(kCmd));
    return out;
  }
  // HCLRCTL: acknowledge all HC05 flags (bank 1, port 3).
  void ack() {
    cd.writeRegister(kAddr, 1);
    cd.writeRegister(kFlag, 0x1F);
    cd.writeRegister(kAddr, 0);
  }
  void setLoc(uint8_t mm, uint8_t ss, uint8_t ff) {
    send(0x02, {mm, ss, ff});
    response();
    ack();
  }
};

TEST_F(CdromCommandTest, GetstatAcknowledgesWithTheMotorOnStatus) {
  send(0x01);
  EXPECT_EQ(intType(), INT_ACKNOWLEDGE);
  const auto r = response();
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0], kStatMotor);
}

TEST_F(CdromCommandTest, AcknowledgeClearsTheInterruptFlag) {
  send(0x01);
  ack();
  EXPECT_EQ(intType(), INT_NONE);
}

TEST_F(CdromCommandTest, HintstsReservedBitsReadAsOne) {
  // psx-spx HINTSTS: bits 5-7 "Reserved (always 1)".
  send(0x01);
  cd.writeRegister(kAddr, 1);
  EXPECT_EQ(cd.readRegister(kFlag), 0xE0 | INT_ACKNOWLEDGE);
}

TEST_F(CdromCommandTest, HintmskReadsBackWithReservedBitsSet) {
  // psx-spx HINTMSK read (banks 0 and 2): bits 5-7 always 1; reset value 1Fh.
  cd.writeRegister(kAddr, 0);
  EXPECT_EQ(cd.readRegister(kFlag), 0xFF);
}

TEST_F(CdromCommandTest, HstsReportsTheBankAndFifoState) {
  cd.writeRegister(kAddr, 2);
  EXPECT_EQ(cd.readRegister(kAddr) & 3, 2);
  cd.writeRegister(kAddr, 0);
  EXPECT_NE(cd.readRegister(kAddr) & (1u << 4), 0) << "PRMWRDY: room left";
  EXPECT_EQ(cd.readRegister(kAddr) & (1u << 5), 0) << "RSLRRDY: no result";
  send(0x01);
  EXPECT_NE(cd.readRegister(kAddr) & (1u << 5), 0) << "RSLRRDY after INT3";
}

TEST_F(CdromCommandTest, FullParameterFifoClearsPrmwrdy) {
  // psx-spx: the FIFO holds 16 bytes; "once full, the decoder will clear the
  // PRMWRDY flag".
  cd.writeRegister(kAddr, 0);
  for (int i = 0; i < 16; ++i)
    cd.writeRegister(kParam, static_cast<uint8_t>(i));
  EXPECT_EQ(cd.readRegister(kAddr) & (1u << 4), 0);
}

TEST_F(CdromCommandTest, HclrctlBit6ClearsTheParameterFifo) {
  // psx-spx HCLRCTL bit6 CLRPRM: with the stale byte gone, 15 more still fit.
  cd.writeRegister(kAddr, 0);
  cd.writeRegister(kParam, 0x11);
  cd.writeRegister(kAddr, 1);
  cd.writeRegister(kFlag, 0x40);
  cd.writeRegister(kAddr, 0);
  for (int i = 0; i < 15; ++i)
    cd.writeRegister(kParam, 0);
  EXPECT_NE(cd.readRegister(kAddr) & (1u << 4), 0);
}

TEST_F(CdromCommandTest, SeeklAcknowledgesThenCompletes) {
  // psx-spx command table: SeekL -> INT3(stat), INT2(stat).
  setLoc(0x00, 0x02, 0x16);
  irqs.clear();
  send(0x15);
  EXPECT_EQ(intType(), INT_ACKNOWLEDGE);
  EXPECT_NE(response().at(0) & kStatSeek, 0) << "seeking while acknowledged";
  ack();
  EXPECT_EQ(intType(), INT_COMPLETE);
  EXPECT_EQ(response().size(), 1u);
  EXPECT_EQ(irqs, (std::vector<uint8_t>{INT_ACKNOWLEDGE, INT_COMPLETE}));
}

TEST_F(CdromCommandTest, SeekpAlsoCompletesWithInt2) {
  setLoc(0x00, 0x02, 0x00);
  send(0x16);
  EXPECT_EQ(intType(), INT_ACKNOWLEDGE);
  ack();
  EXPECT_EQ(intType(), INT_COMPLETE);
}

TEST_F(CdromCommandTest, SecondResponseIsDeliveredByTimeWithoutAnAck) {
  // A command's INT2 also arrives on its own once the mechanical delay passes.
  setLoc(0x00, 0x02, 0x00);
  send(0x15);
  response();
  cd.ackInterrupt(0x1F); // clear without the HCLRCTL fast path
  irqs.clear();
  cd.tick(1000);
  EXPECT_TRUE(irqs.empty()) << "INT2 before the seek delay elapsed";
  cd.tick(1000000);
  EXPECT_EQ(irqs, (std::vector<uint8_t>{INT_COMPLETE}));
  EXPECT_EQ(cd.interruptFlag(), INT_COMPLETE);
}

TEST_F(CdromCommandTest, GetloclReportsTheSeekTargetInBcd) {
  // psx-spx GetlocL -> INT3(amm,ass,asect,mode,file,channel,sm,ci).
  setLoc(0x00, 0x02, 0x16);
  send(0x15);
  ack();
  response();
  ack();
  send(0x10);
  EXPECT_EQ(intType(), INT_ACKNOWLEDGE);
  const auto r = response();
  ASSERT_EQ(r.size(), 8u);
  EXPECT_EQ(r[0], 0x00);
  EXPECT_EQ(r[1], 0x02);
  EXPECT_EQ(r[2], 0x16);
}

TEST_F(CdromCommandTest, GetlocpReportsTrackIndexAndAbsoluteTime) {
  // psx-spx GetlocP -> INT3(track,index,mm,ss,sect,amm,ass,asect).
  setLoc(0x01, 0x23, 0x45);
  send(0x15);
  ack();
  ack();
  send(0x11);
  const auto r = response();
  ASSERT_EQ(r.size(), 8u);
  EXPECT_EQ(r[0], 0x01); // track 1
  EXPECT_EQ(r[1], 0x01); // index 1
  EXPECT_EQ(r[5], 0x01);
  EXPECT_EQ(r[6], 0x23);
  EXPECT_EQ(r[7], 0x45);
}

TEST_F(CdromCommandTest, GettnReportsFirstAndLastTrack) {
  // psx-spx GetTN -> INT3(stat,first,last), BCD; a data disc has one track.
  send(0x13);
  EXPECT_EQ(intType(), INT_ACKNOWLEDGE);
  const auto r = response();
  ASSERT_EQ(r.size(), 3u);
  EXPECT_EQ(r[1], 0x01);
  EXPECT_EQ(r[2], 0x01);
}

TEST_F(CdromCommandTest, GettdRepliesWithStatusMinuteAndSecond) {
  // psx-spx GetTD -> INT3(stat,min,sec), both BCD.
  send(0x14, {0x00});
  EXPECT_EQ(intType(), INT_ACKNOWLEDGE);
  const auto r = response();
  ASSERT_EQ(r.size(), 3u);
  EXPECT_LE(r[1] & 0x0F, 9) << "minute is not BCD";
  EXPECT_LE(r[2] & 0x0F, 9) << "second is not BCD";
}

TEST_F(CdromCommandTest, SetmodeIsReportedByGetparam) {
  send(0x0E, {0xA0});
  EXPECT_EQ(intType(), INT_ACKNOWLEDGE);
  ack();
  EXPECT_EQ(cd.getMode(), 0xA0);
  response();
  send(0x0F);
  const auto r = response();
  ASSERT_GE(r.size(), 2u);
  EXPECT_EQ(r[1], 0xA0); // psx-spx Getparam: INT3(stat,mode,...)
}

TEST_F(CdromCommandTest, SetfilterAcknowledges) {
  send(0x0D, {0x01, 0x02});
  EXPECT_EQ(intType(), INT_ACKNOWLEDGE);
  EXPECT_EQ(response().size(), 1u);
}

TEST_F(CdromCommandTest, MuteAndDemuteAcknowledge) {
  send(0x0B);
  EXPECT_EQ(intType(), INT_ACKNOWLEDGE);
  ack();
  send(0x0C);
  EXPECT_EQ(intType(), INT_ACKNOWLEDGE);
  EXPECT_EQ(response().size(), 1u);
}

TEST_F(CdromCommandTest, PlaySetsThePlayStatusBit) {
  send(0x03);
  EXPECT_EQ(intType(), INT_ACKNOWLEDGE);
  EXPECT_EQ(cd.getState(), CdromState::Playing);
  EXPECT_NE(response().at(0) & kStatPlay, 0);
}

TEST_F(CdromCommandTest, ReadnDeliversInt1PerSectorWithTheReadBit) {
  FilledFs fs;
  cd.attachVirtualFs(&fs);
  setLoc(0x00, 0x02, 0x05); // LBA 5
  irqs.clear();
  send(0x06);
  EXPECT_EQ(intType(), INT_ACKNOWLEDGE);
  response();
  ack();
  for (int i = 0; i < 40 && !cd.hasSectorReady(); ++i)
    cd.tick(100000);
  ASSERT_TRUE(cd.hasSectorReady());
  EXPECT_EQ(fs.lastLba, 5u) << "Setloc 00:02:05 is LBA 5";
  EXPECT_EQ(intType(), INT_DATA_READY);
  const auto r = response();
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0] & (kStatRead | kStatMotor), kStatRead | kStatMotor);
}

TEST_F(CdromCommandTest, ReadsWaitsForTheAckBeforeTheNextSector) {
  FilledFs fs;
  cd.attachVirtualFs(&fs);
  send(0x1B); // ReadS
  ack();
  for (int i = 0; i < 40 && !cd.hasSectorReady(); ++i)
    cd.tick(100000);
  const uint32_t first = fs.lastLba;
  cd.tick(10000000);
  EXPECT_EQ(fs.lastLba, first) << "read on while INT1 was unacknowledged";
  ack();
  cd.tick(cd.getCyclesPerSector());
  EXPECT_EQ(fs.lastLba, first + 1);
}

TEST_F(CdromCommandTest, DoubleSpeedModeHalvesTheSectorTime) {
  // Setmode bit7: 0=normal speed (75 sectors/s), 1=double speed (150/s).
  send(0x0E, {0x80});
  ack();
  send(0x06);
  EXPECT_EQ(cd.getCyclesPerSector(), 33868800u / 150);
}

TEST_F(CdromCommandTest, SetmodeBit5SelectsWholeSectorReads) {
  // Setmode bit5: 0=800h data bytes, 1=924h bytes (whole sector minus sync).
  FilledFs fs;
  cd.attachVirtualFs(&fs);
  send(0x0E, {0x20});
  ack();
  send(0x06);
  ack();
  for (int i = 0; i < 40 && !cd.hasSectorReady(); ++i)
    cd.tick(100000);
  EXPECT_EQ(cd.getSectorSize(), 0x924u);
}

TEST_F(CdromCommandTest, PauseStopsReadingAndCompletes) {
  // psx-spx Pause -> INT3(stat), INT2(stat); the read bit drops.
  send(0x06);
  ack();
  send(0x09);
  EXPECT_EQ(intType(), INT_ACKNOWLEDGE);
  response();
  ack();
  EXPECT_EQ(intType(), INT_COMPLETE);
  EXPECT_EQ(cd.getState(), CdromState::Paused);
  EXPECT_EQ(response().at(0) & kStatRead, 0);
}

TEST_F(CdromCommandTest, StopSpinsDownAndCompletes) {
  // psx-spx Stop -> INT3(stat), INT2(stat); the motor bit drops.
  send(0x01); // motor on
  ack();
  send(0x08);
  EXPECT_EQ(intType(), INT_ACKNOWLEDGE);
  response();
  ack();
  EXPECT_EQ(intType(), INT_COMPLETE);
  EXPECT_EQ(response().at(0) & kStatMotor, 0);
  EXPECT_EQ(cd.getState(), CdromState::Idle);
}

TEST_F(CdromCommandTest, InitRaisesInt3ThenInt2AndResetsTheMode) {
  send(0x0E, {0x80});
  ack();
  irqs.clear();
  send(0x0A);
  EXPECT_EQ(irqs, (std::vector<uint8_t>{INT_ACKNOWLEDGE, INT_COMPLETE}));
  EXPECT_EQ(cd.getMode(), 0x00);
  EXPECT_NE(response().at(0) & kStatMotor, 0);
}

TEST_F(CdromCommandTest, TestCommand20ReturnsTheControllerVersion) {
  // psx-spx Test(20h) -> INT3(yy,mm,dd,ver); 98h,06h,10h,C3h is PU-18 vC3.
  send(0x19, {0x20});
  EXPECT_EQ(intType(), INT_ACKNOWLEDGE);
  EXPECT_EQ(response(), (std::vector<uint8_t>{0x98, 0x06, 0x10, 0xC3}));
}

TEST_F(CdromCommandTest, UnusedCommandFailsWithInvalidCommandError) {
  // psx-spx command table: unused commands -> INT5(stat, 40h).
  send(0x17);
  EXPECT_EQ(intType(), INT_ERROR);
  const auto r = response();
  ASSERT_EQ(r.size(), 2u);
  EXPECT_EQ(r[1], 0x40);
}

TEST_F(CdromCommandTest, GetidAcknowledgesThenReportsALicensedMode2Disc) {
  // psx-spx GetID, Licensed:Mode2 -> INT3(stat), 2nd (02h,00h,20h,00h,"SCEx").
  send(0x1A);
  EXPECT_EQ(intType(), INT_ACKNOWLEDGE);
  EXPECT_EQ(response().size(), 1u);
  ack();
  const auto r = response();
  ASSERT_EQ(r.size(), 8u);
  EXPECT_EQ(r[0], 0x02);
  EXPECT_EQ(r[1], 0x00);
  EXPECT_EQ(r[2], 0x20);
  EXPECT_EQ(r[4], 'S');
  EXPECT_EQ(r[5], 'C');
  EXPECT_EQ(r[6], 'E');
}

TEST_F(CdromCommandTest, AckWithoutPendingSecondResponseRaisesNothing) {
  send(0x01);
  irqs.clear();
  ack();
  EXPECT_TRUE(irqs.empty());
  EXPECT_FALSE(cd.hasSecondaryResponse());
}

TEST_F(CdromCommandTest, CancelPendingInterruptDropsTheSecondResponse) {
  send(0x15);
  cd.cancelPendingInterrupt();
  irqs.clear();
  cd.tick(1000000);
  EXPECT_TRUE(irqs.empty());
  EXPECT_FALSE(cd.hasInterrupt());
}

} // namespace
