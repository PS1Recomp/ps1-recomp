#include "runtime/input/input.h"
#include "runtime/metrics.h"
#include <fmt/format.h>

namespace ps1::input {

// Memory Card

MemoryCard::MemoryCard() { reset(); }

uint8_t MemoryCard::frameChecksum(const uint8_t *frame, std::size_t n) {
  uint8_t x = 0;
  for (std::size_t i = 0; i < n; i++)
    x ^= frame[i];
  return x;
}

// Lay down block 0 exactly as a Sony card leaves it after formatting.
// Without this the card reads back as blank and every game reports it
// unformatted or absent.  Layout from psx-spx,
// controllersandmemorycards.md "Memory Card Data Format".
void MemoryCard::formatImage() {
  data_.fill(0);

  auto frame = [&](uint32_t index) { return data_.data() + index * SECTOR_SIZE; };

  // Frame 0: header.  "MC", zeroes, checksum of everything above it.
  uint8_t *hdr = frame(0);
  hdr[0] = 'M';
  hdr[1] = 'C';
  hdr[SECTOR_SIZE - 1] = frameChecksum(hdr, SECTOR_SIZE - 1);

  // Frames 1..15: directory, every block free and never used.
  for (uint32_t i = 1; i <= 15; i++) {
    uint8_t *d = frame(i);
    std::memset(d, 0, SECTOR_SIZE);
    d[0] = 0xA0; // free, freshly formatted
    d[8] = 0xFF; // next block: none
    d[9] = 0xFF;
    d[SECTOR_SIZE - 1] = frameChecksum(d, SECTOR_SIZE - 1);
  }

  // Frames 16..35: broken sector list, all "none".
  for (uint32_t i = 16; i <= 35; i++) {
    uint8_t *d = frame(i);
    std::memset(d, 0, SECTOR_SIZE);
    std::memset(d, 0xFF, 4);
    d[SECTOR_SIZE - 1] = frameChecksum(d, SECTOR_SIZE - 1);
  }

  // Frames 36..62: replacement data and unused, FFh-filled.
  for (uint32_t i = 36; i <= 62; i++)
    std::memset(frame(i), 0xFF, SECTOR_SIZE);

  // Frame 63: write-test frame, same shape as the header.
  uint8_t *wt = frame(63);
  std::memset(wt, 0, SECTOR_SIZE);
  wt[0] = 'M';
  wt[1] = 'C';
  wt[SECTOR_SIZE - 1] = frameChecksum(wt, SECTOR_SIZE - 1);
}

void MemoryCard::reset() {
  formatImage();
  cmd_ = Cmd::None;
  step_ = 0;
  lastIn_ = 0;
  sectorAddr_ = 0;
  checksum_ = 0;
  flag_ = 0x08;
  dirty_ = false;
}

bool MemoryCard::loadFromFile(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return false;
  file.read(reinterpret_cast<char *>(data_.data()), CARD_SIZE);
  return file.good() || file.eof();
}

bool MemoryCard::saveToFile(const std::string &path) const {
  std::ofstream file(path, std::ios::binary);
  if (!file)
    return false;
  file.write(reinterpret_cast<const char *>(data_.data()), CARD_SIZE);
  return file.good();
}

bool MemoryCard::attachFile(const std::string &path) {
  backingPath_ = path;
  if (loadFromFile(path))
    return true;
  // No file yet: keep the formatted image and write it out, so the card the
  // game sees on this run is the one it will see on the next.
  return saveToFile(path);
}

// Written through after every accepted sector so a crash, or the fast exit
// that skips destructors, cannot lose a save.  A sector write is 128 bytes of
// game data against a 128 KB file; a whole save file is 64 of them, which is
// well inside what this costs.
void MemoryCard::flush() {
  if (!dirty_ || backingPath_.empty())
    return;
  if (saveToFile(backingPath_))
    dirty_ = false;
}

// One byte of an SIO exchange.
//
// Each command is a fixed script of byte pairs; `step_` counts how far in we
// are.  "(pre)" in the spec means the card echoes the byte it received last,
// which is what `lastIn_` carries.  Sequences from psx-spx,
// controllersandmemorycards.md "Memory Card Read/Write Commands".
uint8_t MemoryCard::transfer(uint8_t dataIn) {
  const uint8_t pre = lastIn_;
  lastIn_ = dataIn;

  if (!present_) {
    cmd_ = Cmd::None;
    return 0xFF;
  }

  if (cmd_ == Cmd::None) {
    if (dataIn == 0x81) { // memory card address
      cmd_ = Cmd::Await;
      step_ = 0;
    }
    return 0xFF;
  }

  if (cmd_ == Cmd::Await) {
    step_ = 0;
    switch (dataIn) {
    case 0x52:
      cmd_ = Cmd::Read;
      break;
    case 0x57:
      cmd_ = Cmd::Write;
      break;
    case 0x53:
      cmd_ = Cmd::GetId;
      break;
    default:
      cmd_ = Cmd::None;
      if (ps1::metrics::enabled())
        ps1::metrics::count("memcard.cmd.unknown");
      return 0xFF;
    }
    // A save screen that shows nothing has two very different causes: the game
    // never addressed the card, or it did and disliked the answer.  Counting
    // the commands separates them without a guess.
    if (ps1::metrics::enabled())
      ps1::metrics::count(dataIn == 0x52   ? "memcard.cmd.read"
                          : dataIn == 0x57 ? "memcard.cmd.write"
                                           : "memcard.cmd.getid");
    return flag_; // FLAG byte comes back with the command
  }

  const uint32_t step = step_++;
  const bool badSector = sectorAddr_ >= NUM_SECTORS;

  if (cmd_ == Cmd::GetId) {
    static const uint8_t reply[8] = {0x5A, 0x5D, 0x5C, 0x5D,
                                     0x04, 0x00, 0x00, 0x80};
    if (step >= 8) {
      cmd_ = Cmd::None;
      return 0xFF;
    }
    if (step == 7)
      cmd_ = Cmd::None;
    return reply[step];
  }

  if (cmd_ == Cmd::Read) {
    switch (step) {
    case 0:
      return 0x5A; // ID1
    case 1:
      return 0x5D; // ID2
    case 2:
      sectorAddr_ = static_cast<uint16_t>(dataIn) << 8;
      return 0x00;
    case 3:
      sectorAddr_ = static_cast<uint16_t>(sectorAddr_ | dataIn);
      checksum_ = static_cast<uint8_t>(sectorAddr_ >> 8) ^
                  static_cast<uint8_t>(sectorAddr_ & 0xFF);
      return pre;
    case 4:
      return 0x5C; // command acknowledge 1
    case 5:
      return 0x5D; // command acknowledge 2
    case 6:
      // An out-of-range sector answers FFFFh and sends nothing further.
      return badSector ? 0xFF : static_cast<uint8_t>(sectorAddr_ >> 8);
    case 7:
      if (badSector) {
        cmd_ = Cmd::None;
        return 0xFF;
      }
      return static_cast<uint8_t>(sectorAddr_ & 0xFF);
    default:
      break;
    }
    const uint32_t i = step - 8;
    if (i < SECTOR_SIZE) {
      const uint8_t v = data_[sectorAddr_ * SECTOR_SIZE + i];
      checksum_ ^= v;
      return v;
    }
    if (i == SECTOR_SIZE)
      return checksum_;
    cmd_ = Cmd::None;
    return 0x47; // "G": read good
  }

  // Cmd::Write
  switch (step) {
  case 0:
    return 0x5A;
  case 1:
    return 0x5D;
  case 2:
    sectorAddr_ = static_cast<uint16_t>(dataIn) << 8;
    return 0x00;
  case 3:
    sectorAddr_ = static_cast<uint16_t>(sectorAddr_ | dataIn);
    checksum_ = static_cast<uint8_t>(sectorAddr_ >> 8) ^
                static_cast<uint8_t>(sectorAddr_ & 0xFF);
    return pre;
  default:
    break;
  }
  const uint32_t i = step - 4;
  if (i < SECTOR_SIZE) {
    writeBuf_[i] = dataIn;
    checksum_ ^= dataIn;
    return pre;
  }
  if (i == SECTOR_SIZE) {
    // The host's checksum byte.  Keep it; the verdict goes out at the end.
    checksum_ ^= dataIn; // zero iff the host agrees with us
    return pre;
  }
  if (i == SECTOR_SIZE + 1)
    return 0x5C;
  if (i == SECTOR_SIZE + 2)
    return 0x5D;

  cmd_ = Cmd::None;
  if (badSector)
    return 0xFF; // bad sector
  if (checksum_ != 0)
    return 0x4E; // "N": bad checksum, nothing written
  writeSector(sectorAddr_, writeBuf_);
  return 0x47; // "G": write good
}

void MemoryCard::readSector(uint32_t sector, uint8_t *dst) const {
  std::memcpy(dst, data_.data() + sector * SECTOR_SIZE, SECTOR_SIZE);
}

void MemoryCard::writeSector(uint32_t sector, const uint8_t *src) {
  std::memcpy(data_.data() + sector * SECTOR_SIZE, src, SECTOR_SIZE);
  dirty_ = true;
  flag_ = static_cast<uint8_t>(flag_ & ~0x08); // directory has now been touched
  flush();
}

// Input Controller

InputController::InputController() { reset(); }

void InputController::reset() {
  for (auto &p : ports_) {
    p = PortState{};
  }
  for (int p = 0; p < 2; ++p) {
    tapped_[p] = 0;
    shown_[p] = 0;
  }
  joyData_ = 0;
  joyStat_ = 0x05; // TX ready, TX not empty
  joyMode_ = 0;
  joyCtrl_ = 0;
  joyBaud_ = 0x88;
  sioState_ = SioState::Idle;
  selectedPort_ = 0;
  irqPending_ = false;
}

void InputController::press(uint16_t btn, int port) {
  if (port >= 0 && port < 2) {
    ports_[port].buttons &= ~btn;
    tapped_[port].fetch_or(btn, std::memory_order_relaxed);
  }
}

void InputController::release(uint16_t btn, int port) {
  if (port >= 0 && port < 2)
    ports_[port].buttons |= btn;
}

void InputController::latchFrame() {
  for (int p = 0; p < 2; ++p)
    shown_[p] = tapped_[p].exchange(0, std::memory_order_relaxed);
}

uint16_t InputController::buttonState(int port) const {
  if (port >= 0 && port < 2)
    return ports_[port].buttons & ~shown_[port];
  return 0xFFFF;
}

void InputController::setAnalog(int port, uint8_t lx, uint8_t ly, uint8_t rx,
                                uint8_t ry) {
  if (port >= 0 && port < 2) {
    ports_[port].analogLX = lx;
    ports_[port].analogLY = ly;
    ports_[port].analogRX = rx;
    ports_[port].analogRY = ry;
  }
}

void InputController::setPadType(int port, PadType type) {
  if (port >= 0 && port < 2)
    ports_[port].padType = type;
}

PadType InputController::getPadType(int port) const {
  if (port >= 0 && port < 2)
    return ports_[port].padType;
  return PadType::None;
}

// SIO Register Access

void InputController::writeRegister(uint32_t addr, uint32_t val) {
  uint32_t offset = addr - 0x1F801040;
  switch (offset) {
  case 0x00: // JOY_DATA
    joyData_ = val;
    {
      uint8_t response = processSioTransfer(val & 0xFF);
      joyData_ = response;
      irqPending_ = true;
    }
    break;
  }
}

uint32_t InputController::readRegister(uint32_t addr) const {
  uint32_t offset = addr - 0x1F801040;
  switch (offset) {
  case 0x00:
    return joyData_;
  case 0x04:
    return joyStat_;
  }
  return 0;
}

void InputController::writeRegister16(uint32_t addr, uint16_t val) {
  uint32_t offset = addr - 0x1F801040;
  switch (offset) {
  case 0x08:
    joyMode_ = val;
    break;
  case 0x0A:
    joyCtrl_ = val;
    if (val & (1 << 4)) { // Acknowledge
      irqPending_ = false;
    }
    if (val & (1 << 6)) { // Reset
      sioState_ = SioState::Idle;
      joyStat_ = 0x05;
    }
    // Select port (bit 13)
    selectedPort_ = (val >> 13) & 1;
    break;
  case 0x0E:
    joyBaud_ = val;
    break;
  }
}

uint16_t InputController::readRegister16(uint32_t addr) const {
  uint32_t offset = addr - 0x1F801040;
  switch (offset) {
  case 0x08:
    return joyMode_;
  case 0x0A:
    return joyCtrl_;
  case 0x0E:
    return joyBaud_;
  case 0x04:
    return joyStat_ & 0xFFFF;
  }
  return 0;
}

// SIO Transfer State Machine

uint8_t InputController::processSioTransfer(uint8_t dataIn) {
  const auto &port = ports_[selectedPort_];

  switch (sioState_) {
  case SioState::Idle:
    if (dataIn == 0x01) { // Start communication
      sioState_ = SioState::SelectDevice;
      return 0xFF;
    }
    if (dataIn == 0x81) { // Memory card
      sioState_ = SioState::MemCardTransfer;
      return memCards_[selectedPort_].transfer(dataIn);
    }
    return 0xFF;

  case SioState::SelectDevice:
    if (dataIn == 0x42) { // Read pad
      sioState_ = SioState::TransferId;
      return static_cast<uint8_t>(port.padType);
    }
    sioState_ = SioState::Idle;
    return 0xFF;

  case SioState::TransferId:
    sioState_ = SioState::TransferPadLo;
    return 0x5A; // Always 0x5A

  case SioState::TransferPadLo:
    sioState_ = SioState::TransferPadHi;
    return buttonState(selectedPort_) & 0xFF;

  case SioState::TransferPadHi:
    if (port.padType == PadType::Analog || port.padType == PadType::DualShock) {
      sioState_ = SioState::TransferAnalogRX;
    } else {
      sioState_ = SioState::Idle;
    }
    return (buttonState(selectedPort_) >> 8) & 0xFF;

  case SioState::TransferAnalogRX:
    sioState_ = SioState::TransferAnalogRY;
    return port.analogRX;

  case SioState::TransferAnalogRY:
    sioState_ = SioState::TransferAnalogLX;
    return port.analogRY;

  case SioState::TransferAnalogLX:
    sioState_ = SioState::TransferAnalogLY;
    return port.analogLX;

  case SioState::TransferAnalogLY:
    sioState_ = SioState::Idle;
    return port.analogLY;

  case SioState::MemCardTransfer: {
    const uint8_t out = memCards_[selectedPort_].transfer(dataIn);
    // The card drops back to idle when a command's script runs out, so the
    // port has to follow it -- otherwise the next 0x01 from the pad poll is
    // fed to the card instead of starting a controller exchange.
    if (!memCards_[selectedPort_].transferActive())
      sioState_ = SioState::Idle;
    return out;
  }
  }

  return 0xFF;
}

} // namespace ps1::input
