#pragma once

// ps1Runtime -- Input Controller
// PS1 Controller (Digital Pad, DualShock) and Memory Card via SIO

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace ps1::input {

// PS1 Button Bits (active-low: 0 = pressed)
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

// Controller Type
enum class PadType : uint8_t {
  None = 0x00,
  Digital = 0x41,   // Standard digital pad
  Analog = 0x73,    // DualShock in analog mode
  DualShock = 0x79, // DualShock with pressure
};

// Memory Card
class MemoryCard {
public:
  static constexpr uint32_t CARD_SIZE = 128 * 1024; // 128KB
  static constexpr uint32_t BLOCK_SIZE = 8 * 1024;  // 8KB per block
  static constexpr uint32_t NUM_BLOCKS = 15;        // 15 usable blocks
  static constexpr uint32_t SECTOR_SIZE = 128;      // 128 bytes per sector

  static constexpr uint32_t NUM_SECTORS = CARD_SIZE / SECTOR_SIZE; // 1024

  MemoryCard();

  /// Wipe and lay down a valid directory, as a freshly formatted card.
  void reset();

  /// Back the card with `path`.  An existing file is loaded; a missing one is
  /// created from the formatted card.  Later writes are flushed there.
  bool attachFile(const std::string &path);

  bool loadFromFile(const std::string &path);
  bool saveToFile(const std::string &path) const;

  /// One byte of an SIO exchange: returns what the card drives back.
  uint8_t transfer(uint8_t dataIn);

  /// True while the card still owes the host more bytes of a command.
  bool transferActive() const { return cmd_ != Cmd::None; }

  bool isPresent() const { return present_; }
  void setPresent(bool p) { present_ = p; }

  /// Test seam: the raw 128 KB image.
  const std::array<uint8_t, CARD_SIZE> &image() const { return data_; }
  std::array<uint8_t, CARD_SIZE> &image() { return data_; }

  uint8_t flagByte() const { return flag_; }

  /// FLAG bit3: the directory has not been touched since insertion.
  bool changed() const { return (flag_ & 0x08) != 0; }

  /// Whole-sector access for the BIOS card functions, which talk to the card
  /// a sector at a time.  `sector` must be < NUM_SECTORS.
  void readSector(uint32_t sector, uint8_t *dst) const;
  void writeSector(uint32_t sector, const uint8_t *src);

private:
  // The card answers a fixed script per command, so the state is just "which
  // command" plus "how many bytes into it".  See psx-spx,
  // controllersandmemorycards.md "Memory Card Read/Write Commands".
  enum class Cmd : uint8_t { None, Await, Read, Write, GetId };

  void formatImage();
  static uint8_t frameChecksum(const uint8_t *frame, std::size_t n);
  void flush();

  std::array<uint8_t, CARD_SIZE> data_{};
  bool present_ = true;

  Cmd cmd_ = Cmd::None;
  uint32_t step_ = 0;     // bytes exchanged since the command byte
  uint8_t lastIn_ = 0;    // the "(pre)" reply: echo of the previous byte
  uint16_t sectorAddr_ = 0;
  uint8_t checksum_ = 0;  // running MSB^LSB^data
  uint8_t writeBuf_[SECTOR_SIZE]{};
  // Bit3 means "directory not read since insertion".  Games clear it with a
  // dummy write, and use it to sense a swapped card.
  uint8_t flag_ = 0x08;

  std::string backingPath_;
  bool dirty_ = false;
};

// Input Controller
class InputController {
public:
  InputController();
  void reset();

  // Button state management
  void press(uint16_t btn, int port = 0);
  void release(uint16_t btn, int port = 0);
  uint16_t buttonState(int port = 0) const;

  /// Call once per VBlank, before the pad is sampled: buttons pressed since
  /// the previous call read as pressed until the next one, so a tap shorter
  /// than a frame is not lost.
  void latchFrame();

  // Analog sticks (0x00-0xFF, 0x80 = center)
  void setAnalog(int port, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry);

  // Controller type
  void setPadType(int port, PadType type);
  PadType getPadType(int port) const;

  // SIO register access (0x1F801040-0x1F80104F)
  void writeRegister(uint32_t addr, uint32_t val);
  uint32_t readRegister(uint32_t addr) const;
  void writeRegister16(uint32_t addr, uint16_t val);
  uint16_t readRegister16(uint32_t addr) const;

  // Memory card access
  MemoryCard &getMemoryCard(int slot) { return memCards_[slot]; }

  // Check for pending IRQ (controller ACK)
  bool hasInterrupt() const { return irqPending_; }
  void clearInterrupt() { irqPending_ = false; }

private:
  // Port state
  struct PortState {
    uint16_t buttons = 0xFFFF; // Active low
    uint8_t analogLX = 0x80;
    uint8_t analogLY = 0x80;
    uint8_t analogRX = 0x80;
    uint8_t analogRY = 0x80;
    PadType padType = PadType::Digital;
  };

  PortState ports_[2];
  // press() runs on the host event thread, latchFrame() on the game thread.
  std::atomic<uint16_t> tapped_[2] = {0, 0}; // pressed since the last latch
  uint16_t shown_[2] = {0, 0};               // taps visible this frame

  // SIO state
  uint32_t joyData_ = 0;
  uint32_t joyStat_ = 0;
  uint16_t joyMode_ = 0;
  uint16_t joyCtrl_ = 0;
  uint16_t joyBaud_ = 0;

  // SIO transfer state machine
  enum class SioState : uint8_t {
    Idle,
    SelectDevice,
    TransferId,
    TransferPadLo,
    TransferPadHi,
    TransferAnalogRX,
    TransferAnalogRY,
    TransferAnalogLX,
    TransferAnalogLY,
    MemCardTransfer
  };
  SioState sioState_ = SioState::Idle;
  uint8_t selectedPort_ = 0;
  bool irqPending_ = false;

  // Memory cards (2 slots)
  MemoryCard memCards_[2];

  uint8_t processSioTransfer(uint8_t dataIn);
};

} // namespace ps1::input
