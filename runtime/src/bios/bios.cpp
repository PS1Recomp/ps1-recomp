#include "runtime/bios/bios.h"
#include "runtime/cdrom/cdrom_controller.h"
#include "runtime/gpu/gpu.h"
#include "runtime/input/input.h"
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <fmt/format.h>

// Forward declare recomp_dispatch (defined in recompiled_out.cpp, global namespace)
extern void recomp_dispatch(uint8_t *rdram, recomp_context *ctx,
                            uint32_t addr);

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
    fmt::print("[BIOS] SetCustomExitFromException(handler: 0x{:08X}) [STUB]\n",
               ctx_.r[A0]);
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
    fmt::print("[BIOS] SysEnqIntRP(priority: {}, handler: 0x{:08X}) [STUB]\n",
               ctx_.r[A0], ctx_.r[A1]);
    break;
  case 0x02: // SysDeqIntRP
    fmt::print("[BIOS] SysDeqIntRP(priority: {}, handler: 0x{:08X}) [STUB]\n",
               ctx_.r[A0], ctx_.r[A1]);
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
    fmt::print("[BIOS] ChangeClearRCnt(t: {}, flag: {}) [STUB]\n", ctx_.r[A0],
               ctx_.r[A1]);
    ctx_.r[V0] = 0;
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
// Crash Bandicoot opens events for class 0xF4000001 with specs
// 0x0004, 0x8000, 0x0100, 0x2000 — matching the above mapping.
// It also mirrors them on class 0xF0000011.

static constexpr uint32_t CDROM_EVENT_CLASS_1 = 0xF4000001;
static constexpr uint32_t CDROM_EVENT_CLASS_2 = 0xF0000011;

static uint32_t cdIntToEventSpec(uint8_t intType) {
  switch (intType) {
  case 1:
    return 0x0004; // INT1 DataReady
  case 2:
    return 0x8000; // INT2 Complete
  case 3:
    return 0x0100; // INT3 Acknowledge
  case 4:
    return 0x0004; // INT4 DataEnd
  case 5:
    return 0x2000; // INT5 Error
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
    uint32_t basePtr  = ctx_.mem->read32(0x80055864);
    uint32_t respPtr  = ctx_.mem->read32(0x80055868);
    uint32_t iePtr    = ctx_.mem->read32(0x8005586C);
    uint32_t ifPtr    = ctx_.mem->read32(0x80055870);
    uint32_t dataCb   = ctx_.mem->read32(0x800555A4);
    int32_t  remaining = (int32_t)ctx_.mem->read32(0x80055898);
    fmt::print(stderr, "[CD-INT1] base=0x{:08X} resp=0x{:08X} ie=0x{:08X} if=0x{:08X} dataCb=0x{:08X} remaining={}\n",
               basePtr, respPtr, iePtr, ifPtr, dataCb, remaining);
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
  // Key PsyQ CD state addresses (Crash Bandicoot USA):
  //   0x8005587C  — "sync" byte (command completion: INT2/INT3/INT5)
  //   0x8005587D  — "ready" byte (data readiness: INT1/INT4/INT5)
  //   0x80053936  — CD status halfword (gate for func_8003E848, non-zero
  //                 lets the polling code proceed to HW register access)
  //
  constexpr uint32_t PSYQ_CD_SYNC_BYTE  = 0x8005587C;
  constexpr uint32_t PSYQ_CD_READY_BYTE = 0x8005587D;
  constexpr uint32_t PSYQ_CD_STATUS_HW  = 0x80053936;

  // PsyQ interrupt handler writes different bytes per INT type:
  //   INT1 (DataReady)  → ready byte = 1
  //   INT2 (Complete)   → sync byte  = 2
  //   INT3 (Acknowledge)→ sync byte  = 3 (or 2 for simple cmds)
  //   INT4 (DataEnd)    → ready byte = 4
  //   INT5 (DiskError)  → both       = 5
  //
  switch (cdIntType) {
  case 1: // DataReady → ready byte
  case 4: // DataEnd   → ready byte
    ctx_.mem->write8(PSYQ_CD_READY_BYTE, cdIntType);
    break;
  case 2: // Complete    → sync byte
  case 3: // Acknowledge → sync byte
    ctx_.mem->write8(PSYQ_CD_SYNC_BYTE, cdIntType);
    break;
  case 5: // DiskError → both
    ctx_.mem->write8(PSYQ_CD_SYNC_BYTE,  5);
    ctx_.mem->write8(PSYQ_CD_READY_BYTE, 5);
    break;
  default:
    break;
  }

  // Write a non-zero status halfword so func_8003E848 returns non-zero,
  // allowing the polling code to proceed past the gate check.
  // 0x0002 = "motor on" status bit – a reasonable default.
  ctx_.mem->write16(PSYQ_CD_STATUS_HW, 0x0002);

  eventSystem_.triggerEvent(CDROM_EVENT_CLASS_1, spec);
  eventSystem_.triggerEvent(CDROM_EVENT_CLASS_2, spec);

  // ── HLE sector copy for INT1 (DataReady) ──────────────────────
  //
  // On a real PS1, INT1 fires asynchronously and the interrupt handler chain
  // calls func_80045C04 which programs DMA Ch3 to copy the sector.  In our
  // recompiled environment, interruptFlag_ can be overwritten by subsequent
  // command INT3s before any game-thread callback runs.
  //
  // Instead, we HLE the entire sector copy right here on the main thread:
  //   1. Read remaining/destPtr/wordCount from PsyQ BSS
  //   2. memcpy from CDROM sector buffer to game RAM
  //   3. Update destPtr and decrement remaining
  //   4. If remaining <= 0: issue CdlPause so the CDROM stops reading
  //   5. ACK + clear waitingForAck_ so the next sector can arrive
  //
  if (cdIntType == 1 && cdrom_) {
    constexpr uint32_t PSYQ_REMAINING = 0x80055898;
    constexpr uint32_t PSYQ_DEST_PTR = 0x8005588C;
    constexpr uint32_t PSYQ_WORD_CNT = 0x80055894;

    int32_t remaining = (int32_t)ctx_.mem->read32(PSYQ_REMAINING);

    if (remaining > 0) {
      uint32_t destPtr  = ctx_.mem->read32(PSYQ_DEST_PTR);
      uint32_t wordCount = ctx_.mem->read32(PSYQ_WORD_CNT);
      uint32_t byteCount = wordCount * 4;

      // Determine sector data offset based on current mode
      uint32_t dataOffset = (cdrom_->getMode() & (1 << 5)) ? 12 : 24;
      const uint8_t *sectorData = cdrom_->getSectorBuffer() + dataOffset;

      // Copy sector data to game RAM
      uint8_t *ramDest = ctx_.mem->ramPtr() + (destPtr & 0x1FFFFF);
      std::memcpy(ramDest, sectorData, byteCount);

      // Advance destination pointer and decrement remaining
      destPtr += byteCount;
      remaining--;
      ctx_.mem->write32(PSYQ_DEST_PTR, destPtr);
      ctx_.mem->write32(PSYQ_REMAINING, (uint32_t)remaining);

      static int hleCopyCount = 0;
      hleCopyCount++;
      if (hleCopyCount <= 30) {
        fmt::print(stderr, "[CD-HLE-COPY] #{} dest=0x{:08X} words={} rem={}\n",
                   hleCopyCount, destPtr - byteCount, wordCount, remaining);
      }

      // If all sectors for this batch are done, issue CdlPause
      if (remaining <= 0) {
        cdrom_->writeRegister(0x1F801800, 0);   // index = 0
        cdrom_->writeRegister(0x1F801801, 0x09); // CdlPause command
      }
    }

    // ACK the interrupt so the CDROM can deliver the next sector
    cdrom_->ackInterrupt(0x1F);
    cdrom_->clearWaitingForAck();
  }
}

void Bios::triggerVBlankEvent() {
  // ── Increment PsyQ VBlank counter (Root Counter 3) ────
  //
  // PsyQ stores a VBlank counter at 0x800549F0 that VSync() polls in a tight
  // loop.  On a real PS1 this is incremented by the BIOS IRQ handler at
  // interrupt vector 0x80000080.  In our recompiled environment the handler
  // never runs, so we HLE the counter increment here.
  //
  // The counter lives in emulated RAM — just read/write through Memory.
  //
  constexpr uint32_t VBLANK_COUNTER_ADDR = 0x800549F0;  // rcnt[3] counter
  uint32_t cnt = ctx_.mem->read32(VBLANK_COUNTER_ADDR);
  ctx_.mem->write32(VBLANK_COUNTER_ADDR, cnt + 1);

  // VBlank event class: 0xF2000002 (Root Counter 2 / VSync)
  eventSystem_.triggerEvent(0xF2000002, 0x0002);
  // Also some games use 0xF0000001 for VBlank
  eventSystem_.triggerEvent(0xF0000001, 0x0002);
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
  // We can't route through func_80043BA8 because interruptFlag_ may have
  // been overwritten by a subsequent command's INT3 (pushResponse overwrites).
  // Instead we directly HLE the callback dispatch using the stored INT type.
  //
  uint8_t intType = cdIntPending_.exchange(0, std::memory_order_acquire);
  if (intType != 0) {
    // Save registers — callbacks clobber temps.
    auto saved = static_cast<ps1::CPUContext>(ctx_);

    constexpr uint32_t PSYQ_CD_DATA_CB   = 0x800555A4;
    constexpr uint32_t PSYQ_CD_NOTIFY_CB = 0x800555A0;

    if (intType == 1 || intType == 4) {
      // INT1 (DataReady) or INT4 (DataEnd) → call dataCb
      uint32_t dataCb = ctx_.mem->read32(PSYQ_CD_DATA_CB);
      int32_t remaining = (int32_t)ctx_.mem->read32(0x80055898);
      static int hleDispatch = 0;
      hleDispatch++;
      if (hleDispatch <= 20) {
        fmt::print(stderr, "[CD-HLE] #{} INT{} dataCb=0x{:08X} remaining={}\n",
                   hleDispatch, intType, dataCb, remaining);
      }
      if (dataCb != 0) {
        ctx_.r4 = intType;   // a0 = interrupt type
        ctx_.r5 = 0;         // a1 (unused by dataCb in practice)
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
