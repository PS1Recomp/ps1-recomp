#include "bios_internal.h"
#include "runtime/bios/bios.h"
#include "runtime/cdrom/cdrom_controller.h"
#include "runtime/dma/dma.h"
#include "runtime/gpu/gpu.h"
#include "runtime/input/input.h"
#include "runtime/psyq/psyq_state.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fmt/format.h>

// Forward declare recomp_dispatch (defined in recompiled_out.cpp, global
// namespace)
extern void recomp_dispatch(uint8_t *rdram, recomp_context *ctx, uint32_t addr);

namespace ps1::bios {

using detail::readString;

// Constructor / Destructor

Bios::Bios(recomp_context &ctx, cdrom::VirtualFs &fs, Memory &mem)
    : ctx_(ctx), heap_(mem), eventSystem_(ctx), fileIo_(fs, mem) {
  // Reset syscall and handlers
  // In a real PS1, vectors are at 0x80000080

  // Set up A0, B0, and C0 sentinel tables at the end of RAM (2MB limit).
  // When a game reads GetXTable()[i] and does `jalr $v0`, $v0 will contain
  // the sentinel 0x0000X0xx, which recomp_dispatch() maps to the BIOS handler.
  // This covers both direct BIOS calls and indirect calls through stored table
  // pointers (used by some games like Tomba that cache table pointers in BSS).
  //
  // Layout (growing downward from end of RAM):
  //   A0 table: 0xC0 entries * 4 = 0x300 bytes  -> starts at 0x801FFD00
  //   B0 table: 0x60 entries * 4 = 0x180 bytes  -> starts at 0x801FFE00 (was -0x200)
  //   C0 table: 0x20 entries * 4 = 0x080 bytes  -> starts at 0x801FFF80
  uint32_t a0Addr = 0x80000000 + (2 * 1024 * 1024) - 0x500; // 0x801FFB00
  uint32_t b0Addr = 0x80000000 + (2 * 1024 * 1024) - 0x200; // 0x801FFE00
  uint32_t c0Addr = b0Addr + 0x180;                         // 0x801FFF80

  for (int i = 0; i < 0xC0; i++) {
    mem.write32(a0Addr + i * 4, 0x0000A000 + i); // Sentinel for A0:i
  }
  for (int i = 0; i < 0x60; i++) {
    mem.write32(b0Addr + i * 4, 0x0000B000 + i); // Sentinel for B0:i
  }
  for (int i = 0; i < 0x20; i++) {
    mem.write32(c0Addr + i * 4, 0x0000C000 + i); // Sentinel for C0:i
  }

  // Store the table pointers where BIOS calls can return them.
  // K0/K1 are kernel-reserved registers; the game must not rely on them.
  ctx_.r[K0] = b0Addr;
  ctx_.r[K1] = c0Addr;
}

Bios::~Bios() = default;

// Entry Points

void Bios::executeA0() {
  uint32_t index =
      ctx_.r[T1]; // In PS1 BIOS standard, t1(r9) holds the function index
  BIOS_LOG("[BIOS] A0:{:02X} (a0={:08X} a1={:08X} RA={:08X})\n", index,
           ctx_.r[A0], ctx_.r[A1], ctx_.r[RA]);
  handleA0(index);
}

void Bios::executeB0() {
  uint32_t index = ctx_.r[T1];
  BIOS_LOG("[BIOS] B0:{:02X} (a0={:08X} a1={:08X} a2={:08X} RA={:08X})\n",
           index, ctx_.r[A0], ctx_.r[A1], ctx_.r[A2], ctx_.r[RA]);
  handleB0(index);
}

void Bios::executeC0() {
  uint32_t index = ctx_.r[T1];
  BIOS_LOG("[BIOS] C0:{:02X} (a0={:08X} a1={:08X} RA={:08X})\n", index,
           ctx_.r[A0], ctx_.r[A1], ctx_.r[RA]);
  handleC0(index);
}

// Printf Implementation

// CDROM Event Triggering
//
// The PS1 BIOS CDROM interrupt handler translates CDROM INT types
// into event system notifications.  In a real PS1 the IRQ handler
// at 0x80000080 would do this, but we HLE it here.
//
// Mapping (empirically derived from Rayman openEvent calls):
//   Rayman opens class 0xF4000001 with 4 specs: {0x0004, 0x8000, 0x0100, 0x2000}
//   Matching against CDROM INT types:
//   INT1 (DataReady)   -> event spec 0x0100  <- Rayman ID2 (sector read complete)
//   INT2 (Complete)    -> event spec 0x8000  <- Rayman ID1 (init/cmd done)
//   INT3 (Acknowledge) -> event spec 0x0004  <- Rayman ID0 (cmd accepted)
//   INT4 (DataEnd)     -> event spec 0x0100  <- same as DataReady
//   INT5 (DiskError)   -> event spec 0x2000  <- Rayman ID3 (error)

static constexpr uint32_t CDROM_EVENT_CLASS_1 = 0xF4000001;
static constexpr uint32_t CDROM_EVENT_CLASS_2 = 0xF0000011;

static uint32_t cdIntToEventSpec(uint8_t intType) {
  switch (intType) {
  case 1:
    return 0x0100; // INT1 DataReady
  case 2:
    return 0x8000; // INT2 Complete
  case 3:
    return 0x0004; // INT3 Acknowledge
  case 4:
    return 0x0100; // INT4 DataEnd (same class as DataReady)
  case 5:
    return 0x2000; // INT5 DiskError
  default:
    return 0;
  }
}

void Bios::queueCdromEvent(uint8_t cdIntType) {
  // Push under a short mutex.  This is the only cross-thread CDROM IRQ
  // entry-point: `cdromCtrl.setInterruptCallback` (wired in main_host.cpp)
  // routes here from both the SDL-render-thread `tick()` path and the
  // game-thread synchronous `writeRegister` path.  The actual side-effects
  // (psyq_state writes, eventSystem_.triggerEvent, cdIntPending_ set)
  // happen later in `drainCdromEventQueue` on the game thread.
  {
    std::lock_guard<std::mutex> lk(cdEventQueueMtx_);
    cdEventQueue_.push(cdIntType);
  }

  // Inline-drain when the push originates from the game thread itself
  // (recompiled MIPS port writes -> cdromCtrl.writeRegister -> this
  // callback fires synchronously on the game thread).  Recompiled PsyQ
  // kernel polls BSS mirrors right after the port write -- without the
  // inline drain, triggerCdromEvent would only run later (next yield
  // point) and the polling loop would observe stale mirror state.
  // Cross-thread pushes (SDL render thread via tick) skip the drain;
  // they're handled by the next drainPendingCallbacks / hle_libcd_CdSync.
  if (std::this_thread::get_id() == gameThreadId_) {
    drainCdromEventQueue();
  }
}

std::size_t Bios::drainCdromEventQueue() {
  // Pop everything under the mutex into a local queue, then drop the lock
  // before dispatching -- `triggerCdromEvent` may re-enter via cdrom_->tick
  // during the pump-loop pass and we don't want to recursively lock.
  std::queue<uint8_t> local;
  {
    std::lock_guard<std::mutex> lk(cdEventQueueMtx_);
    std::swap(local, cdEventQueue_);
  }
  std::size_t count = local.size();
  while (!local.empty()) {
    triggerCdromEvent(local.front());
    local.pop();
  }
  return count;
}

void Bios::triggerCdromEvent(uint8_t cdIntType) {
  uint32_t spec = cdIntToEventSpec(cdIntType);
  if (spec == 0)
    return;
  BIOS_LOG("[BIOS] triggerCdromEvent: INT{} -> spec 0x{:04X}\n", cdIntType,
           spec);

  // HLE PsyQ CDROM interrupt handler variables
  //
  // On a real PS1 the BIOS exception handler at 0x80000080 reads the CDROM
  // interrupt flag + response FIFO and stores the results in PsyQ-internal
  // RAM variables.  The PsyQ CdSync / CdControl polling functions then check
  // these variables instead of reading CDROM hardware registers directly.
  //
  // In our recompiled environment the exception handler never runs, so we
  // HLE the relevant writes here.  Phase 2.3 moved cdSyncByte/cdReadyByte
  // out of per-game BSS into `psyq_state()` (atomic uint8_t), so VSync-style
  // cooperative polling works without configuring a memory address.  The
  // legacy cdStatusHw write was a one-way mirror with no reader after HLE
  // coverage landed -- dropped entirely.
  //

  // HLE sector copy for INT1 (DataReady)
  //
  // IMPORTANT: Do the sector copy BEFORE writing the ready byte to avoid a
  // race with the game thread's polling loop.  The game thread checks the
  // ready byte and immediately reads the destination buffer, so the data
  // must already be there when the signal is visible.
  //
  // On a real PS1, INT1 fires asynchronously and the interrupt handler chain
  // programs DMA Ch3 to copy the sector, then sets the ready byte.  We HLE
  // the entire sector copy right here on the main thread:
  //   1. Read remaining/destPtr/wordCount from PsyQ BSS
  //   2. memcpy from CDROM sector buffer to game RAM
  //   3. Update destPtr and decrement remaining
  //   4. If remaining <= 0: issue CdlPause so the CDROM stops reading
  //   5. ACK + clear waitingForAck_ so the next sector can arrive
  //
  // APPROACH: Sector copies are handled by the game's own PsyQ callback
  // (dispatched from drainPendingCallbacks on the game thread).
  //
  // We set cdIntPending_ so drainPendingCallbacks on the game thread
  // dispatches the game's dataCb.  The game thread will ACK the interrupt
  // after processing the callback, so the CDROM stays in waitingForAck
  // until then.  This prevents the main-thread tick from racing ahead
  // and firing dozens of INT1s that overwrite the single cdIntPending_.
  //
  // On a real PS1 the entire interrupt -> callback -> ACK chain runs
  // atomically within the interrupt handler.  Here we split it across
  // threads: main thread signals (cdIntPending_), game thread processes
  // (dataCb + ACK).
  //
  if (cdIntType == 1 && cdrom_) {
    cdIntPending_.store(cdIntType, std::memory_order_release);
    // DO NOT ACK here -- let the game thread's drainPendingCallbacks
    // handle ACK after dispatching the callback.  This keeps the CDROM
    // in waitingForAck state so tick() won't generate more INT1s.

    auto &state = ps1::psyq::psyq_state();

    // HLE register-direct sector copy
    // When the game programs DMA Ch3 directly (no libcd HLE in flight:
    // cdRemaining == 0 and cdDataCb == 0), the channel was previously
    // armed before the sector was ready -- `DMA::executeChannel` deferred
    // it and left the start bit set.  Now that INT1 has fired, retry
    // any armed channels so the deferred CDROM transfer runs.  Safe to
    // call unconditionally: other channels won't be triggered unless
    // their own start/trigger bits are set.
    if (dma_ && state.cdRemaining == 0 && state.cdDataCb == 0) {
      dma_->checkAndRunTransfers();
    }

    // HLE libcd sector copy (no callback registered)
    // When the game uses libcd HLE `CdRead` (state.cdRemaining > 0,
    // cdDestPtr/cdWordCount stashed) but never registered
    // `CdReadCallback` (cdDataCb == 0), the drainPendingCallbacks pump
    // would dispatch nothing and ACK -- discarding the sector.  Copy it
    // ourselves to the destination CdRead stashed in psyq_state.
    if (state.cdRemaining > 0 && state.cdDataCb == 0 &&
        cdrom_->hasSectorReady() && state.cdDestPtr != 0) {
      const uint8_t *sector = cdrom_->getSectorBuffer();
      uint32_t sectorSz = cdrom_->getSectorSize();
      // Raw sector (2352 bytes): 12-sync + 4-header + 8-subheader + data.
      // sectorSize=2048 -> user data at +24; sectorSize=2340 -> +12.
      uint32_t dataOff = (sectorSz == 2048) ? 24u : 12u;
      uint32_t words = state.cdWordCount;
      for (uint32_t i = 0; i < words; ++i) {
        uint32_t off = dataOff + i * 4;
        if (off + 3 >= 2352)
          break;
        uint32_t word = static_cast<uint32_t>(sector[off]) |
                        (static_cast<uint32_t>(sector[off + 1]) << 8) |
                        (static_cast<uint32_t>(sector[off + 2]) << 16) |
                        (static_cast<uint32_t>(sector[off + 3]) << 24);
        ctx_.mem->write32(state.cdDestPtr + i * 4, word);
      }
      state.cdDestPtr += words * 4;
      state.cdRemaining -= 1;
      cdrom_->clearSectorReady();
      if (state.cdRemaining == 0)
        cdrom_->stopReading();
    }
  }

  // Update PsyQ CD state in psyq_state() singleton
  //
  // PsyQ interrupt handler mapping (as implemented by the game's own IRQ
  // chain):
  //
  //   INT1 (DataReady)  -> readyByte = 1          (syncByte untouched)
  //   INT2 (Complete)   -> syncByte  = 2          (readyByte untouched)
  //   INT3 (Acknowledge)-> syncByte  = 2 (mapped!) (readyByte untouched)
  //   INT4 (DataEnd)    -> readyByte = 4          (syncByte untouched)
  //   INT5 (DiskError)  -> syncByte  = 5, readyByte = 5
  //
  // CRITICAL: INT3 maps to syncByte=2 (Complete), NOT 3!  The game's
  // CdCommand function gates on syncByte==2 before issuing new commands.
  // If we wrote 3, subsequent commands would hang waiting for 2.
  //
  auto &psyqSync  = ps1::psyq::psyq_state().cdSyncByte;
  auto &psyqReady = ps1::psyq::psyq_state().cdReadyByte;
  switch (cdIntType) {
  case 1: psyqReady.store(1, std::memory_order_release); break;
  case 2: psyqSync.store(2, std::memory_order_release); break;
  case 3: psyqSync.store(2, std::memory_order_release); break; // mapped to Complete
  case 4: psyqReady.store(4, std::memory_order_release); break;
  case 5:
    psyqSync.store(5, std::memory_order_release);
    psyqReady.store(5, std::memory_order_release);
    break;
  default: break;
  }

  // BSS write-through (Phase 2.3 fallout): when the per-game TOML
  // declares `[bss_mirrors]` cd_sync_byte / cd_ready_byte, mirror the
  // atomic value into PS1 RAM so recompiled native MIPS that polls the
  // legacy BSS slot directly (Rayman PsyQ CdReset at 0x801CF1D8 / 0x801CF1DC)
  // observes the new state.  Single byte per slot -- same width as the
  // pre-2.3 BIOS handler used to write.
  if (cdSyncMirror_ != 0)
    ctx_.mem->write8(cdSyncMirror_,
                     psyqSync.load(std::memory_order_acquire));
  if (cdReadyMirror_ != 0)
    ctx_.mem->write8(cdReadyMirror_,
                     psyqReady.load(std::memory_order_acquire));
  BIOS_LOG("[CDROM-HLE] INT{} -> psyq_state.sync={} ready={}\n", cdIntType,
           psyqSync.load(std::memory_order_relaxed),
           psyqReady.load(std::memory_order_relaxed));

  // Dispatch CustomExceptionExit if registered
  //
  // CD interrupts fire SYNCHRONOUSLY from the game thread during CDROM
  // register writes.  If we longjmp here, we abort the game's call stack
  // before it enters its polling loop.
  //
  // On a real PS1, CD interrupts are ASYNCHRONOUS: the game sends a command,
  // enters a polling loop, and the interrupt arrives later.  The BIOS handler
  // processes it and then longjmps (if SetCustomExitFromException was used).
  //
  // We emulate this by DEFERRING the customExceptionExit dispatch: queue the
  // CD interrupt type, and fire the longjmp from drainPendingCallbacks() --
  // which the recompiler inserts into polling loops.  This gives the game time
  // to set up its polling state before the longjmp fires.
  //
  // Do NOT defer customExceptionExit for CD interrupts.
  //
  // CD commands in our implementation execute synchronously on the game
  // thread (writeRegister -> executeCommand -> pushResponse -> callback).
  // Secondary responses (INT2) also fire synchronously via fireSecondaryNow()
  // when the game acks the primary interrupt.  The game's PsyQ polling code
  // reads the hardware interrupt flag register directly and processes the
  // response through register I/O -- it does not need (and is disrupted by)
  // a deferred longjmp.
  //
  // The BSS state (cd_sync_byte, cd_ready_byte, cd_status_hw) is already
  // updated above, which is sufficient for the polling code to detect
  // command completion.

  eventSystem_.triggerEvent(CDROM_EVENT_CLASS_1, spec);
  eventSystem_.triggerEvent(CDROM_EVENT_CLASS_2, spec);
}

void Bios::triggerVBlankEvent() {
  // The Phase-2 PsyQ VBlank counter lives in `psyq_state().vsyncCounter`
  // and is incremented by the host VBlank thread in `main_host.cpp`.
  // This entry-point now exists only to deliver event callbacks (below)
  // and defer CustomExceptionExit longjmps onto the game thread.

  // Defer CustomExceptionExit on VBlank
  //
  // On a real PS1, B0:0x19 (SetCustomExitFromException) installs a jmpbuf
  // that is triggered by ANY hardware exception, including VBlank.
  // Games like Tomba! use this to implement CD command polling: they
  // setjmp, send a CD command, and wait -- the VBlank (or CD INT) fires
  // the longjmp which breaks them out of the wait loop.
  //
  // IMPORTANT: triggerVBlankEvent runs on the VBlank THREAD, not the game
  // thread.  Writing ctx_ registers here is a data race that corrupts game
  // state.  Instead, set a flag and let drainPendingCallbacks() (which runs
  // on the game thread) perform the actual longjmp.
  //
  if (customExceptionRegistered_.load(std::memory_order_acquire)) {
    vblankExceptionPending_.store(1, std::memory_order_release);
    BIOS_LOG("[VBLANK-HLE] CustomExitFromException deferred\n");
  }

  // Deliver ALL standard PsyQ events that should fire each frame
  //
  // On a real PS1, the BIOS exception handler at 0x80000080 dispatches
  // through the SysEnqIntRP chain.  Root counter handlers call DeliverEvent
  // for their respective event classes.  Since SysEnqIntRP is HLE'd (we
  // don't run the real handler chain), we deliver events directly here.
  //
  // PsyQ Event Classes:
  //   0xF0000001 = VBlank IRQ (interrupt class)
  //   0xF2000001 = Root Counter 0 (pixel clock -- fires every scanline)
  //   0xF2000002 = Root Counter 1 (horizontal retrace)
  //   0xF2000003 = Root Counter 2 (system clock / 8)
  //
  // Event Specs:
  //   0x0001 = counter reached target value
  //   0x0002 = counter overflow
  //

  // VBlank IRQ event
  eventSystem_.triggerEvent(0xF0000001, 0x0001);

  // Root Counter 0 (pixel clock) -- overflows many times per frame
  eventSystem_.triggerEvent(0xF2000001, 0x0001); // target reached
  eventSystem_.triggerEvent(0xF2000001, 0x0002); // overflow

  // Root Counter 1 (horizontal retrace) -- ~263 hblanks per frame
  eventSystem_.triggerEvent(0xF2000002, 0x0001); // target reached
  eventSystem_.triggerEvent(0xF2000002, 0x0002); // overflow

  // Root Counter 2 (system clock / 8) -- fast freerun timer
  eventSystem_.triggerEvent(0xF2000003, 0x0001);
  eventSystem_.triggerEvent(0xF2000003, 0x0002);

  // NOTE: triggering 0xF0000011/12/13 (SYSTEM-side root-counter events,
  // as opposed to the USER-side 0xF2000001/2/3 above) caused Rayman boot
  // to enter a fast crash path -- investigated 2026-05-05.  Game opens
  // those classes during init but the path that consumes the events
  // expects them to come ordered with other system signals we don't
  // simulate.  Leaving them un-triggered keeps the game in its
  // CdlInit-retry loop (slower but stable) until that path is mapped.

  // NOTE: Memory Card / CARD events (class 0xF4000001) are NOT triggered
  // here anymore.  On a real PS1, class 0xF4000001 is shared between the
  // memory-card SIO handler and the CDROM interrupt handler.  Firing them
  // every VBlank was causing false-positive TestEvent returns that confused
  // the PsyQ CdInit polling loop.  Card events should only be triggered
  // by actual SIO/CDROM hardware activity (i.e. triggerCdromEvent).

  // HLE PsyQ display swap callback (SysEnqIntRP replacement)
  //
  // On a real PS1, the SysEnqIntRP priority chain includes a PsyQ VBlank
  // handler that manages display double-buffering.  This handler calls
  // the game's swap callback with r4=4 to signal "swap done", which sets
  // a flag that the game's display-ready poll loop checks.
  //
  // Since SysEnqIntRP is stubbed (we don't build the handler chain),
  // we QUEUE this callback for the game thread.  Dispatching it directly
  // from the main thread would be a data race on the shared recomp_context.
  // The game thread's drainPendingCallbacks() will dispatch it safely.
  //
  uint32_t swapCb = ps1::psyq::psyq_state().gpuSwapCb;
  if (swapCb != 0) {
    eventSystem_.queueCallbackWithArg(swapCb,
                                      4); // a0 = 4 -> "swap done"
  }

  // HLE PsyQ DrawSync status (GPU ordering table completion)
  //
  // Since the runtime GPU is fully synchronous (all GP0 commands process
  // inline), every OT slot is "complete" by the time the next VBlank fires.
  // Stamp psyq_state().drawSync.status[] with 2 (= CdlComplete sentinel
  // used by PsyQ DrawSync polling) so any future consumer reading the
  // singleton sees consistent state.  Native PsyQ DrawSync polling against
  // PS1 RAM is short-circuited by the libgpu HLE (`hle_DrawSync` returns 0
  // unconditionally), so no BSS write is needed anymore -- the matcher
  // catches DrawSync in both Rayman and Crash.
  {
    auto &draw = ps1::psyq::psyq_state().drawSync;
    const uint32_t cnt = std::min<uint32_t>(
        draw.count, ps1::psyq::GpuDrawSync::kMaxSlots);
    for (uint32_t i = 0; i < cnt; ++i)
      draw.status[i] = 2;
  }
}

void Bios::drainPendingCallbacks() {
  // Diagnostic — env-gated via `PS1_DRAIN_TRACE=N` (period). Prints
  // every Nth entry + every Nth exit so we can tell whether the
  // function is being re-entered, returns cleanly, or hangs inside.
  // Used to debug the Crash Bandicoot iter-3+ stall (ISSUES.md #1).
  static const long period = []() {
    const char *e = std::getenv("PS1_DRAIN_TRACE");
    return (e && *e) ? std::strtol(e, nullptr, 0) : 0;
  }();
  static std::atomic<long> enter{0};
  static std::atomic<long> exit{0};
  if (period > 0) {
    long n = enter.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n % period == 0) {
      fmt::print(stderr, "[DRAIN] enter#{} (exits={})\n",
                 n, exit.load(std::memory_order_relaxed));
    }
  }

  // Drain any cross-thread CDROM IRQs first -- they may set
  // cdIntPending_ / cdExceptionPending_ / event-system state, which the
  // rest of this method then consumes.  Phase 3.3.
  drainCdromEventQueue();

  eventSystem_.drainPendingCallbacks();

  // Deferred CD customExceptionExit dispatch
  //
  // When a CD interrupt fires synchronously (during a CDROM register write),
  // triggerCdromEvent queues it here instead of longjmping immediately.
  // Now that the game is at a safe yield point (polling loop), we fire the
  // longjmp so the game's exception handler can process the CD result.
  //
  // Check for deferred exceptions (CD or VBlank) -- fire at most one per
  // drainPendingCallbacks call.  CD takes priority over VBlank.
  uint8_t cdExc = cdExceptionPending_.exchange(0, std::memory_order_acquire);
  uint8_t vbExc = vblankExceptionPending_.exchange(0, std::memory_order_acquire);
  if (cdExc != 0 || vbExc != 0) {
    if (customExceptionCallback_) {
      if (cdExc != 0) {
        BIOS_LOG("[CDROM-HLE] Firing deferred customExceptionExit for INT{}\n",
                 cdExc);
      } else {
        BIOS_LOG("[VBLANK-HLE] Firing deferred customExceptionExit\n");
      }
      triggerCustomException();
      if (period > 0) {
        long n = exit.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n % period == 0) {
          fmt::print(stderr, "[DRAIN] exit#{} via customException\n", n);
        }
      }
      return; // longjmp fired -- game handles it
    }
  }

  // Simulate SysEnqIntRP CDROM interrupt handler chain
  //
  // On a real PS1, when a CDROM interrupt fires the BIOS exception handler
  // at 0x80000080 dispatches through the SysEnqIntRP chain.  The PsyQ CD
  // interrupt handler reads HW registers, processes the response, and the
  // chain caller then dispatches dataCb/notifyCb.
  //
  // We dispatch the game's own dataCb/notifyCb here on the GAME thread.
  // After ACKing, we immediately try to advance the CDROM to deliver more
  // sectors (pumping loop), so that reads don't take one frame per sector.
  //
  // Maximum sectors to process per call -- prevents infinite loops if
  // something goes wrong, and limits time spent in one drain call.
  constexpr int MAX_SECTORS_PER_DRAIN = 32;

  auto &state = ps1::psyq::psyq_state();

  for (int pump = 0; pump < MAX_SECTORS_PER_DRAIN; ++pump) {
    // Drain the cross-thread queue at the top of each pump iteration: the
    // previous iteration's `cdrom_->tick(...)` may have fired the IRQ
    // callback inline, which now enqueues onto `cdEventQueue_` instead of
    // calling `triggerCdromEvent` directly.  Draining here populates
    // `cdIntPending_` so the loop body picks up streaming-read INT1s.
    drainCdromEventQueue();

    uint8_t intType = cdIntPending_.exchange(0, std::memory_order_acquire);
    if (intType == 0)
      break;

    // Save registers -- callbacks clobber temps.
    auto saved = static_cast<ps1::CPUContext>(ctx_);

    if (intType == 1 || intType == 4) {
      // INT1 (DataReady) or INT4 (DataEnd) -> call dataCb
      uint32_t dataCb = state.cdDataCb;
      int32_t remaining = static_cast<int32_t>(state.cdRemaining);
      static int hleDispatch = 0;
      hleDispatch++;
      if (detail::biosVerbose() && hleDispatch <= 20) {
        fmt::print(stderr, "[CD-CB] #{} INT{} dataCb=0x{:08X} remaining={}\n",
                   hleDispatch, intType, dataCb, remaining);
      }
      if (dataCb != 0) {
        ctx_.r4 = intType; // a0 = interrupt type
        ctx_.r5 = 0;       // a1 (unused by dataCb in practice)
        recomp_dispatch(ctx_.mem->ramPtr(), &ctx_, dataCb);
      }
    } else if (intType == 2) {
      // INT2 (Complete) -> call notifyCb
      uint32_t notifyCb = state.cdNotifyCb;
      if (notifyCb != 0) {
        ctx_.r4 = intType;
        ctx_.r5 = 0;
        recomp_dispatch(ctx_.mem->ramPtr(), &ctx_, notifyCb);
      }
    }
    // INT3 (Acknowledge) -- handled by CdControlF's own polling loop, skip.
    // INT5 (DiskError) -- not expected in normal operation.

    // ACK the interrupt so the CDROM can deliver the next sector.
    if (cdrom_ && (intType == 1 || intType == 4)) {
      cdrom_->ackInterrupt(0x1F);
      cdrom_->clearWaitingForAck();
    }

    // Restore registers
    static_cast<ps1::CPUContext &>(ctx_) = saved;

    // Pump: try to advance the CDROM so the next sector is ready
    //
    // Only pump if the game still has sectors remaining to read.
    // When remaining <= 0, the read is complete -- stop feeding cycles
    // so the CDROM transitions out of ReadingData state naturally.
    //
    if (cdrom_ && (intType == 1) && state.cdRemaining != 0) {
      int32_t remainAfterCb = static_cast<int32_t>(state.cdRemaining);
      if (remainAfterCb > 0) {
        // Give enough cycles for the next sector to be "ready".
        cdrom_->tick(0);
        // If the above didn't fire an INT1 (no accumulated cycles), feed
        // one sector's worth of cycles so reads don't stall at 1/frame.
        if (cdIntPending_.load(std::memory_order_relaxed) == 0) {
          cdrom_->tick(cdrom_->getCyclesPerSector());
        }
      } else {
        // Read complete -- stop the CDROM so it doesn't generate more INT1s.
        cdrom_->stopReading();
        cdIntPending_.store(0, std::memory_order_release);
      }
    }
  }
  if (period > 0) {
    long n = exit.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n % period == 0) {
      fmt::print(stderr, "[DRAIN] exit#{}\n", n);
    }
  }
}

void Bios::stub_printf() {
  // Read format string from emulated RAM
  uint32_t fmtAddr = ctx_.r[A0];
  std::string fmtStr = readString(*ctx_.mem, fmtAddr);

  // Process basic format specifiers using arguments from $a1, $a2, $a3, stack
  std::string output;
  uint32_t argRegs[] = {ctx_.r[A1], ctx_.r[A2], ctx_.r[A3]};
  int argIdx = 0;
  uint32_t stackArgBase =
      ctx_.r[SP] + 16; // First stack arg after 4 register shadow area

  auto getArg = [&]() -> uint32_t {
    if (argIdx < 3) {
      return argRegs[argIdx++];
    }
    uint32_t val = ctx_.mem->read32(stackArgBase + (argIdx - 3) * 4);
    argIdx++;
    return val;
  };

  for (size_t i = 0; i < fmtStr.size(); i++) {
    if (fmtStr[i] != '%') {
      output += fmtStr[i];
      continue;
    }
    i++;
    if (i >= fmtStr.size())
      break;

    // Skip flags/width/precision modifiers
    while (i < fmtStr.size() &&
           (fmtStr[i] == '-' || fmtStr[i] == '+' || fmtStr[i] == ' ' ||
            fmtStr[i] == '0' || fmtStr[i] == '#'))
      i++;
    while (i < fmtStr.size() && fmtStr[i] >= '0' && fmtStr[i] <= '9')
      i++;
    if (i < fmtStr.size() && fmtStr[i] == '.')
      i++;
    while (i < fmtStr.size() && fmtStr[i] >= '0' && fmtStr[i] <= '9')
      i++;
    // Skip length modifiers (l, h)
    while (i < fmtStr.size() && (fmtStr[i] == 'l' || fmtStr[i] == 'h'))
      i++;

    if (i >= fmtStr.size())
      break;

    switch (fmtStr[i]) {
    case 'd':
    case 'i':
      output += fmt::format("{}", static_cast<int32_t>(getArg()));
      break;
    case 'u':
      output += fmt::format("{}", getArg());
      break;
    case 'x':
      output += fmt::format("{:x}", getArg());
      break;
    case 'X':
      output += fmt::format("{:X}", getArg());
      break;
    case 'p':
      output += fmt::format("0x{:08x}", getArg());
      break;
    case 'c':
      output += static_cast<char>(getArg() & 0xFF);
      break;
    case 's': {
      uint32_t strAddr = getArg();
      if (strAddr != 0) {
        output += readString(*ctx_.mem, strAddr);
      } else {
        output += "(null)";
      }
      break;
    }
    case '%':
      output += '%';
      break;
    default:
      output += '%';
      output += fmtStr[i];
      break;
    }
  }

  fmt::print("{}", output);
}

// Pad Buffer Update (called each VBlank)

void Bios::updatePadBuffers() {
  if (!padActive_ || !input_)
    return;

  // PS1 BIOS pad buffer format (34 bytes per port):
  //   byte 0: status (0x00 = OK, 0xFF = no controller)
  //   byte 1: pad type high nibble + half-word count (e.g., 0x41 = digital)
  //   byte 2: button state low byte  (active-low)
  //   byte 3: button state high byte (active-low)
  //   bytes 4-7: analog data (if analog controller)

  auto writePadBuffer = [&](uint32_t bufAddr, uint32_t bufSize, int port) {
    if (bufAddr == 0 || bufSize < 4)
      return;

    input::PadType padType = input_->getPadType(port);
    uint16_t buttons = input_->buttonState(port);

    if (padType == input::PadType::None) {
      // No controller connected
      ctx_.mem->write8(bufAddr + 0, 0xFF);
      ctx_.mem->write8(bufAddr + 1, 0xFF);
      return;
    }

    ctx_.mem->write8(bufAddr + 0, 0x00);                          // Status OK
    ctx_.mem->write8(bufAddr + 1, static_cast<uint8_t>(padType)); // Type ID
    ctx_.mem->write8(bufAddr + 2, buttons & 0xFF);                // Buttons low
    ctx_.mem->write8(bufAddr + 3, (buttons >> 8) & 0xFF); // Buttons high
  };

  writePadBuffer(padBuf1Addr_, padBuf1Size_, 0);
  writePadBuffer(padBuf2Addr_, padBuf2Size_, 1);
}

void Bios::triggerCustomException() {
  if (customExceptionCallback_)
    customExceptionCallback_();
}

void hle_longjmp_emulator(recomp_context &ctx, Memory &mem, uint32_t buf) {
  // PsyQ jmp_buf layout (matches BIOS exception exit):
  //   +0   RA   +4   SP   +8   FP
  //   +12  S0   ... +40  S7
  //   +44  GP
  uint32_t jmpRA = mem.read32(buf + 0);
  ctx.r[RA] = jmpRA;
  ctx.r[SP] = mem.read32(buf + 4);
  ctx.r[FP] = mem.read32(buf + 8);
  for (int i = 0; i < 8; i++) {
    ctx.r[S0 + i] = mem.read32(buf + 12 + (i * 4));
  }
  ctx.r[GP] = mem.read32(buf + 44);

  // Cause = ext-interrupt (matches what the BIOS exception path leaves behind)
  ctx.cop0[COP0_CAUSE] = 0x400;
  ctx.pc = jmpRA;
  ctx.r[V0] = 1; // longjmp return convention

  // Pop SR exception stack: bits 5:2 -> 3:0
  uint32_t sr = ctx.cop0[COP0_SR];
  ctx.cop0[COP0_SR] = (sr & ~0xFu) | ((sr >> 2) & 0xFu);
}

} // namespace ps1::bios
