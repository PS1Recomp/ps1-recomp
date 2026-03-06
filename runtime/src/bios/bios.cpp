#include "runtime/bios/bios.h"
#include "runtime/cdrom/cdrom_controller.h"
#include "runtime/gpu/gpu.h"
#include "runtime/input/input.h"
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>

// Forward declare recomp_dispatch (defined in recompiled_out.cpp, global
// namespace)
extern void recomp_dispatch(uint8_t *rdram, recomp_context *ctx, uint32_t addr);

namespace ps1::bios {

// ─── Helpers ─────────────────────────────────────────────

static uint32_t sRandSeed = 1;

// Read a NUL-terminated string from emulated RAM
static std::string readString(Memory &mem, uint32_t addr) {
  std::string s;
  char c;
  while ((c = mem.read8(addr++)) != '\0') {
    s += c;
  }
  return s;
}

// ─── Constructor / Destructor ────────────────────────────

Bios::Bios(recomp_context &ctx, cdrom::VirtualFs &fs, Memory &mem)
    : ctx_(ctx), heap_(mem), eventSystem_(ctx), fileIo_(fs, mem) {
  // Reset syscall and handlers
  // In a real PS1, vectors are at 0x80000080

  // Set up dummy B0 and C0 tables at the end of RAM (2MB limit)
  // B0 table size: ~0x60 entries * 4 bytes = 0x180 bytes
  // C0 table size: ~0x20 entries * 4 bytes = 0x80 bytes
  uint32_t b0Addr = 0x80000000 + (2 * 1024 * 1024) - 0x200; // 0x801FFE00
  uint32_t c0Addr = b0Addr + 0x180;                         // 0x801FFF80

  // Fill tables with SENTINEL addresses that recomp_dispatch() recognizes.
  // When a game reads GetB0Table()[i] and does `jalr $v0`, $v0 will contain
  // the sentinel 0x0000B0xx, which dispatch maps to BIOS B0 function xx.
  // This replaces the old JR RA approach which broke indirect calls.
  for (int i = 0; i < 0x60; i++) {
    mem.write32(b0Addr + i * 4, 0x0000B000 + i); // Sentinel for B0:i
  }
  for (int i = 0; i < 0x20; i++) {
    mem.write32(c0Addr + i * 4, 0x0000C000 + i); // Sentinel for C0:i
  }

  // Store the table pointers somewhere the BIOS can return them
  ctx_.r[K0] = b0Addr; // Custom scratchpad storage
  ctx_.r[K1] = c0Addr;
}

Bios::~Bios() = default;

// ─── Entry Points ────────────────────────────────────────

void Bios::executeA0() {
  uint32_t index =
      ctx_.r[T1]; // In PS1 BIOS standard, t1(r9) holds the function index
  fmt::print("[BIOS] A0:{:02X} (a0={:08X} a1={:08X} RA={:08X})\n", index,
             ctx_.r[A0], ctx_.r[A1], ctx_.r[RA]);
  handleA0(index);
}

void Bios::executeB0() {
  uint32_t index = ctx_.r[T1];
  fmt::print("[BIOS] B0:{:02X} (a0={:08X} a1={:08X} a2={:08X} RA={:08X})\n",
             index, ctx_.r[A0], ctx_.r[A1], ctx_.r[A2], ctx_.r[RA]);
  handleB0(index);
}

void Bios::executeC0() {
  uint32_t index = ctx_.r[T1];
  fmt::print("[BIOS] C0:{:02X} (a0={:08X} a1={:08X} RA={:08X})\n", index,
             ctx_.r[A0], ctx_.r[A1], ctx_.r[RA]);
  handleC0(index);
}

// ─── Table A (0xA0) ──────────────────────────────────────

void Bios::handleA0(uint32_t index) {
  switch (index) {
  // ── String functions ──────────────────────
  case 0x0A: { // bzero
    uint32_t dst = ctx_.r[A0];
    uint32_t n = ctx_.r[A1];
    for (uint32_t i = 0; i < n; i++)
      ctx_.mem->write8(dst + i, 0);
    break;
  }
  case 0x0B: { // bcmp
    uint32_t s1 = ctx_.r[A0];
    uint32_t s2 = ctx_.r[A1];
    uint32_t n = ctx_.r[A2];
    int res = 0;
    for (uint32_t i = 0; i < n; i++) {
      if (ctx_.mem->read8(s1 + i) != ctx_.mem->read8(s2 + i)) {
        res = 1;
        break;
      }
    }
    ctx_.r[V0] = res;
    break;
  }
  case 0x10: // abs
    ctx_.r[V0] =
        static_cast<uint32_t>(std::abs(static_cast<int32_t>(ctx_.r[A0])));
    break;
  case 0x11: // labs
    ctx_.r[V0] =
        static_cast<uint32_t>(std::abs(static_cast<int32_t>(ctx_.r[A0])));
    break;
  case 0x13: // setjmp — stub: return 0 (first call)
    fmt::print("[BIOS] setjmp(buf: 0x{:08X}) [STUB]\n", ctx_.r[A0]);
    ctx_.r[V0] = 0;
    break;
  case 0x14: // longjmp — stub
    fmt::print("[BIOS] longjmp(buf: 0x{:08X}, val: {}) [STUB]\n", ctx_.r[A0],
               ctx_.r[A1]);
    ctx_.r[V0] = ctx_.r[A1] ? ctx_.r[A1] : 1;
    break;
  case 0x15: { // strcpy
    uint32_t dst = ctx_.r[A0];
    uint32_t src = ctx_.r[A1];
    char c;
    do {
      c = ctx_.mem->read8(src++);
      ctx_.mem->write8(dst++, c);
    } while (c != '\0');
    ctx_.r[V0] = ctx_.r[A0];
    break;
  }
  case 0x16: { // strcmp
    uint32_t s1 = ctx_.r[A0];
    uint32_t s2 = ctx_.r[A1];
    char c1, c2;
    do {
      c1 = ctx_.mem->read8(s1++);
      c2 = ctx_.mem->read8(s2++);
      if (c1 != c2)
        break;
    } while (c1 != '\0');
    ctx_.r[V0] = c1 - c2;
    break;
  }
  case 0x17: { // strlen
    uint32_t s = ctx_.r[A0];
    uint32_t len = 0;
    while (ctx_.mem->read8(s++) != '\0')
      len++;
    ctx_.r[V0] = len;
    break;
  }
  case 0x18: { // strncpy
    uint32_t dst = ctx_.r[A0];
    uint32_t src = ctx_.r[A1];
    uint32_t n = ctx_.r[A2];
    uint32_t i = 0;
    char c;
    while (i < n && (c = ctx_.mem->read8(src++)) != '\0') {
      ctx_.mem->write8(dst++, c);
      i++;
    }
    while (i < n) {
      ctx_.mem->write8(dst++, '\0');
      i++;
    }
    ctx_.r[V0] = ctx_.r[A0];
    break;
  }
  case 0x19: { // strcat
    uint32_t dst = ctx_.r[A0];
    uint32_t src = ctx_.r[A1];
    while (ctx_.mem->read8(dst) != '\0')
      dst++;
    char c;
    do {
      c = ctx_.mem->read8(src++);
      ctx_.mem->write8(dst++, c);
    } while (c != '\0');
    ctx_.r[V0] = ctx_.r[A0];
    break;
  }
  case 0x1A: { // strncmp
    uint32_t s1 = ctx_.r[A0];
    uint32_t s2 = ctx_.r[A1];
    uint32_t n = ctx_.r[A2];
    if (n == 0) {
      ctx_.r[V0] = 0;
      break;
    }
    char c1, c2;
    do {
      c1 = ctx_.mem->read8(s1++);
      c2 = ctx_.mem->read8(s2++);
      if (c1 != c2 || --n == 0)
        break;
    } while (c1 != '\0');
    ctx_.r[V0] = c1 - c2;
    break;
  }
  case 0x1B: { // index (strchr)
    uint32_t s = ctx_.r[A0];
    char c = static_cast<char>(ctx_.r[A1]);
    char curr;
    uint32_t res = 0;
    while ((curr = ctx_.mem->read8(s)) != '\0') {
      if (curr == c) {
        res = s;
        break;
      }
      s++;
    }
    if (c == '\0' && curr == '\0')
      res = s;
    ctx_.r[V0] = res;
    break;
  }
  case 0x1C: { // rindex (strrchr)
    uint32_t s = ctx_.r[A0];
    char c = static_cast<char>(ctx_.r[A1]);
    char curr;
    uint32_t res = 0;
    while ((curr = ctx_.mem->read8(s)) != '\0') {
      if (curr == c)
        res = s;
      s++;
    }
    if (c == '\0')
      res = s;
    ctx_.r[V0] = res;
    break;
  }
  case 0x1D: { // memchr
    uint32_t s = ctx_.r[A0];
    uint8_t c = ctx_.r[A1] & 0xFF;
    uint32_t n = ctx_.r[A2];
    uint32_t res = 0;
    for (uint32_t i = 0; i < n; i++) {
      if (ctx_.mem->read8(s + i) == c) {
        res = s + i;
        break;
      }
    }
    ctx_.r[V0] = res;
    break;
  }
  case 0x1E: // rand
    sRandSeed = sRandSeed * 1103515245 + 12345;
    ctx_.r[V0] = (sRandSeed >> 16) & 0x7FFF;
    break;
  case 0x1F: // srand
    sRandSeed = ctx_.r[A0];
    break;

  // ── Character functions ────────────────────
  case 0x25: // toupper
    ctx_.r[V0] = std::toupper(static_cast<unsigned char>(ctx_.r[A0]));
    break;
  case 0x26: // tolower
    ctx_.r[V0] = std::tolower(static_cast<unsigned char>(ctx_.r[A0]));
    break;

  // ── Memory functions ───────────────────────
  case 0x27: { // bcopy (src, dst, len) — note: args reversed from memcpy
    uint32_t src = ctx_.r[A0];
    uint32_t dst = ctx_.r[A1];
    uint32_t n = ctx_.r[A2];
    for (uint32_t i = 0; i < n; i++)
      ctx_.mem->write8(dst + i, ctx_.mem->read8(src + i));
    break;
  }
  case 0x28: { // bzero (alias)
    uint32_t dst = ctx_.r[A0];
    uint32_t n = ctx_.r[A1];
    for (uint32_t i = 0; i < n; i++)
      ctx_.mem->write8(dst + i, 0);
    break;
  }
  case 0x29: { // bcmp (alias)
    uint32_t s1 = ctx_.r[A0];
    uint32_t s2 = ctx_.r[A1];
    uint32_t n = ctx_.r[A2];
    int res = 0;
    for (uint32_t i = 0; i < n; i++) {
      if (ctx_.mem->read8(s1 + i) != ctx_.mem->read8(s2 + i)) {
        res = 1;
        break;
      }
    }
    ctx_.r[V0] = res;
    break;
  }
  case 0x2A: // memcpy
    for (uint32_t i = 0; i < ctx_.r[A2]; i++) {
      ctx_.mem->write8(ctx_.r[A0] + i, ctx_.mem->read8(ctx_.r[A1] + i));
    }
    ctx_.r[V0] = ctx_.r[A0];
    break;
  case 0x2B: // memset
    for (uint32_t i = 0; i < ctx_.r[A2]; i++) {
      ctx_.mem->write8(ctx_.r[A0] + i, ctx_.r[A1] & 0xFF);
    }
    ctx_.r[V0] = ctx_.r[A0];
    break;
  case 0x2C: { // memmove — handles overlapping regions
    uint32_t dst = ctx_.r[A0];
    uint32_t src = ctx_.r[A1];
    uint32_t n = ctx_.r[A2];
    if (dst < src) {
      for (uint32_t i = 0; i < n; i++)
        ctx_.mem->write8(dst + i, ctx_.mem->read8(src + i));
    } else {
      for (uint32_t i = n; i > 0; i--)
        ctx_.mem->write8(dst + i - 1, ctx_.mem->read8(src + i - 1));
    }
    ctx_.r[V0] = ctx_.r[A0];
    break;
  }
  case 0x2D: { // memcmp
    uint32_t s1 = ctx_.r[A0];
    uint32_t s2 = ctx_.r[A1];
    uint32_t n = ctx_.r[A2];
    int32_t res = 0;
    for (uint32_t i = 0; i < n; i++) {
      int d = ctx_.mem->read8(s1 + i) - ctx_.mem->read8(s2 + i);
      if (d != 0) {
        res = d;
        break;
      }
    }
    ctx_.r[V0] = static_cast<uint32_t>(res);
    break;
  }

  // ── Heap ───────────────────────────────────
  case 0x33: // malloc
    ctx_.r[V0] = heap_.malloc(ctx_.r[A0]);
    break;
  case 0x34: // free
    heap_.free(ctx_.r[A0]);
    break;
  case 0x35: { // calloc
    uint32_t nmemb = ctx_.r[A0];
    uint32_t size = ctx_.r[A1];
    uint32_t total = nmemb * size;
    uint32_t ptr = heap_.malloc(total);
    if (ptr != 0) {
      for (uint32_t i = 0; i < total; i++)
        ctx_.mem->write8(ptr + i, 0);
    }
    ctx_.r[V0] = ptr;
    break;
  }
  case 0x36: { // realloc — simplified: alloc new + copy + free old
    uint32_t oldPtr = ctx_.r[A0];
    uint32_t newSize = ctx_.r[A1];
    if (newSize == 0) {
      heap_.free(oldPtr);
      ctx_.r[V0] = 0;
      break;
    }
    uint32_t newPtr = heap_.malloc(newSize);
    if (oldPtr != 0 && newPtr != 0) {
      // Copy min(oldSize, newSize) — we don't know oldSize, copy newSize
      for (uint32_t i = 0; i < newSize; i++)
        ctx_.mem->write8(newPtr + i, ctx_.mem->read8(oldPtr + i));
      heap_.free(oldPtr);
    }
    ctx_.r[V0] = newPtr;
    break;
  }
  case 0x39: // InitHeap
    heap_.initHeap(ctx_.r[A0], ctx_.r[A1]);
    break;
  case 0x3B: // _exit
    fmt::print("[BIOS] _exit({})\n", static_cast<int32_t>(ctx_.r[A0]));
    break;

  // ── Printf ─────────────────────────────────
  case 0x3F:
    stub_printf();
    break;

  // ── GPU functions ───────────────────────────
  case 0x49: { // GPU_cw — send single GP0 command word
    uint32_t cmd = ctx_.r[A0];
    fmt::print("[BIOS] GPU_cw(0x{:08X})\n", cmd);
    if (gpu_) {
      gpu_->writeGP0(cmd);
    }
    break;
  }
  case 0x4A: { // GPU_cwp — send multiple GP0 command words from RAM
    uint32_t addr = ctx_.r[A0];
    uint32_t count = ctx_.r[A1];
    fmt::print("[BIOS] GPU_cwp(addr: 0x{:08X}, count: {})\n", addr, count);
    if (gpu_) {
      for (uint32_t i = 0; i < count; i++) {
        uint32_t word = ctx_.mem->read32(addr + i * 4);
        gpu_->writeGP0(word);
      }
    }
    break;
  }

  // ── Cache ──────────────────────────────────
  case 0x40: // FlushCache — NOP in recompiler
  case 0x44: // FlushCache (alternate)
    break;

  // ── CD-ROM stubs ───────────────────────────
  case 0x70: // _bu_init
    fmt::print("[BIOS] _bu_init() [STUB]\n");
    break;
  case 0x72: // _96_CdStop
    fmt::print("[BIOS] _96_CdStop() [STUB]\n");
    break;
  case 0x78: // _96_CdAutoPause
    fmt::print("[BIOS] _96_CdAutoPause({}) [STUB]\n", ctx_.r[A0]);
    break;
  case 0x7C: // _96_CdReadSector
    fmt::print("[BIOS] _96_CdReadSector() [STUB]\n");
    ctx_.r[V0] = 0;
    break;

  case 0xAB: // _96_CdInitSubFunc — PsyQ CD initialization sub-function
    fmt::print("[BIOS] _96_CdInitSubFunc({}) — setting up CD internal state\n",
               ctx_.r[A0]);
    // On a real PS1, this initializes the CD exception handler chain.
    // In our HLE environment, the event system is already set up.
    ctx_.r[V0] = 0; // success
    break;

  case 0xAD: { // _96_CdReset — send CdlInit (hardware reset)
    fmt::print("[BIOS] _96_CdReset() — sending CdlInit via CDROM controller\n");
    if (cdrom_) {
      cdrom_->writeRegister(0x1F801800, 0);    // index = 0
      cdrom_->writeRegister(0x1F801801, 0x0A); // CdlInit
    }
    ctx_.r[V0] = 0; // success
    break;
  }

  default:
    fmt::print("[BIOS] Unimplemented A0 call: 0x{:02X} (A0: {:08X}, A1: "
               "{:08X}, A2: {:08X})\n",
               index, ctx_.r[A0], ctx_.r[A1], ctx_.r[A2]);
    break;
  }
}

// ─── Table B (0xB0) ──────────────────────────────────────

void Bios::handleB0(uint32_t index) {
  switch (index) {
  case 0x00: // alloc_kernel_memory
    fmt::print("[BIOS] alloc_kernel_memory({}) [STUB]\n", ctx_.r[A0]);
    ctx_.r[V0] = 0;
    break;
  case 0x07: // DeliverEvent
    eventSystem_.triggerEvent(ctx_.r[A0], ctx_.r[A1]);
    break;
  case 0x08: // openEvent
    ctx_.r[V0] =
        eventSystem_.openEvent(ctx_.r[A0], ctx_.r[A1], ctx_.r[A2], ctx_.r[A3]);
    break;
  case 0x09: // closeEvent
    ctx_.r[V0] = eventSystem_.closeEvent(ctx_.r[A0]);
    break;
  case 0x0A: // waitEvent
    // Drain pending callbacks (game-thread yield point)
    drainPendingCallbacks();
    ctx_.r[V0] = eventSystem_.waitEvent(ctx_.r[A0]);
    break;
  case 0x0B: { // testEvent
    // Drain pending callbacks (game-thread yield point)
    drainPendingCallbacks();
    static int testEventCallCount = 0;
    ctx_.r[V0] = eventSystem_.testEvent(ctx_.r[A0]);
    testEventCallCount++;
    if (testEventCallCount <= 50 || ctx_.r[V0] != 0) {
      fmt::print("[BIOS] B0:0B testEvent({}) -> {} (call #{})\n", ctx_.r[A0],
                 ctx_.r[V0], testEventCallCount);
    }
    break;
  }
  case 0x0C: // enableEvent
    ctx_.r[V0] = eventSystem_.enableEvent(ctx_.r[A0]);
    break;
  case 0x0D: // disableEvent
    ctx_.r[V0] = eventSystem_.disableEvent(ctx_.r[A0]);
    break;
  case 0x12: { // InitPAD — store pad buffer addresses for VBlank polling
    padBuf1Addr_ = ctx_.r[A0];
    padBuf1Size_ = ctx_.r[A1];
    padBuf2Addr_ = ctx_.r[A2];
    padBuf2Size_ = ctx_.r[A3];
    padActive_ = false; // Needs StartPAD to activate
    fmt::print("[BIOS] InitPAD(buf1: 0x{:08X}, sz1: {}, buf2: 0x{:08X}, sz2: "
               "{})\n",
               padBuf1Addr_, padBuf1Size_, padBuf2Addr_, padBuf2Size_);
    break;
  }
  case 0x13: // StartPAD — begin controller polling during VBlank
    padActive_ = true;
    fmt::print("[BIOS] StartPAD() — polling active\n");
    break;
  case 0x14: // StopPAD — stop controller polling
    padActive_ = false;
    fmt::print("[BIOS] StopPAD()\n");
    break;
  case 0x17: { // ReturnFromException
    // Restore PC from EPC (COP0 reg 14) and shift Status Register
    ctx_.pc = ctx_.cop0[COP0_EPC];
    // Shift SR exception stack: bits 5:2 -> bits 3:0
    uint32_t sr = ctx_.cop0[COP0_SR];
    ctx_.cop0[COP0_SR] = (sr & ~0xF) | ((sr >> 2) & 0xF);
    break;
  }
  case 0x18: // SetDefaultExitFromException
    fmt::print("[BIOS] SetDefaultExitFromException() [STUB]\n");
    break;
  case 0x19: // SetCustomExitFromException
    fmt::print("[BIOS] SetCustomExitFromException(handler: 0x{:08X})\n",
               ctx_.r[A0]);
    customExceptionExit_ = ctx_.r[A0];
    break;
  case 0x20: // UnDeliverEvent
    fmt::print("[BIOS] UnDeliverEvent(class: 0x{:X}, spec: 0x{:X}) [STUB]\n",
               ctx_.r[A0], ctx_.r[A1]);
    break;
  case 0x32: // open
  {
    std::string path = readString(*ctx_.mem, ctx_.r[A0]);
    ctx_.r[V0] = fileIo_.open(path.c_str(), ctx_.r[A1]);
  } break;
  case 0x33: // lseek
    ctx_.r[V0] = fileIo_.lseek(ctx_.r[A0], ctx_.r[A1], ctx_.r[A2]);
    break;
  case 0x34: // read
    ctx_.r[V0] = fileIo_.read(ctx_.r[A0], ctx_.r[A1], ctx_.r[A2]);
    break;
  case 0x35: // write — stub (stdout)
    fmt::print("[BIOS] write(fd: {}, src: 0x{:08X}, len: {}) [STUB]\n",
               ctx_.r[A0], ctx_.r[A1], ctx_.r[A2]);
    if (ctx_.r[A0] == 1) { // stdout
      for (uint32_t i = 0; i < ctx_.r[A2]; i++) {
        char c = ctx_.mem->read8(ctx_.r[A1] + i);
        fmt::print("{}", c);
      }
    }
    ctx_.r[V0] = ctx_.r[A2];
    break;
  case 0x36: // close
    ctx_.r[V0] = fileIo_.close(ctx_.r[A0]);
    break;
  case 0x3D: // std_out_putchar
    fmt::print("{}", static_cast<char>(ctx_.r[A0]));
    break;
  case 0x3E: // std_out_puts
  {
    std::string s = readString(*ctx_.mem, ctx_.r[A0]);
    fmt::print("{}\n", s);
  } break;
  case 0x3F: // printf
    stub_printf();
    break;
  case 0x47: // AddDevice
    fmt::print("[BIOS] AddDevice(device_info: 0x{:08X}) [STUB]\n", ctx_.r[A0]);
    ctx_.r[V0] = 1; // Success
    break;
  case 0x48: // RemoveDevice
    fmt::print("[BIOS] RemoveDevice(device_info: 0x{:08X}) [STUB]\n",
               ctx_.r[A0]);
    ctx_.r[V0] = 1;
    break;
  case 0x4A: // InitCARD
    fmt::print("[BIOS] InitCARD(pad_enable: {})\n", ctx_.r[A0]);
    break;
  case 0x4B: // StartCARD
    fmt::print("[BIOS] StartCARD()\n");
    break;
  case 0x4C: // StopCARD
    fmt::print("[BIOS] StopCARD()\n");
    break;
  case 0x56: // GetC0Table — return pointer to C0 jump table
    fmt::print("[BIOS] GetC0Table() -> 0x{:08X}\n", ctx_.r[K1]);
    ctx_.r[V0] = ctx_.r[K1];
    break;
  case 0x57: // GetB0Table — return pointer to B0 jump table
    fmt::print("[BIOS] GetB0Table() -> 0x{:08X}\n", ctx_.r[K0]);
    ctx_.r[V0] = ctx_.r[K0];
    break;
  case 0x5B: // ChangeClearPad
    fmt::print("[BIOS] ChangeClearPad({}) [STUB]\n", ctx_.r[A0]);
    break;
  default:
    fmt::print("[BIOS] Unimplemented B0 call: 0x{:02X} (A0: {:08X}, A1: "
               "{:08X}, A2: {:08X})\n",
               index, ctx_.r[A0], ctx_.r[A1], ctx_.r[A2]);
    break;
  }
}

// ─── Table C (0xC0) ──────────────────────────────────────

void Bios::handleC0(uint32_t index) {
  switch (index) {
  case 0x00: // InstallExceptionHandler — stub
    fmt::print("[BIOS] InstallExceptionHandler() [STUB]\n");
    break;
  case 0x01: // SysEnqIntRP
    // Registers a handler in the interrupt priority chain.
    // On a real PS1 this would add to the exception dispatch chain at
    // the given priority level.  In our HLE, we deliver events directly
    // from the main thread, so the handler chain isn't needed.
    // We still acknowledge the call properly (return 0 = success).
    fmt::print("[BIOS] SysEnqIntRP(priority: {}, handler: 0x{:08X})\n",
               ctx_.r[A0], ctx_.r[A1]);
    ctx_.r[V0] = 0;
    break;
  case 0x02: // SysDeqIntRP
    // Removes a handler from the interrupt priority chain.
    fmt::print("[BIOS] SysDeqIntRP(priority: {}, handler: 0x{:08X})\n",
               ctx_.r[A0], ctx_.r[A1]);
    ctx_.r[V0] = 0;
    break;
  case 0x03: // SysInitMemory
    fmt::print("[BIOS] SysInitMemory(addr: 0x{:08X}, size: {}) [STUB]\n",
               ctx_.r[A0], ctx_.r[A1]);
    break;
  case 0x07: // InstallExceptionHandlers
    fmt::print("[BIOS] InstallExceptionHandlers() [STUB]\n");
    break;
  case 0x08: // SysInitKMem
    fmt::print("[BIOS] SysInitKMem() [STUB]\n");
    break;
  case 0x0A: // ChangeClearRCnt
    // Controls whether root counter interrupts auto-clear after firing.
    // t (A0) = root counter index (0-3), flag (A1) = 0 or 1
    // Returns the old flag value.  We always return 0 (was auto-clear).
    // PsyQ VSync depends on this being called during init to set up RCnt3.
    fmt::print("[BIOS] ChangeClearRCnt(t: {}, flag: {})\n", ctx_.r[A0],
               ctx_.r[A1]);
    ctx_.r[V0] = 0; // return old value
    break;
  case 0x0C: // InitDefInt
    fmt::print("[BIOS] InitDefInt(priority: {}) [STUB]\n", ctx_.r[A0]);
    break;
  default:
    fmt::print("[BIOS] Unimplemented C0 call: 0x{:02X} (A0: {:08X}, A1: "
               "{:08X}, A2: {:08X})\n",
               index, ctx_.r[A0], ctx_.r[A1], ctx_.r[A2]);
    break;
  }
}

// ─── Printf Implementation ──────────────────────────────

// ─── CDROM Event Triggering ──────────────────────────────
//
// The PS1 BIOS CDROM interrupt handler translates CDROM INT types
// into event system notifications.  In a real PS1 the IRQ handler
// at 0x80000080 would do this, but we HLE it here.
//
// Mapping (from PsyQ libcd / BIOS analysis):
//   INT1 (DataReady)   → event spec 0x0004
//   INT2 (Complete)    → event spec 0x8000
//   INT3 (Acknowledge) → event spec 0x0100
//   INT4 (DataEnd)     → event spec 0x0004  (same as DataReady)
//   INT5 (Error)       → event spec 0x2000
//
// Games typically open events for class 0xF4000001 with specs
// 0x0004, 0x8000, 0x0100, 0x2000 — matching the above mapping.
// They may also mirror them on class 0xF0000011.

static constexpr uint32_t CDROM_EVENT_CLASS_1 = 0xF4000001;
static constexpr uint32_t CDROM_EVENT_CLASS_2 = 0xF0000011;

static uint32_t cdIntToEventSpec(uint8_t intType) {
  switch (intType) {
  case 1:
    return 0x0004; // INT1 DataReady
  case 2:
    return 0x8000; // INT2 Complete  (was incorrectly 0x2000)
  case 3:
    return 0x0100; // INT3 Acknowledge
  case 4:
    return 0x0004; // INT4 DataEnd (same as DataReady)
  case 5:
    return 0x2000; // INT5 DiskError (was incorrectly 0x8000)
  default:
    return 0;
  }
}

void Bios::triggerCdromEvent(uint8_t cdIntType) {
  uint32_t spec = cdIntToEventSpec(cdIntType);
  if (spec == 0)
    return;
  fmt::print("[BIOS] triggerCdromEvent: INT{} -> spec 0x{:04X}\n", cdIntType,
             spec);

  // Debug: dump PsyQ CDROM register pointers to verify CdInit initialized them
  if (cdIntType == 1) {
    uint32_t dataCb = ctx_.mem->read32(psyq_.cdDataCb);
    int32_t remaining = (int32_t)ctx_.mem->read32(psyq_.cdRemaining);
    fmt::print(stderr, "[CD-INT1] dataCb=0x{:08X} remaining={}\n", dataCb,
               remaining);
  }

  // ── HLE PsyQ CDROM interrupt handler variables ────
  //
  // On a real PS1 the BIOS exception handler at 0x80000080 reads the CDROM
  // interrupt flag + response FIFO and stores the results in PsyQ-internal
  // RAM variables.  The PsyQ CdSync / CdControl polling functions then check
  // these variables instead of reading CDROM hardware registers directly.
  //
  // In our recompiled environment the exception handler never runs, so we
  // HLE the relevant writes here.
  //
  // PsyQ CD state addresses (configured per-game):
  //   cdSyncByte  — "sync" byte (command completion: INT2/INT3/INT5)
  //   cdReadyByte — "ready" byte (data readiness: INT1/INT4/INT5)
  //   cdStatusHw  — CD status halfword (gate for PsyQ polling code)
  //
  const uint32_t PSYQ_CD_SYNC_BYTE = psyq_.cdSyncByte;
  const uint32_t PSYQ_CD_READY_BYTE = psyq_.cdReadyByte;
  const uint32_t PSYQ_CD_STATUS_HW = psyq_.cdStatusHw;

  // ── HLE sector copy for INT1 (DataReady) ──────────────────────
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
  // On a real PS1 the entire interrupt → callback → ACK chain runs
  // atomically within the interrupt handler.  Here we split it across
  // threads: main thread signals (cdIntPending_), game thread processes
  // (dataCb + ACK).
  //
  if (cdIntType == 1 && cdrom_) {
    cdIntPending_.store(cdIntType, std::memory_order_release);
    // DO NOT ACK here — let the game thread's drainPendingCallbacks
    // handle ACK after dispatching the callback.  This keeps the CDROM
    // in waitingForAck state so tick() won't generate more INT1s.
  }

  // ── Dispatch CustomExceptionExit if registered ──
  // Games like Silent Hill register a custom interrupt handler instead of
  // SysEnqIntRP to process CDROM state directly. If such a handler exists,
  // invoke it so the game logic can run and update PsyQ variables naturally.
  if (customExceptionExit_ != 0) {
    fmt::print("[CDROM-HLE] Triggering custom exception handler 0x{:08X}\n",
               customExceptionExit_);
    // We must emulate the COP0 registers expected during an exception
    uint32_t saved_epc = ctx_.cop0[14];
    uint32_t saved_cause = ctx_.cop0[13];
    uint32_t saved_sr = ctx_.cop0[12];

    // Cause register: Int bit 2 corresponds to hardware interrupts
    ctx_.cop0[13] = 0x400;

    // Simulate calling the handler - the handler should end with RFE + JR RA
    // But the handler itself might have no "JR RA" since it normally returns
    // with ERET/RFE! In the decompiled code, the analyzer flagged the custom
    // handler, but without tracking RFE as a return, the static recompilation
    // of the handler will likely fall through or crash if it expects an
    // exception return. Wait, let's look at what the recompilation of
    // `func_added_80021540` looks like: it was empty. That means it contains no
    // recognized instructions before returning. This confirms the handler was
    // not recompiled properly because of missing ER/RFE tracking or we jumped
    // to the wrong place. If it's empty, calling it won't do anything. We must
    // just rely on the rest of the HLE. Wait, but the whole point is that
    // Silent Hill needs this to update PsyQ. If the handler is empty, we must
    // HLE the update of PsyQ for Silent Hill directly! Wait! Let's just restore
    // registers for now, we'll write the PsyQ variables for Silent Hill
    // directly if we can find them. Or we should figure out how to compile
    // 80021540 properly!

    auto saved_ctx = static_cast<ps1::CPUContext>(ctx_);
    recomp_dispatch(ctx_.mem->ramPtr(), &ctx_, customExceptionExit_);
    static_cast<ps1::CPUContext &>(ctx_) = saved_ctx;

    ctx_.cop0[14] = saved_epc;
    ctx_.cop0[13] = saved_cause;
    ctx_.cop0[12] = saved_sr;
  }

  // PsyQ interrupt handler mapping (as implemented by the game's own IRQ
  // chain):
  //
  //   INT1 (DataReady)  → readyByte = 1          (syncByte untouched)
  //   INT2 (Complete)   → syncByte  = 2          (readyByte untouched)
  //   INT3 (Acknowledge)→ syncByte  = 2 (mapped!) (readyByte untouched)
  //   INT4 (DataEnd)    → readyByte = 4          (syncByte untouched)
  //   INT5 (DiskError)  → syncByte  = 5, readyByte = 5
  //
  // CRITICAL: INT3 maps to syncByte=2 (Complete), NOT 3!  The game's
  // CdCommand function gates on syncByte==2 before issuing new commands.
  // If we wrote 3, subsequent commands would hang waiting for 2.
  //
  // NOTE: We use write32 instead of write8 because some games read these as
  // 32-bit words. Writing the full word ensures upper bytes are zero.
  //
  // NOTE: For INT1, the ready byte is written AFTER the sector copy above
  // completes, so the game thread sees data before the signal.
  //
  switch (cdIntType) {
  case 1: // DataReady → readyByte only
    if (PSYQ_CD_READY_BYTE)
      ctx_.mem->write32(PSYQ_CD_READY_BYTE, 1);
    break;
  case 2: // Complete → syncByte only
    if (PSYQ_CD_SYNC_BYTE)
      ctx_.mem->write32(PSYQ_CD_SYNC_BYTE, 2);
    break;
  case 3: // Acknowledge → syncByte = 2 (mapped to Complete)
    if (PSYQ_CD_SYNC_BYTE)
      ctx_.mem->write32(PSYQ_CD_SYNC_BYTE, 2);
    break;
  case 4: // DataEnd → readyByte only
    if (PSYQ_CD_READY_BYTE)
      ctx_.mem->write32(PSYQ_CD_READY_BYTE, 4);
    break;
  case 5: // DiskError → both
    if (PSYQ_CD_SYNC_BYTE)
      ctx_.mem->write32(PSYQ_CD_SYNC_BYTE, 5);
    if (PSYQ_CD_READY_BYTE)
      ctx_.mem->write32(PSYQ_CD_READY_BYTE, 5);
    break;
  default:
    break;
  }
  fmt::print("[CDROM-HLE] INT{} → sync@0x{:08X}={} ready@0x{:08X}={}\n",
             cdIntType, PSYQ_CD_SYNC_BYTE, ctx_.mem->read32(PSYQ_CD_SYNC_BYTE),
             PSYQ_CD_READY_BYTE, ctx_.mem->read32(PSYQ_CD_READY_BYTE));

  // Write a non-zero status halfword so the PsyQ polling code proceeds
  // past the gate check.  0x0002 = "motor on" status bit.
  if (PSYQ_CD_STATUS_HW != 0) {
    ctx_.mem->write16(PSYQ_CD_STATUS_HW, 0x0002);
  }

  eventSystem_.triggerEvent(CDROM_EVENT_CLASS_1, spec);
  eventSystem_.triggerEvent(CDROM_EVENT_CLASS_2, spec);
}

void Bios::triggerVBlankEvent() {
  // ── Increment PsyQ VBlank counter (Root Counter 3) ────
  //
  // PsyQ stores a VBlank counter that VSync() polls in a tight loop.
  // On a real PS1 this is incremented by the BIOS IRQ handler at interrupt
  // vector 0x80000080.  In our recompiled environment the handler never runs,
  // so we HLE the counter increment here.
  //
  // The address varies per game (PsyQ version / link layout).
  //
  if (psyq_.vblankCounter != 0) {
    uint32_t cnt = ctx_.mem->read32(psyq_.vblankCounter);
    ctx_.mem->write32(psyq_.vblankCounter, cnt + 1);
  }

  // ── Deliver ALL standard PsyQ events that should fire each frame ──
  //
  // On a real PS1, the BIOS exception handler at 0x80000080 dispatches
  // through the SysEnqIntRP chain.  Root counter handlers call DeliverEvent
  // for their respective event classes.  Since SysEnqIntRP is HLE'd (we
  // don't run the real handler chain), we deliver events directly here.
  //
  // PsyQ Event Classes:
  //   0xF0000001 = VBlank IRQ (interrupt class)
  //   0xF2000001 = Root Counter 0 (pixel clock — fires every scanline)
  //   0xF2000002 = Root Counter 1 (horizontal retrace)
  //   0xF2000003 = Root Counter 2 (system clock / 8)
  //
  // Event Specs:
  //   0x0001 = counter reached target value
  //   0x0002 = counter overflow
  //

  // VBlank IRQ event
  eventSystem_.triggerEvent(0xF0000001, 0x0001);

  // Root Counter 0 (pixel clock) — overflows many times per frame
  eventSystem_.triggerEvent(0xF2000001, 0x0001); // target reached
  eventSystem_.triggerEvent(0xF2000001, 0x0002); // overflow

  // Root Counter 1 (horizontal retrace) — ~263 hblanks per frame
  eventSystem_.triggerEvent(0xF2000002, 0x0001); // target reached
  eventSystem_.triggerEvent(0xF2000002, 0x0002); // overflow

  // Root Counter 2 (system clock / 8) — fast freerun timer
  eventSystem_.triggerEvent(0xF2000003, 0x0001);
  eventSystem_.triggerEvent(0xF2000003, 0x0002);

  // NOTE: Memory Card / CARD events (class 0xF4000001) are NOT triggered
  // here anymore.  On a real PS1, class 0xF4000001 is shared between the
  // memory-card SIO handler and the CDROM interrupt handler.  Firing them
  // every VBlank was causing false-positive TestEvent returns that confused
  // the PsyQ CdInit polling loop.  Card events should only be triggered
  // by actual SIO/CDROM hardware activity (i.e. triggerCdromEvent).

  // ── HLE PsyQ display swap callback (SysEnqIntRP replacement) ──────
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
  if (psyq_.gpuSwapCb != 0) {
    eventSystem_.queueCallbackWithArg(psyq_.gpuSwapCb,
                                      4); // a0 = 4 → "swap done"
  }

  // ── HLE PsyQ DrawSync status (GPU ordering table completion) ──
  //
  // PsyQ DrawSync (func_801AA484 in Rayman) works as follows:
  //   1. Reads an OT index from a BSS variable (gpuDrawSyncIndex)
  //   2. Reads a base pointer from another BSS variable (gpuDrawSyncBase)
  //   3. Computes status_addr = base_ptr + (index << 5)  [stride = 0x20 = 32]
  //   4. Reads a halfword at status_addr+0
  //   5. If value == 2, drawing is complete
  //
  // On a real PS1, the GPU VBlank interrupt handler sets this to 2 after
  // the GPU finishes processing the ordering table.  Since our GPU processes
  // GP0 commands synchronously, we can mark it as complete every VBlank.
  //
  // gpuDrawSyncBase: BSS address containing the POINTER to the OT array
  // gpuDrawSyncIndex: BSS address containing the current OT index
  //
  if (psyq_.gpuDrawSyncBase != 0) {
    uint32_t basePtr = ctx_.mem->read32(psyq_.gpuDrawSyncBase);
    if (basePtr != 0) {
      // Mark all possible OT entries as complete
      // PsyQ typically uses 2 double-buffered OT entries (indices 0 and 1)
      for (uint32_t i = 0; i < psyq_.gpuDrawSyncCount; i++) {
        uint32_t statusAddr = basePtr + (i << 5); // stride = 0x20
        ctx_.mem->write16(statusAddr, 2);         // 2 = complete
      }
    }
  }
}

void Bios::drainPendingCallbacks() {
  eventSystem_.drainPendingCallbacks();

  // ── Simulate SysEnqIntRP CDROM interrupt handler chain ──────────
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
  // Maximum sectors to process per call — prevents infinite loops if
  // something goes wrong, and limits time spent in one drain call.
  constexpr int MAX_SECTORS_PER_DRAIN = 32;

  for (int pump = 0; pump < MAX_SECTORS_PER_DRAIN; ++pump) {
    uint8_t intType = cdIntPending_.exchange(0, std::memory_order_acquire);
    if (intType == 0)
      break;

    // Save registers — callbacks clobber temps.
    auto saved = static_cast<ps1::CPUContext>(ctx_);

    const uint32_t PSYQ_CD_DATA_CB = psyq_.cdDataCb;
    const uint32_t PSYQ_CD_NOTIFY_CB = psyq_.cdNotifyCb;

    if (intType == 1 || intType == 4) {
      // INT1 (DataReady) or INT4 (DataEnd) → call dataCb
      uint32_t dataCb = ctx_.mem->read32(PSYQ_CD_DATA_CB);
      int32_t remaining = (int32_t)ctx_.mem->read32(psyq_.cdRemaining);
      static int hleDispatch = 0;
      hleDispatch++;
      if (hleDispatch <= 20) {
        fmt::print(stderr, "[CD-CB] #{} INT{} dataCb=0x{:08X} remaining={}\n",
                   hleDispatch, intType, dataCb, remaining);
      }
      if (dataCb != 0) {
        ctx_.r4 = intType; // a0 = interrupt type
        ctx_.r5 = 0;       // a1 (unused by dataCb in practice)
        recomp_dispatch(ctx_.mem->ramPtr(), &ctx_, dataCb);
      }
    } else if (intType == 2) {
      // INT2 (Complete) → call notifyCb
      uint32_t notifyCb = ctx_.mem->read32(PSYQ_CD_NOTIFY_CB);
      if (notifyCb != 0) {
        ctx_.r4 = intType;
        ctx_.r5 = 0;
        recomp_dispatch(ctx_.mem->ramPtr(), &ctx_, notifyCb);
      }
    }
    // INT3 (Acknowledge) — handled by CdControlF's own polling loop, skip.
    // INT5 (DiskError) — not expected in normal operation.

    // ACK the interrupt so the CDROM can deliver the next sector.
    if (cdrom_ && (intType == 1 || intType == 4)) {
      cdrom_->ackInterrupt(0x1F);
      cdrom_->clearWaitingForAck();
    }

    // Restore registers
    static_cast<ps1::CPUContext &>(ctx_) = saved;

    // ── Pump: try to advance the CDROM so the next sector is ready ──
    //
    // Only pump if the game still has sectors remaining to read.
    // When remaining <= 0, the read is complete — stop feeding cycles
    // so the CDROM transitions out of ReadingData state naturally.
    //
    if (cdrom_ && (intType == 1) && psyq_.cdRemaining != 0) {
      int32_t remainAfterCb = (int32_t)ctx_.mem->read32(psyq_.cdRemaining);
      if (remainAfterCb > 0) {
        // Give enough cycles for the next sector to be "ready".
        cdrom_->tick(0);
        // If the above didn't fire an INT1 (no accumulated cycles), feed
        // one sector's worth of cycles so reads don't stall at 1/frame.
        if (cdIntPending_.load(std::memory_order_relaxed) == 0) {
          cdrom_->tick(cdrom_->getCyclesPerSector());
        }
      } else {
        // Read complete — stop the CDROM so it doesn't generate more INT1s.
        cdrom_->stopReading();
        cdIntPending_.store(0, std::memory_order_release);
      }
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

// ─── Pad Buffer Update (called each VBlank) ─────────────

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

} // namespace ps1::bios
