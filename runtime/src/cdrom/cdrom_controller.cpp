#include "runtime/cdrom/cdrom_controller.h"
#include <cstring>
#include <fmt/format.h>

namespace ps1::cdrom {

// ─── Helpers ────────────────────────────────────────────

uint32_t CdromController::msfToLba(uint8_t m, uint8_t s, uint8_t f) {
  return (m * 60 + s) * 75 + f;
}

void CdromController::lbaToMsf(uint32_t lba, uint8_t &m, uint8_t &s,
                               uint8_t &f) {
  m = lba / (60 * 75);
  s = (lba / 75) % 60;
  f = lba % 75;
}

uint8_t CdromController::toBcd(uint8_t val) {
  return ((val / 10) << 4) | (val % 10);
}

uint8_t CdromController::fromBcd(uint8_t bcd) {
  return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

// ─── Constructor / Reset ────────────────────────────────

CdromController::CdromController() { reset(); }

void CdromController::reset() {
  state_ = CdromState::Idle;
  indexReg_ = 0;
  paramFifo_.clear();
  responseFifo_.clear();
  interruptFlag_ = 0;
  interruptEnable_ = 0x1F;
  setLocMinutes_ = 0;
  setLocSeconds_ = 2;
  setLocSector_ = 0;
  currentLba_ = 0;
  seekTarget_ = 0;
  mode_ = 0;
  sectorBuffer_.fill(0);
  sectorSize_ = 2048;
  sectorReady_ = false;
  cyclesUntilResponse_ = 0;
  cyclesPerSector_ = 33868800 / 75; // ~451584 cycles per sector at 1x
  readCycleCounter_ = 0;
  xaFilterFile_ = 0;
  xaFilterChannel_ = 0;
  xaFilterEnabled_ = false;
  motorOn_ = false;
  shellOpen_ = false;
  pendingCommand_ = 0;
  commandPending_ = false;
}

void CdromController::attachVirtualFs(VirtualFs *vfs) { vfs_ = vfs; }

// ─── Status Byte ────────────────────────────────────────

uint8_t CdromController::buildStatusByte() const {
  uint8_t stat = 0;
  if (motorOn_)
    stat |= (1 << 1);
  if (state_ == CdromState::ReadingData)
    stat |= (1 << 5);
  if (state_ == CdromState::Seeking)
    stat |= (1 << 6);
  if (state_ == CdromState::Playing)
    stat |= (1 << 7);
  if (shellOpen_)
    stat |= (1 << 4);
  return stat;
}

// ─── Register I/O ───────────────────────────────────────

void CdromController::writeRegister(uint32_t addr, uint8_t val) {
  uint32_t port = addr & 3;

  // fmt::print("[CDROM-IO] Write port{}.idx{} = 0x{:02X}\n", port, indexReg_,
  //            val);

  if (port == 0) {
    indexReg_ = val & 3;
    return;
  }

  switch (indexReg_) {
  case 0:
    switch (port) {
    case 1: // Command register
      // Execute commands immediately (HLE-style) so the game thread can
      // see the response without waiting for tick().  This avoids a race
      // where the game's tight polling loop times out before the main
      // thread's per-frame tick() ever processes the command.
      pendingCommand_ = val;
      commandPending_ = false; // no deferred processing needed
      executeCommand(val);
      break;
    case 2: // Parameter FIFO
      if (paramFifo_.size() < 16)
        paramFifo_.push_back(val);
      break;
    case 3: // Request register
      // bit5: want data (triggers data read)
      break;
    }
    break;
  case 1:
    switch (port) {
    case 3: // Interrupt flag (ack by writing bits) — Index 1
      interruptFlag_ &= ~(val & 0x1F);
      // When the game clears interrupt bits, it has finished processing
      // the interrupt (including reading the response and calling any
      // callbacks).  Clear the delivery gate so the CDROM can deliver
      // the next sector.
      if (val & 0x1F)
        waitingForAck_ = false;
      if (val & 0x40)
        paramFifo_.clear();
      break;
    }
    break;
  case 2:
    switch (port) {
    case 2: // Interrupt enable
      interruptEnable_ = val & 0x1F;
      break;
    case 3: // Interrupt flag (ack by writing bits) — Index 2
      // Some docs say index 2/3 port 3 also ACKs IF.
      // PsyQ CdInit writes port3.idx2 = 0x00 (no-op clear).
      interruptFlag_ &= ~(val & 0x1F);
      if (val & 0x1F)
        waitingForAck_ = false;
      if (val & 0x40)
        paramFifo_.clear();
      break;
    }
    break;
  case 3:
    switch (port) {
    case 1: // Volume apply
      break;
    case 3: // Interrupt flag (ack by writing bits) — Index 3
      // PS1 CDROM: port 3 with ODD index (1 or 3) = write to IF ACK.
      // PsyQ CdInit writes port3.idx3 = 0x20 to clear param FIFO.
      interruptFlag_ &= ~(val & 0x1F);
      if (val & 0x1F)
        waitingForAck_ = false;
      if (val & 0x40)
        paramFifo_.clear();
      break;
    }
    break;
  }
}

uint8_t CdromController::readRegister(uint32_t addr) {
  uint32_t port = addr & 3;

  if (port == 0) {
    // Status register
    uint8_t stat = indexReg_ & 3;
    if (!paramFifo_.empty())
      stat |= (1 << 3); // Param fifo not empty
    if (paramFifo_.size() < 16)
      stat |= (1 << 4); // Param fifo not full
    if (!responseFifo_.empty())
      stat |= (1 << 5); // Response fifo not empty
    if (sectorReady_)
      stat |= (1 << 6); // Data fifo has data
    // bit 7: command/parameter busy
    if (commandPending_)
      stat |= (1 << 7);
    // fmt::print("[CDROM-IO] Read port0 = 0x{:02X} (idx={})\n", stat,
    // indexReg_);
    return stat;
  }

  uint8_t result = 0;
  switch (port) {
  case 1: // Response FIFO
    if (!responseFifo_.empty()) {
      result = responseFifo_.front();
      responseFifo_.pop_front();
    }
    // fmt::print(
    //     "[CDROM-IO] Read port1.idx{} = 0x{:02X} (response, remaining={})\n",
    //     indexReg_, result, responseFifo_.size());
    return result;
  case 2: // Data read (sector data)
    // fmt::print("[CDROM-IO] Read port2.idx{} = 0x00 (data stub)\n",
    // indexReg_);
    return 0; // DMA should be used instead
  case 3:
    if (indexReg_ == 0 || indexReg_ == 2) {
      result = interruptEnable_ | 0xE0;
      // fmt::print("[CDROM-IO] Read port3.idx{} = 0x{:02X} (IE)\n", indexReg_,
      // result);
    } else {
      result = interruptFlag_ | 0xE0;
      static int ifReadCount = 0;
      ifReadCount++;
      if (ifReadCount <= 100) {
        fmt::print(stderr, "[CDROM-IF] Read IF={} (interruptFlag_={}) idx={}\n",
                   result, interruptFlag_, indexReg_);
      }
    }
    return result;
  }
  return 0;
}

// ─── Interrupt ──────────────────────────────────────────

void CdromController::ackInterrupt(uint8_t val) {
  interruptFlag_ &= ~(val & 0x1F);
  // NOTE: no longer clears waitingForAck_ here.
  // Use clearWaitingForAck() explicitly after the data callback has run.
}

void CdromController::clearWaitingForAck() { waitingForAck_ = false; }

// ─── Tick ───────────────────────────────────────────────

void CdromController::tick(uint32_t cycles) {
  // Process pending command after delay
  if (commandPending_ && cyclesUntilResponse_ > 0) {
    if (cycles >= cyclesUntilResponse_) {
      cyclesUntilResponse_ = 0;
      commandPending_ = false;
      executeCommand(pendingCommand_);
    } else {
      cyclesUntilResponse_ -= cycles;
    }
  } else if (hasSecondaryResponse_ && secondaryResponseDelay_ > 0) {
    if (cycles >= secondaryResponseDelay_) {
      secondaryResponseDelay_ = 0;
      hasSecondaryResponse_ = false;

      responseFifo_.clear();
      for (auto b : secondaryData_)
        responseFifo_.push_back(b);
      interruptFlag_ = static_cast<uint8_t>(secondaryInterrupt_);
      if (interruptCallback_ && secondaryInterrupt_ != INT_NONE) {
        interruptCallback_(static_cast<uint8_t>(secondaryInterrupt_));
      }
    } else {
      secondaryResponseDelay_ -= cycles;
    }
  }

  // Process ongoing reads — deliver one sector at a time, wait for ACK
  if (state_ == CdromState::ReadingData && !waitingForAck_) {
    readCycleCounter_ += static_cast<int32_t>(cycles);
    if (readCycleCounter_ >= static_cast<int32_t>(cyclesPerSector_)) {
      readCycleCounter_ -= static_cast<int32_t>(cyclesPerSector_);

      // Read next sector
      if (vfs_) {
        auto sector = vfs_->readSector(currentLba_);
        if (sector) {
          std::memcpy(sectorBuffer_.data(), sector->raw,
                      std::min<size_t>(SECTOR_SIZE_RAW, 2352));
          sectorReady_ = true;

          // Determine data size based on mode
          sectorSize_ = (mode_ & (1 << 5)) ? 2340 : 2048;

          // Deliver INT1 and wait for acknowledgement before next sector
          waitingForAck_ = true;
          pushResponse(INT_DATA_READY, {buildStatusByte()});
        }
      }
      currentLba_++;
    }
  }
}

// ─── Push Response ──────────────────────────────────────

void CdromController::pushResponse(CdromInt intType,
                                   std::initializer_list<uint8_t> data) {
  responseFifo_.clear();
  for (auto b : data)
    responseFifo_.push_back(b);
  interruptFlag_ = static_cast<uint8_t>(intType);
  // Fire callback immediately so BIOS events are triggered before the game
  // thread can read/acknowledge the hardware interrupt flag.
  if (interruptCallback_ && intType != INT_NONE) {
    interruptCallback_(static_cast<uint8_t>(intType));
  }
}

// ─── Command Dispatch ───────────────────────────────────

void CdromController::executeCommand(uint8_t cmd) {
  fmt::print("[CDROM] Command 0x{:02X}\n", cmd);

  switch (cmd) {
  case 0x01:
    cmdGetStat();
    break;
  case 0x02:
    cmdSetLoc();
    break;
  case 0x03:
    cmdPlay();
    break;
  case 0x06:
    cmdReadN();
    break;
  case 0x08:
    cmdStop();
    break;
  case 0x09:
    cmdPause();
    break;
  case 0x0A:
    cmdInit();
    break;
  case 0x0B:
    cmdMute();
    break;
  case 0x0C:
    cmdDemute();
    break;
  case 0x0D:
    cmdSetFilter();
    break;
  case 0x0E:
    cmdSetMode();
    break;
  case 0x0F:
    cmdGetParam();
    break;
  case 0x10:
    cmdGetLocL();
    break;
  case 0x11:
    cmdGetLocP();
    break;
  case 0x13:
    cmdGetTN();
    break;
  case 0x14:
    cmdGetTD();
    break;
  case 0x15:
    cmdSeekL();
    break;
  case 0x16:
    cmdSeekP();
    break;
  case 0x19:
    cmdTest();
    break;
  case 0x1A:
    cmdGetID();
    break;
  case 0x1B:
    cmdReadS();
    break;
  default:
    fmt::print("[CDROM] Unknown command: 0x{:02X}\n", cmd);
    pushResponse(INT_ERROR, {buildStatusByte(), 0x40}); // Invalid command
    break;
  }
  paramFifo_.clear();
}

// ─── Command Implementations ────────────────────────────

void CdromController::cmdGetStat() {
  motorOn_ = true;
  pushResponse(INT_ACKNOWLEDGE, {buildStatusByte()});
}

void CdromController::cmdSetLoc() {
  if (paramFifo_.size() >= 3) {
    setLocMinutes_ = fromBcd(paramFifo_[0]);
    setLocSeconds_ = fromBcd(paramFifo_[1]);
    setLocSector_ = fromBcd(paramFifo_[2]);
    seekTarget_ = msfToLba(setLocMinutes_, setLocSeconds_, setLocSector_);
    if (seekTarget_ >= 150)
      seekTarget_ -= 150; // Subtract 2-second pregap
  }
  pushResponse(INT_ACKNOWLEDGE, {buildStatusByte()});
}

void CdromController::cmdPlay() {
  state_ = CdromState::Playing;
  currentLba_ = seekTarget_;
  motorOn_ = true;
  pushResponse(INT_ACKNOWLEDGE, {buildStatusByte()});
}

void CdromController::cmdReadN() {
  motorOn_ = true;
  // NOTE: do NOT clear waitingForAck_ here.  The callback-based approach
  // handles INT1 sequentially: triggerCdromEvent queues the callback,
  // drainPendingCallbacks runs it (DMA copies the sector), THEN clears the
  // gate.  If a new ReadN is issued from inside the callback (queue
  // processing), it happens BEFORE the gate is cleared, which is correct —
  // drainPendingCallbacks clears it afterwards, enabling the first sector
  // of the new read.
  pushResponse(INT_ACKNOWLEDGE, {buildStatusByte()});

  // Transition to reading data state, tick() will handle INT1
  state_ = CdromState::ReadingData;
  currentLba_ = seekTarget_;
  // Speed: bit7 of mode = double speed
  cyclesPerSector_ = (mode_ & 0x80) ? (33868800 / 150) : (33868800 / 75);
  // Small startup delay: prevents first sector from being delivered in the
  // same tick as ReadN, giving the game thread time to set remaining/destPtr.
  // Half a sector's worth of cycles — enough for the game thread to finish
  // its writes, but not so long that the game sees a timeout.
  readCycleCounter_ = -(static_cast<int32_t>(cyclesPerSector_) / 2);
}

void CdromController::cmdReadS() {
  cmdReadN(); // Same as ReadN for our purposes
}

void CdromController::cmdStop() {
  state_ = CdromState::Idle;
  motorOn_ = false;
  pushResponse(INT_ACKNOWLEDGE, {buildStatusByte()});
}

void CdromController::cmdPause() {
  state_ = CdromState::Paused;
  pushResponse(INT_ACKNOWLEDGE, {buildStatusByte()});

  // Secondary response: INT2 (Complete) after the drive stops reading.
  hasSecondaryResponse_ = true;
  secondaryResponseDelay_ = 10000; // Short delay for pause completion
  secondaryInterrupt_ = INT_COMPLETE;
  secondaryData_ = {buildStatusByte()};
}

void CdromController::cmdInit() {
  state_ = CdromState::Idle;
  mode_ = 0;
  motorOn_ = true;

  // Deliver INT3 (Acknowledge)
  pushResponse(INT_ACKNOWLEDGE, {buildStatusByte()});

  // Secondary response: INT2 (Complete) after a short delay
  // This allows the game's polling loop to see INT3, acknowledge it,
  // and then wait for INT2 instead of them arriving simultaneously.
  hasSecondaryResponse_ = true;
  secondaryResponseDelay_ = 20000; // Simulated init delay
  secondaryInterrupt_ = INT_COMPLETE;
  secondaryData_ = {buildStatusByte()};
}

void CdromController::cmdMute() {
  pushResponse(INT_ACKNOWLEDGE, {buildStatusByte()});
}

void CdromController::cmdDemute() {
  pushResponse(INT_ACKNOWLEDGE, {buildStatusByte()});
}

void CdromController::cmdSetFilter() {
  if (paramFifo_.size() >= 2) {
    xaFilterFile_ = paramFifo_[0];
    xaFilterChannel_ = paramFifo_[1];
  }
  pushResponse(INT_ACKNOWLEDGE, {buildStatusByte()});
}

void CdromController::cmdSetMode() {
  if (!paramFifo_.empty()) {
    mode_ = paramFifo_[0];
    xaFilterEnabled_ = (mode_ & (1 << 4)) != 0;
  }
  fmt::print("[CDROM] SetMode: mode_=0x{:02X}\n", mode_);
  pushResponse(INT_ACKNOWLEDGE, {buildStatusByte()});
}

void CdromController::cmdGetParam() {
  pushResponse(INT_ACKNOWLEDGE,
               {buildStatusByte(), mode_, xaFilterFile_, xaFilterChannel_});
}

void CdromController::cmdGetLocL() {
  uint8_t m, s, f;
  lbaToMsf(currentLba_ + 150, m, s, f);
  pushResponse(INT_ACKNOWLEDGE,
               {toBcd(m), toBcd(s), toBcd(f), 0x01, 0x00, 0x00, 0x00, 0x00});
}

void CdromController::cmdGetLocP() {
  uint8_t m, s, f;
  lbaToMsf(currentLba_ + 150, m, s, f);
  pushResponse(INT_ACKNOWLEDGE, {0x01, 0x01, toBcd(m), toBcd(s), toBcd(f),
                                 toBcd(m), toBcd(s), toBcd(f)});
}

void CdromController::cmdGetTN() {
  pushResponse(INT_ACKNOWLEDGE, {buildStatusByte(), 0x01, 0x01}); // 1 track
}

void CdromController::cmdGetTD() {
  // Return disc end position (simplified: 74 minutes)
  pushResponse(INT_ACKNOWLEDGE, {buildStatusByte(), toBcd(74), toBcd(0)});
}

void CdromController::cmdSeekL() {
  currentLba_ = seekTarget_;
  state_ = CdromState::Seeking;
  pushResponse(INT_ACKNOWLEDGE, {buildStatusByte()});

  // Second response after seek completes
  hasSecondaryResponse_ = true;
  secondaryResponseDelay_ = 50000;    // Seek mechanical delay
  secondaryInterrupt_ = INT_COMPLETE; // INT2
  state_ = CdromState::Idle; // State becomes idle immediately after seeking
  secondaryData_ = {buildStatusByte()};
}

void CdromController::cmdSeekP() { cmdSeekL(); }

void CdromController::cmdTest() {
  if (!paramFifo_.empty() && paramFifo_[0] == 0x20) {
    // Get CDROM BIOS date/version
    pushResponse(INT_ACKNOWLEDGE, {0x98, 0x06, 0x10, 0xC3});
  } else {
    pushResponse(INT_ACKNOWLEDGE, {buildStatusByte()});
  }
}

void CdromController::cmdGetID() {
  motorOn_ = true;
  // First response: acknowledge
  pushResponse(INT_ACKNOWLEDGE, {buildStatusByte()});

  // Second response (INT5) after a delay: return licensed game ID
  // SCEI = Sony Computer Entertainment Inc
  // 02,00 = disc type (licensed data disc)
  // 20,00,53,43,45,49 = "  SCEI"
  hasSecondaryResponse_ = true;
  secondaryResponseDelay_ = 20000;    // Arbitrary mechanical delay
  secondaryInterrupt_ = INT_DATA_END; // INT5
  secondaryData_ = {0x02, 0x00, 0x20, 0x00, 0x53, 0x43, 0x45, 0x49};
}

} // namespace ps1::cdrom
