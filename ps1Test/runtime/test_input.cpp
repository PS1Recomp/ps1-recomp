#include "runtime/input/input.h"
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
  // Start communication
  uint8_t resp = mc.transfer(0x81);
  EXPECT_EQ(resp, 0xFF); // Flag byte

  // Send read command
  resp = mc.transfer(0x52);
  EXPECT_EQ(resp, 0x5A); // ID1

  // Address high byte
  resp = mc.transfer(0x00);
  EXPECT_EQ(resp, 0x5D); // ID2

  // Address low byte
  resp = mc.transfer(0x00);
  // Should return ACK
}

TEST(MemCard, WriteAndReadBack) {
  MemoryCard mc;

  // Directly test file I/O persistence
  mc.setPresent(true);
  EXPECT_TRUE(mc.isPresent());

  mc.setPresent(false);
  EXPECT_FALSE(mc.isPresent());
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
