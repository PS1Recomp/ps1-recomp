#include "runtime/bios/bios.h"
#include <cctype>
#include <cstdlib>
#include <fmt/format.h>

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
    : ctx_(ctx), heap_(*ctx.mem), fileIo_(fs, mem) {
  fmt::print("[BIOS] Initialized.\n");
}

Bios::~Bios() = default;

// ─── Entry Points ────────────────────────────────────────

void Bios::executeA0() {
  uint32_t index =
      ctx_.r[T1]; // In PS1 BIOS standard, t1(r9) holds the function index
  handleA0(index);
}

void Bios::executeB0() {
  uint32_t index = ctx_.r[T1];
  handleB0(index);
}

void Bios::executeC0() {
  uint32_t index = ctx_.r[T1];
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

  // ── GPU stubs ──────────────────────────────
  case 0x49: // GPU_cw
    fmt::print("[BIOS] GPU_cw(0x{:08X}) [STUB]\n", ctx_.r[A0]);
    break;
  case 0x4A: // GPU_cwp
    fmt::print("[BIOS] GPU_cwp(addr: 0x{:08X}, count: {}) [STUB]\n", ctx_.r[A0],
               ctx_.r[A1]);
    break;

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
    ctx_.r[V0] = eventSystem_.waitEvent(ctx_.r[A0]);
    break;
  case 0x0B: // testEvent
    ctx_.r[V0] = eventSystem_.testEvent(ctx_.r[A0]);
    break;
  case 0x0C: // enableEvent
    ctx_.r[V0] = eventSystem_.enableEvent(ctx_.r[A0]);
    break;
  case 0x0D: // disableEvent
    ctx_.r[V0] = eventSystem_.disableEvent(ctx_.r[A0]);
    break;
  case 0x12: // InitPAD
    fmt::print("[BIOS] InitPAD(buf1: 0x{:08X}, sz1: {}, buf2: 0x{:08X}, sz2: "
               "{}) [STUB]\n",
               ctx_.r[A0], ctx_.r[A1], ctx_.r[A2], ctx_.r[A3]);
    break;
  case 0x13: // StartPAD
    fmt::print("[BIOS] StartPAD() [STUB]\n");
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
    fmt::print("[BIOS] GetC0Table() [STUB]\n");
    ctx_.r[V0] = 0;
    break;
  case 0x57: // GetB0Table — return pointer to B0 jump table
    fmt::print("[BIOS] GetB0Table() [STUB]\n");
    ctx_.r[V0] = 0;
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

} // namespace ps1::bios
