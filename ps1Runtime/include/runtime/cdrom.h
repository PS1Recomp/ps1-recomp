#pragma once

// ps1Runtime — CD-ROM Stub
// PS1 CD-ROM controller (command/parameter/response FIFOs)
// This is a stub for future incremental implementation.

#include <cstdint>

namespace ps1 {

class CDROM {
public:
  CDROM() { reset(); }

  void reset() {
    status_ = 0x18; // Shell open, motor off
    interruptFlag_ = 0;
  }

  // Status register (1F801800h.Index0)
  uint8_t readStatus() const { return status_; }

  // Command — stub (will handle SetLoc, ReadN, etc.)
  void writeCommand(uint8_t cmd) { (void)cmd; }
  void writeParam(uint8_t param) { (void)param; }

  // Response FIFO — stub
  uint8_t readResponse() { return 0; }

  // Data read — stub
  uint8_t readData() { return 0; }

  // Interrupt
  uint8_t interruptFlag() const { return interruptFlag_; }
  void ackInterrupt(uint8_t val) { interruptFlag_ &= ~val; }

private:
  uint8_t status_;
  uint8_t interruptFlag_;
};

} // namespace ps1
