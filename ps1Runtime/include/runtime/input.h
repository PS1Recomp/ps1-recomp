#pragma once

// ps1Runtime — Input Stub
// PS1 Controller/Memory Card (SIO port)
// This is a stub for future incremental implementation.

#include <cstdint>

namespace ps1 {

class Input {
public:
  // PS1 Digital Pad button bits (active low)
  enum Button : uint16_t {
    BTN_SELECT = (1u << 0),
    BTN_L3 = (1u << 1),
    BTN_R3 = (1u << 2),
    BTN_START = (1u << 3),
    BTN_UP = (1u << 4),
    BTN_RIGHT = (1u << 5),
    BTN_DOWN = (1u << 6),
    BTN_LEFT = (1u << 7),
    BTN_L2 = (1u << 8),
    BTN_R2 = (1u << 9),
    BTN_L1 = (1u << 10),
    BTN_R1 = (1u << 11),
    BTN_TRIANGLE = (1u << 12),
    BTN_CIRCLE = (1u << 13),
    BTN_CROSS = (1u << 14),
    BTN_SQUARE = (1u << 15),
  };

  Input() { reset(); }

  void reset() {
    buttonState_ = 0xFFFF; // All released (active low)
    joyData_ = 0;
    joyStat_ = 0;
  }

  // Button state (active low: 0 = pressed)
  void press(uint16_t btn) { buttonState_ &= ~btn; }
  void release(uint16_t btn) { buttonState_ |= btn; }
  uint16_t buttonState() const { return buttonState_; }

  // SIO registers — stub
  void writeJoyData(uint32_t val) { joyData_ = val; }
  uint32_t readJoyData() const { return joyData_; }
  uint32_t readJoyStat() const { return joyStat_; }

private:
  uint16_t buttonState_;
  uint32_t joyData_;
  uint32_t joyStat_;
};

} // namespace ps1
