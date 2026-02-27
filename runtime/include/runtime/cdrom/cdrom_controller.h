#pragma once

// ps1xRuntime — CD-ROM Controller
// PS1 CD-ROM hardware: command FIFO, response FIFO, sector buffer, state
// machine

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <vector>

#include "runtime/cdrom/virtual_fs.h"

namespace ps1::cdrom {

// ─── CD-ROM State Machine ───────────────────────────────
enum class CdromState : uint8_t {
  Idle,
  ReadingData,
  Seeking,
  Playing,
  Paused,
  SpinUp
};

// ─── Interrupt types ────────────────────────────────────
enum CdromInt : uint8_t {
  INT_NONE = 0,
  INT_DATA_READY = 1,
  INT_COMPLETE = 2,
  INT_ACKNOWLEDGE = 3,
  INT_DATA_END = 4,
  INT_ERROR = 5
};

// ─── CD-ROM Controller ──────────────────────────────────
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
  bool hasSectorReady() const { return sectorReady_; }
  void clearSectorReady() { sectorReady_ = false; }

  // XA-ADPCM callback for SPU
  using XaCallback = std::function<void(const int16_t *, uint32_t)>;
  void setXaCallback(XaCallback cb) { xaCallback_ = std::move(cb); }

  // Interrupt callback — fired immediately when pushResponse sets the interrupt flag
  // Used to trigger BIOS events before the game thread can acknowledge the HW interrupt
  using InterruptCallback = std::function<void(uint8_t intType)>;
  void setInterruptCallback(InterruptCallback cb) { interruptCallback_ = std::move(cb); }

  // State
  CdromState getState() const { return state_; }

  // HLE helper: stop an active read (equivalent to CdlPause from the game's perspective)
  void stopReading() { state_ = CdromState::Idle; }

private:
  // ─── State ──────────────────────
  CdromState state_ = CdromState::Idle;
  uint8_t indexReg_ = 0; // Current index (0x1F801800 bits 0-1)

  // ─── FIFOs ──────────────────────
  std::deque<uint8_t> paramFifo_;    // Parameter FIFO (16 byte max)
  std::deque<uint8_t> responseFifo_; // Response FIFO (16 byte max)

  // ─── Interrupt ──────────────────
  uint8_t interruptFlag_ = 0;
  uint8_t interruptEnable_ = 0x1F;

  // ─── Location ───────────────────
  uint8_t setLocMinutes_ = 0;
  uint8_t setLocSeconds_ = 0;
  uint8_t setLocSector_ = 0;
  uint32_t currentLba_ = 0;
  uint32_t seekTarget_ = 0;

  // ─── Mode ───────────────────────
  uint8_t mode_ = 0; // bit7=speed, bit5=sectorSize, bit4=xaFilter,
                     // bit3=xaAdpcm, bit2=wholeSector

  // ─── Sector data ────────────────
  std::array<uint8_t, 2352> sectorBuffer_;
  uint32_t sectorSize_ = 2048;
  bool sectorReady_ = false;

  // ─── Timing ─────────────────────
  uint32_t cyclesUntilResponse_ = 0;
  uint32_t cyclesPerSector_ = 0; // depends on 1x/2x speed
  int32_t readCycleCounter_ = 0; // signed: negative = startup seek delay
  bool waitingForAck_ = false;   // true after INT1 until game acknowledges

  // ─── XA filter ──────────────────
  uint8_t xaFilterFile_ = 0;
  uint8_t xaFilterChannel_ = 0;
  bool xaFilterEnabled_ = false;

  // ─── Motor ──────────────────────
  bool motorOn_ = false;
  bool shellOpen_ = false;

  // ─── External ───────────────────
  VirtualFs *vfs_ = nullptr;
  XaCallback xaCallback_;
  InterruptCallback interruptCallback_;

  // ─── Status byte ────────────────
  uint8_t buildStatusByte() const;

  // ─── Command processing ─────────
  uint8_t pendingCommand_ = 0;
  bool commandPending_ = false;

  // ─── Secondary Response (INT5) ──
  bool hasSecondaryResponse_ = false;
  uint32_t secondaryResponseDelay_ = 0;
  CdromInt secondaryInterrupt_ = INT_NONE;
  std::vector<uint8_t> secondaryData_;

  void executeCommand(uint8_t cmd);
  void cmdGetStat();
  void cmdSetLoc();
  void cmdReadN();
  void cmdReadS();
  void cmdStop();
  void cmdPause();
  void cmdInit();
  void cmdMute();
  void cmdDemute();
  void cmdSetMode();
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
