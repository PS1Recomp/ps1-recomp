#pragma once
/**
 * @file cdrom_controller.h
 * @brief PS1 CD-ROM drive: command FIFO, response FIFO, sector buffer, state machine.
 *
 * `ps1::cdrom::CdromController` implements the side of the CD-ROM that the
 * game's libcd code talks to via I/O registers at `0x1F801800`-`0x1F801803`.
 * Disc data is sourced from `ps1::cdrom::VirtualFs` (CUE/BIN or ISO 9660).
 *
 * Threading: `writeRegister`/`readRegister` run on the game thread; `tick`
 * runs on the SDL render thread.  IRQs raised from either thread are
 * funnelled through `interruptCallback` and ultimately queued for delivery
 * on the game thread by `Bios::queueCdromEvent` (Phase 3.3).
 */

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <vector>

#include "runtime/cdrom/virtual_fs.h"

namespace ps1::cdrom {

// CD-ROM State Machine
enum class CdromState : uint8_t {
  Idle,
  ReadingData,
  Seeking,
  Playing,
  Paused,
  SpinUp
};

// Interrupt types
enum CdromInt : uint8_t {
  INT_NONE = 0,
  INT_DATA_READY = 1,
  INT_COMPLETE = 2,
  INT_ACKNOWLEDGE = 3,
  INT_DATA_END = 4,
  INT_ERROR = 5
};

/**
 * @brief PS1 CD-ROM drive state machine + register interface.
 *
 * Owns the command/parameter/response FIFOs, the active sector buffer, and
 * the read/seek/play state machine.  Attach a `VirtualFs` before issuing
 * any read commands; without one, reads return "no disc".
 */
class CdromController {
public:
  CdromController();
  ~CdromController() = default;

  void reset();

  // Attach the virtual filesystem for reading sectors
  void attachVirtualFs(VirtualFs *vfs);

  // I/O register access (0x1F801800-0x1F801803)
  void writeRegister(uint32_t addr, uint8_t val);
  uint8_t readRegister(uint32_t addr);

  // Tick: advance state machine by N system clock cycles
  void tick(uint32_t cycles);

  // Interrupt check
  bool hasInterrupt() const { return interruptFlag_ != 0; }
  uint8_t interruptFlag() const { return interruptFlag_; }
  void ackInterrupt(uint8_t val);
  // Clear the INT1 "waiting for ack" gate so tick() can deliver the next sector.
  // Call this AFTER the game's data callback has DMA-copied the current sector.
  void clearWaitingForAck();

  // Get sector data for DMA transfer
  const uint8_t *getSectorBuffer() const { return sectorBuffer_.data(); }
  uint32_t getSectorSize() const { return sectorSize_; }
  uint8_t getMode() const { return mode_; }
  bool hasSectorReady() const { return sectorReady_; }
  void clearSectorReady() { sectorReady_ = false; }
  uint32_t getCyclesPerSector() const { return cyclesPerSector_; }

  // XA-ADPCM callback for SPU
  using XaCallback = std::function<void(const int16_t *, uint32_t)>;
  void setXaCallback(XaCallback cb) { xaCallback_ = std::move(cb); }

  // Interrupt callback -- fired immediately when pushResponse sets the interrupt flag
  // Used to trigger BIOS events before the game thread can acknowledge the HW interrupt
  using InterruptCallback = std::function<void(uint8_t intType)>;
  void setInterruptCallback(InterruptCallback cb) { interruptCallback_ = std::move(cb); }

  // State
  CdromState getState() const { return state_; }

  // HLE helper: stop an active read (equivalent to CdlPause from the game's perspective)
  void stopReading() { state_ = CdromState::Idle; }

  // Returns true if a secondary response (e.g. INT2 after CdlInit INT3) is queued.
  bool hasSecondaryResponse() const { return hasSecondaryResponse_; }

  // Deliver any queued secondary response immediately (e.g. INT2 after CdlInit).
  // Exposed publicly so the BIOS watchpoint can fire it from the game thread.
  void fireSecondaryNow();

  // Discard any pending primary+secondary interrupt without delivering it.
  // Used when the BIOS HLE has already delivered the interrupt synchronously
  // so the controller's own async response doesn't cause a duplicate event.
  void cancelPendingInterrupt() {
    interruptFlag_ = 0;
    hasSecondaryResponse_ = false;
    secondaryResponseDelay_ = 0;
    commandPending_ = false;
    pendingCommand_ = 0;
    responseFifo_.clear();
  }

private:
  // State
  CdromState state_ = CdromState::Idle;
  uint8_t indexReg_ = 0; // Current index (0x1F801800 bits 0-1)

  // FIFOs
  std::deque<uint8_t> paramFifo_;    // Parameter FIFO (16 byte max)
  std::deque<uint8_t> responseFifo_; // Response FIFO (16 byte max)

  // Interrupt
  uint8_t interruptFlag_ = 0;
  uint8_t interruptEnable_ = 0x1F;

  // Location
  uint8_t setLocMinutes_ = 0;
  uint8_t setLocSeconds_ = 0;
  uint8_t setLocSector_ = 0;
  uint32_t currentLba_ = 0;
  uint32_t seekTarget_ = 0;

  // Mode
  uint8_t mode_ = 0; // bit7=speed, bit5=sectorSize, bit4=xaFilter,
                     // bit3=xaAdpcm, bit2=wholeSector

  // Sector data
  std::array<uint8_t, 2352> sectorBuffer_;
  uint32_t sectorSize_ = 2048;
  bool sectorReady_ = false;

  // Timing
  uint32_t cyclesUntilResponse_ = 0;
  uint32_t cyclesPerSector_ = 0; // depends on 1x/2x speed
  int32_t readCycleCounter_ = 0; // signed: negative = startup seek delay
  bool waitingForAck_ = false;   // true after INT1 until game acknowledges

  // XA filter
  uint8_t xaFilterFile_ = 0;
  uint8_t xaFilterChannel_ = 0;
  bool xaFilterEnabled_ = false;

  // Motor
  bool motorOn_ = false;
  bool shellOpen_ = false;

  // External
  VirtualFs *vfs_ = nullptr;
  XaCallback xaCallback_;
  InterruptCallback interruptCallback_;

  // Status byte
  uint8_t buildStatusByte() const;

  // (fireSecondaryNow is now public -- see above)

  // Command processing
  uint8_t pendingCommand_ = 0;
  bool commandPending_ = false;

  // Secondary Response (INT5)
  bool hasSecondaryResponse_ = false;
  uint32_t secondaryResponseDelay_ = 0;
  CdromInt secondaryInterrupt_ = INT_NONE;
  std::vector<uint8_t> secondaryData_;

  void executeCommand(uint8_t cmd);
  void cmdSync();
  void cmdGetStat();
  void cmdSetLoc();
  void cmdReadN();
  void cmdReadS();
  void cmdStop();
  void cmdPause();
  void cmdInit();
  void cmdMute();
  void cmdDemute();
  void cmdSetFilter();
  void cmdSetMode();
  void cmdGetParam();
  void cmdGetLocL();
  void cmdGetLocP();
  void cmdGetTN();
  void cmdGetTD();
  void cmdGetID();
  void cmdSeekL();
  void cmdSeekP();
  void cmdPlay();
  void cmdTest();

  void pushResponse(CdromInt intType, std::initializer_list<uint8_t> data);

  // MSF <-> LBA conversions (public for testing)
public:
  static uint32_t msfToLba(uint8_t m, uint8_t s, uint8_t f);
  static void lbaToMsf(uint32_t lba, uint8_t &m, uint8_t &s, uint8_t &f);
  static uint8_t toBcd(uint8_t val);
  static uint8_t fromBcd(uint8_t bcd);
};

} // namespace ps1::cdrom
