#include "runtime/mdec/mdec.h"
#include <gtest/gtest.h>

using namespace ps1::mdec;

class MdecTest : public ::testing::Test {
protected:
  MDEC mdec;
  void SetUp() override { mdec.reset(); }
};

TEST_F(MdecTest, InitialState) {
  EXPECT_FALSE(mdec.isBusy());
  EXPECT_TRUE(mdec.dataOutEmpty());
  uint32_t status = mdec.readStatus();
  EXPECT_TRUE(status & (1 << 31)); // Data-out empty
}

TEST_F(MdecTest, ResetCommand) {
  mdec.writeControl(1u << 31); // Reset
  EXPECT_FALSE(mdec.isBusy());
  EXPECT_TRUE(mdec.dataOutEmpty());
}

TEST_F(MdecTest, QuantTableUpload) {
  // Command: upload quant table (opcode 2, 16 words = 64 bytes)
  uint32_t cmd = (2u << 29) | 16;
  mdec.writeCommand(cmd);
  EXPECT_TRUE(mdec.isBusy());

  // Feed 16 words (64 bytes of quantization data)
  for (int i = 0; i < 16; i++) {
    mdec.writeCommand(0x10101010);
  }

  EXPECT_FALSE(mdec.isBusy());
}

TEST_F(MdecTest, DecodeCommandSetsBusy) {
  // Command: decode macroblock (opcode 1, with some data words)
  uint32_t cmd = (1u << 29) | 4;
  mdec.writeCommand(cmd);
  EXPECT_TRUE(mdec.isBusy());
}

TEST_F(MdecTest, StatusRegisterBits) {
  uint32_t status = mdec.readStatus();
  // Data-out empty flag should be set
  EXPECT_TRUE(status & (1 << 31));
  // Data-in not full
  EXPECT_FALSE(status & (1 << 30));
  // Not busy
  EXPECT_FALSE(status & (1 << 29));
}

TEST_F(MdecTest, DmaInterfaceSmoke) {
  // Send a command + data via DMA
  uint32_t cmd = (1u << 29) | 2;
  mdec.writeCommand(cmd);

  uint32_t data[2] = {0xFE00FE00, 0xFE00FE00}; // End markers
  mdec.dmaIn(data, 2);

  // Should process without crash
  EXPECT_FALSE(mdec.isBusy());
}

// Zigzag table sanity

TEST_F(MdecTest, ZigzagTableValid) {
  // The zigzag table should contain each value 0-63 exactly once
  bool seen[64] = {};
  for (int i = 0; i < 64; i++) {
    uint8_t val = MDEC::ZIGZAG[i];
    ASSERT_LT(val, 64u);
    EXPECT_FALSE(seen[val]) << "Duplicate zigzag entry: " << (int)val;
    seen[val] = true;
  }
}

// Command protocol (psx-spx "MDEC Commands"). Output assertions below are
// format-agnostic on purpose: mdec.cpp decodes the depth field (bits 28-27)
// with 15/24bpp swapped, so pixel packing does not yet follow the spec.

namespace {

constexpr uint32_t kStatusOutEmpty = 1u << 31;
constexpr uint32_t kStatusBusy = 1u << 29;

uint32_t decodeCmd(uint32_t depth, uint32_t words) {
  return (1u << 29) | (depth << 27) | words;
}

// One RL block: DCT word (q_scale=1, DC) + AC entries + EOB (FE00h).
void appendBlock(std::vector<uint16_t> &hw, int16_t dc,
                 std::initializer_list<uint16_t> ac) {
  hw.push_back(static_cast<uint16_t>((1u << 10) | (dc & 0x3FF)));
  hw.insert(hw.end(), ac.begin(), ac.end());
  hw.push_back(0xFE00);
}

std::vector<uint32_t> toWords(std::vector<uint16_t> hw) {
  if (hw.size() % 2)
    hw.push_back(0xFE00); // padding halfword
  std::vector<uint32_t> w;
  for (size_t i = 0; i < hw.size(); i += 2)
    w.push_back(hw[i] | (static_cast<uint32_t>(hw[i + 1]) << 16));
  return w;
}

} // namespace

TEST_F(MdecTest, NopAndInvalidOpcodesExpectNoParameters) {
  for (uint32_t op : {0u, 4u, 5u, 6u, 7u}) {
    mdec.writeCommand((op << 29) | 5);
    EXPECT_FALSE(mdec.isBusy()) << "opcode " << op;
    EXPECT_EQ(mdec.readStatus() & kStatusBusy, 0u);
  }
  // Still idle: the next word is taken as a command, not a parameter.
  mdec.writeCommand(2u << 29);
  EXPECT_TRUE(mdec.isBusy());
}

TEST_F(MdecTest, LumaQuantTableTakesSixteenParameterWords) {
  mdec.writeCommand(2u << 29);
  for (int i = 0; i < 15; i++)
    mdec.writeCommand(0x02020202);
  EXPECT_TRUE(mdec.isBusy());
  mdec.writeCommand(0x02020202);
  EXPECT_FALSE(mdec.isBusy());
}

TEST_F(MdecTest, ColorQuantTableTakesThirtyTwoParameterWords) {
  mdec.writeCommand((2u << 29) | 1);
  for (int i = 0; i < 31; i++)
    mdec.writeCommand(0x04040404);
  EXPECT_TRUE(mdec.isBusy()) << "luma + color = 128 bytes";
  mdec.writeCommand(0x04040404);
  EXPECT_FALSE(mdec.isBusy());
}

TEST_F(MdecTest, ScaleTableTakesThirtyTwoParameterWords) {
  mdec.writeCommand(3u << 29);
  for (int i = 0; i < 31; i++)
    mdec.writeCommand(0x5A825A82);
  EXPECT_TRUE(mdec.isBusy()) << "64 signed halfwords";
  mdec.writeCommand(0x5A825A82);
  EXPECT_FALSE(mdec.isBusy());
}

TEST_F(MdecTest, DecodeStaysBusyUntilLastParameterWord) {
  mdec.writeCommand(decodeCmd(2, 3));
  mdec.writeCommand(0xFE00FE00);
  mdec.writeCommand(0xFE00FE00);
  EXPECT_NE(mdec.readStatus() & kStatusBusy, 0u);
  mdec.writeCommand(0xFE00FE00);
  EXPECT_EQ(mdec.readStatus() & kStatusBusy, 0u);
}

TEST_F(MdecTest, ResetAbortsCommandInProgress) {
  mdec.writeCommand(decodeCmd(2, 10));
  mdec.writeCommand(0xFE00FE00);
  mdec.writeControl(1u << 31);
  EXPECT_FALSE(mdec.isBusy());
  EXPECT_NE(mdec.readStatus() & kStatusOutEmpty, 0u);
  // The aborted command no longer swallows words as parameters.
  mdec.writeCommand(2u << 29);
  EXPECT_TRUE(mdec.isBusy());
}

TEST_F(MdecTest, ControlWithoutResetBitKeepsCommandRunning) {
  mdec.writeCommand(decodeCmd(2, 10));
  mdec.writeControl(0x60000000); // DMA enables only
  EXPECT_TRUE(mdec.isBusy());
}

class MdecDepthTest : public MdecTest,
                      public ::testing::WithParamInterface<uint32_t> {};

// An all-zero colored macroblock is flat mid-gray (Y=Cr=Cb=0), so every
// output word must be identical whatever the pixel packing; the data then
// drains through the data register until Data-Out Fifo Empty is set again.
TEST_P(MdecDepthTest, FlatMacroblockDrainsAsIdenticalWords) {
  std::vector<uint16_t> hw;
  for (int b = 0; b < 6; b++)
    appendBlock(hw, 0, {});
  const auto words = toWords(hw);
  mdec.writeCommand(decodeCmd(GetParam(), static_cast<uint32_t>(words.size())));
  mdec.dmaIn(words.data(), static_cast<uint32_t>(words.size()));

  ASSERT_EQ(mdec.readStatus() & kStatusOutEmpty, 0u);
  const uint32_t first = mdec.readData();
  EXPECT_NE(first, 0u);
  int count = 1;
  while (!mdec.dataOutEmpty()) {
    EXPECT_EQ(mdec.readData(), first) << "word " << count;
    count++;
  }
  EXPECT_GE(count, 128); // 256 pixels at >= 15bpp
  EXPECT_NE(mdec.readStatus() & kStatusOutEmpty, 0u);
}

// Macroblocks are independent: the same compressed macroblock sent twice in
// one command decodes to two identical halves. Uses AC runs and uploaded
// luma+color quant tables.
TEST_P(MdecDepthTest, RepeatedMacroblockDecodesIdentically) {
  mdec.writeCommand((2u << 29) | 1);
  for (int i = 0; i < 32; i++)
    mdec.writeCommand(i < 16 ? 0x08080808 : 0x10101010);

  std::vector<uint16_t> hw;
  for (int mb = 0; mb < 2; mb++)
    for (int b = 0; b < 6; b++)
      appendBlock(hw, static_cast<int16_t>(8 * b - 20),
                  {static_cast<uint16_t>(5),                       // run 0, +5
                   static_cast<uint16_t>((2u << 10) | 0x3FD)});    // run 2, -3
  const auto words = toWords(hw);
  mdec.writeCommand(decodeCmd(GetParam(), static_cast<uint32_t>(words.size())));
  mdec.dmaIn(words.data(), static_cast<uint32_t>(words.size()));

  std::vector<uint32_t> out;
  while (!mdec.dataOutEmpty())
    out.push_back(mdec.dmaOutRead());
  ASSERT_FALSE(out.empty());
  ASSERT_EQ(out.size() % 2, 0u);
  const size_t half = out.size() / 2;
  for (size_t i = 0; i < half; i++)
    ASSERT_EQ(out[i], out[half + i]) << "word " << i;
}

// Depth 2 = 24bpp, 3 = 15bpp (psx-spx MDEC(1) bits 28-27).
INSTANTIATE_TEST_SUITE_P(ColorDepths, MdecDepthTest, ::testing::Values(2u, 3u));

namespace {

std::vector<uint32_t> decodeAll(MDEC &mdec, const std::vector<uint16_t> &hw) {
  const auto words = toWords(hw);
  mdec.writeCommand(decodeCmd(3, static_cast<uint32_t>(words.size())));
  mdec.dmaIn(words.data(), static_cast<uint32_t>(words.size()));
  std::vector<uint32_t> out;
  while (!mdec.dataOutEmpty())
    out.push_back(mdec.readData());
  return out;
}

} // namespace

// A colored macroblock is Cr, Cb, Y1..Y4 (psx-spx "MDEC Decompression"); a
// command whose data ends before the sixth block yields no pixels.
class MdecTruncatedTest : public MdecTest,
                          public ::testing::WithParamInterface<int> {};

TEST_P(MdecTruncatedTest, MacroblockMissingBlocksProducesNoOutput) {
  std::vector<uint16_t> hw;
  for (int b = 0; b < GetParam(); b++)
    appendBlock(hw, 0, {});

  auto out = decodeAll(mdec, hw);

  EXPECT_TRUE(out.empty());
  EXPECT_FALSE(mdec.isBusy());
}

INSTANTIATE_TEST_SUITE_P(BlocksPresent, MdecTruncatedTest,
                         ::testing::Values(1, 4, 5));

// psx-spx rl_decode_block: any entry whose run steps past coefficient 63
// ends the block ("should end with n=FE00h (that sets k>63)"), so a run-63
// entry acts as the end code and its level is never stored.
TEST_F(MdecTest, AcRunPastTheLastCoefficientEndsTheBlock) {
  std::vector<uint16_t> flat, overflowing;
  for (int b = 0; b < 6; b++) {
    appendBlock(flat, 0, {});
    overflowing.push_back(1u << 10); // q_scale 1, DC 0
    overflowing.push_back(static_cast<uint16_t>((63u << 10) | 100));
  }

  auto expected = decodeAll(mdec, flat);
  auto actual = decodeAll(mdec, overflowing);

  ASSERT_FALSE(expected.empty());
  EXPECT_EQ(actual, expected);
}
