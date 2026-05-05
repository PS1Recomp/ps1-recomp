#include "runtime/bios/bios.h"
#include "runtime/cdrom/cdrom_controller.h"
#include "runtime/gpu/gpu.h"
#include "runtime/input/input.h"
#include "runtime/psyq/psyq_state.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>

// Forward declare recomp_dispatch (defined in recompiled_out.cpp, global
// namespace)
extern void recomp_dispatch(uint8_t *rdram, recomp_context *ctx, uint32_t addr);

namespace ps1::bios {

// ─── Helpers ─────────────────────────────────────────────

// Set PS1_BIOS_DEBUG=1 in the environment to enable verbose per-call logging.
static const bool s_biosVerbose = (std::getenv("PS1_BIOS_DEBUG") != nullptr);

// Convenience macro: only prints when verbose mode is enabled.
// Usage: BIOS_LOG("[BIOS] {}\n", value);
#define BIOS_LOG(...) do { if (s_biosVerbose) fmt::print(__VA_ARGS__); } while(0)

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

  // Set up A0, B0, and C0 sentinel tables at the end of RAM (2MB limit).
  // When a game reads GetXTable()[i] and does `jalr $v0`, $v0 will contain
  // the sentinel 0x0000X0xx, which recomp_dispatch() maps to the BIOS handler.
  // This covers both direct BIOS calls and indirect calls through stored table
  // pointers (used by some games like Tomba that cache table pointers in BSS).
  //
  // Layout (growing downward from end of RAM):
  //   A0 table: 0xC0 entries * 4 = 0x300 bytes  → starts at 0x801FFD00
  //   B0 table: 0x60 entries * 4 = 0x180 bytes  → starts at 0x801FFE00 (was -0x200)
  //   C0 table: 0x20 entries * 4 = 0x080 bytes  → starts at 0x801FFF80
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

// ─── Entry Points ────────────────────────────────────────

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
  case 0x13: { // setjmp
    uint32_t buf = ctx_.r[A0];
    BIOS_LOG("[BIOS] setjmp(buf: 0x{:08X})\n", buf);
    // PsyQ jmp_buf is an array of 12 integers (48 bytes):
    // [0]=RA [1]=SP [2]=FP [3]=S0 [4]=S1 [5]=S2 [6]=S3 [7]=S4 [8]=S5 [9]=S6
    // [10]=S7 [11]=GP
    ctx_.mem->write32(buf + 0, ctx_.r[31]); // RA
    ctx_.mem->write32(buf + 4, ctx_.r[29]); // SP
    ctx_.mem->write32(buf + 8, ctx_.r[30]); // FP
    for (int i = 0; i < 8; i++) {
      ctx_.mem->write32(buf + 12 + (i * 4), ctx_.r[16 + i]); // S0-S7
    }
    ctx_.mem->write32(buf + 44, ctx_.r[28]); // GP
    ctx_.r[V0] = 0;
    break;
  }
  case 0x14: { // longjmp
    uint32_t buf = ctx_.r[A0];
    uint32_t val = ctx_.r[A1];
    BIOS_LOG("[BIOS] longjmp(buf: 0x{:08X}, val: {})\n", buf, val);

    ctx_.r[31] = ctx_.mem->read32(buf + 0); // RA
    ctx_.r[29] = ctx_.mem->read32(buf + 4); // SP
    ctx_.r[30] = ctx_.mem->read32(buf + 8); // FP
    for (int i = 0; i < 8; i++) {
      ctx_.r[16 + i] = ctx_.mem->read32(buf + 12 + (i * 4)); // S0-S7
    }
    ctx_.r[28] = ctx_.mem->read32(buf + 44); // GP

    ctx_.pc = ctx_.r[31]; // Jump to RA
    ctx_.r[V0] = val ? val : 1;
    break;
  }
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
    BIOS_LOG("[BIOS] _exit({})\n", static_cast<int32_t>(ctx_.r[A0]));
    break;

  // ── Printf ─────────────────────────────────
  case 0x3F:
    stub_printf();
    break;

  // ── GPU functions ───────────────────────────
  case 0x46: { // GPU_dw — copy data block to GP0 (used by PutDispEnv init)
    // A0=src_addr, A1=count (words). Sends each word to GP0.
    uint32_t addr = ctx_.r[A0];
    uint32_t count = ctx_.r[A1];
    BIOS_LOG("[BIOS] GPU_dw(addr: 0x{:08X}, count: {})\n", addr, count);
    if (gpu_) {
      for (uint32_t i = 0; i < count; i++) {
        uint32_t word = ctx_.mem->read32(addr + i * 4);
        gpu_->writeGP0(word);
      }
    }
    break;
  }
  case 0x47: { // gpu_send_dma — wait for DMA, then trigger GPU DMA
    BIOS_LOG("[BIOS] gpu_send_dma() [STUB]\n");
    ctx_.r[V0] = 0;
    break;
  }
  case 0x48: { // SendGP1Command — write single word to GP1 (0x1F801814)
    // PutDispEnv calls this to set display area (GP1 0x05), ranges (0x06/0x07),
    // and display mode (0x08). Critical for double-buffer display swap.
    uint32_t cmd = ctx_.r[A0];
    BIOS_LOG("[BIOS] SendGP1Command(0x{:08X})\n", cmd);
    if (gpu_) {
      gpu_->writeGP1(cmd);
    }
    ctx_.r[V0] = cmd;
    break;
  }
  case 0x4B: { // send_gpu_linked_list — send GP0 OT via DMA-like walk
    // A0 = pointer to linked list tail. Walk the list sending GP0 commands.
    // Each entry: [23:0]=next_ptr, [31:24]=word_count, followed by count words.
    uint32_t ptr = ctx_.r[A0];
    BIOS_LOG("[BIOS] send_gpu_linked_list(0x{:08X})\n", ptr);
    if (gpu_) {
      int safety = 0;
      while ((ptr & 0xFFFFFF) != 0xFFFFFF && safety++ < 100000) {
        uint32_t hdr = ctx_.mem->read32(ptr);
        uint32_t wordCount = (hdr >> 24) & 0xFF;
        for (uint32_t i = 0; i < wordCount; i++) {
          gpu_->writeGP0(ctx_.mem->read32(ptr + 4 + i * 4));
        }
        ptr = hdr & 0xFFFFFF;
        if (ptr != 0xFFFFFF) ptr |= 0x80000000u; // restore KSEG0 bit
      }
    }
    break;
  }
  case 0x4D: { // GetGPUStatus — read GPUSTAT register
    ctx_.r[V0] = gpu_ ? gpu_->readGPUSTAT() : 0x14802000;
    break;
  }
  case 0x4E: { // gpu_sync — wait for GPU to be idle
    // Our GPU is always synchronous, so this is a NOP.
    ctx_.r[V0] = 0;
    break;
  }
  case 0x49: { // GPU_cw — send single GP0 command word
    uint32_t cmd = ctx_.r[A0];
    BIOS_LOG("[BIOS] GPU_cw(0x{:08X})\n", cmd);
    if (gpu_) {
      gpu_->writeGP0(cmd);
    }
    break;
  }
  case 0x4A: { // GPU_cwp — send multiple GP0 command words from RAM
    uint32_t addr = ctx_.r[A0];
    uint32_t count = ctx_.r[A1];
    BIOS_LOG("[BIOS] GPU_cwp(addr: 0x{:08X}, count: {})\n", addr, count);
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
    BIOS_LOG("[BIOS] _bu_init() [STUB]\n");
    break;
  case 0x72: // _96_CdStop
    BIOS_LOG("[BIOS] _96_CdStop() [STUB]\n");
    break;
  case 0x78: // _96_CdAutoPause
    BIOS_LOG("[BIOS] _96_CdAutoPause({}) [STUB]\n", ctx_.r[A0]);
    break;
  case 0x7C: // _96_CdReadSector
    BIOS_LOG("[BIOS] _96_CdReadSector() [STUB]\n");
    ctx_.r[V0] = 0;
    break;

  case 0xAB: // _96_CdInitSubFunc — PsyQ CD initialization sub-function
    BIOS_LOG("[BIOS] _96_CdInitSubFunc({}) — firing INT3+INT2\n",
               ctx_.r[A0]);
    // On real hardware _96_CdInitSubFunc runs as the INT2 completion handler
    // for the CdlInit command.  Before returning it sends a GetStat (0x01)
    // which immediately generates INT3 (Ack).  The CdInit polling loop then
    // checks testEvent(0) [spec=0x0004 = INT3] to verify the drive responded.
    // We fire INT3 first so that testEvent(0) succeeds, then INT2 for event 1.
    triggerCdromEvent(3); // INT3 Ack  → spec 0x0004 → event 0 (GetStat Ack)
    triggerCdromEvent(2); // INT2 Complete → spec 0x8000 → event 1
    ctx_.r[V0] = 0; // success
    break;

  case 0xAC: { // _96_CdGetStatus — send GetStat to query disc/drive state
    // PsyQ CdInit calls this after _96_CdInitSubFunc to probe disc presence.
    // On real HW: sends GetStat(0x01), which fires INT3(Ack) with status byte.
    // We deliver INT3 synchronously and cancel the controller's pending response
    // to prevent double-interrupt for games using SetCustomExitFromException.
    BIOS_LOG("[BIOS] _96_CdGetStatus() — HLE GetStat (synchronous)\n");
    if (cdrom_) {
      cdrom_->writeRegister(0x1F801800, 0);    // index = 0
      cdrom_->writeRegister(0x1F801801, 0x01); // GetStat
      cdrom_->cancelPendingInterrupt();
    }
    triggerCdromEvent(3); // INT3 Ack → disc-present status
    ctx_.r[V0] = 0;
    break;
  }

  case 0xAD: { // _96_CdReset — send CdlInit (hardware reset)
    BIOS_LOG("[BIOS] _96_CdReset() — HLE CdlInit (synchronous)\n");
    // Initialize the CDROM controller state without queuing an async response.
    // We deliver INT3+INT2 synchronously below, so we DON'T want the controller
    // to fire its own deferred INT3+INT2 later (that would cause double-events,
    // disrupting games that use SetCustomExitFromException for CD interrupts).
    if (cdrom_) {
      cdrom_->writeRegister(0x1F801800, 0);    // index = 0
      cdrom_->writeRegister(0x1F801801, 0x0A); // CdlInit → sets controller state
      // Discard the queued response so tick() doesn't fire a duplicate event.
      cdrom_->cancelPendingInterrupt();
    }
    // Fire INT3+INT2 immediately on the game thread so CdSync/testEvent/
    // SetCustomExitFromException handlers see the response right away.
    triggerCdromEvent(3); // INT3 Ack      → cdSyncByte = 2
    triggerCdromEvent(2); // INT2 Complete → cdSyncByte = 2
    ctx_.r[V0] = 0; // success
    break;
  }

  default:
    BIOS_LOG("[BIOS] Unimplemented A0 call: 0x{:02X} (A0: {:08X}, A1: "
               "{:08X}, A2: {:08X})\n",
               index, ctx_.r[A0], ctx_.r[A1], ctx_.r[A2]);
    break;
  }
}

// ─── Table B (0xB0) ──────────────────────────────────────

void Bios::handleB0(uint32_t index) {
  switch (index) {
  case 0x00: // alloc_kernel_memory
    BIOS_LOG("[BIOS] alloc_kernel_memory({}) [STUB]\n", ctx_.r[A0]);
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
      BIOS_LOG("[BIOS] B0:0B testEvent({}) -> {} (call #{})\n", ctx_.r[A0],
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
    BIOS_LOG("[BIOS] InitPAD(buf1: 0x{:08X}, sz1: {}, buf2: 0x{:08X}, sz2: "
               "{})\n",
               padBuf1Addr_, padBuf1Size_, padBuf2Addr_, padBuf2Size_);

    // Pre-fill pad buffers immediately so the game doesn't read uninitialized
    // zeros (which active-low encoding interprets as "all buttons pressed").
    // Write: status=OK(0x00), type=Digital(0x41), no-buttons(0xFF,0xFF).
    auto initBuf = [&](uint32_t addr, uint32_t sz) {
      if (addr == 0 || sz < 4) return;
      ctx_.mem->write8(addr + 0, 0x00); // status OK
      ctx_.mem->write8(addr + 1, 0x41); // Digital pad
      ctx_.mem->write8(addr + 2, 0xFF); // no buttons pressed (active-low)
      ctx_.mem->write8(addr + 3, 0xFF); // no buttons pressed
    };
    initBuf(padBuf1Addr_, padBuf1Size_);
    initBuf(padBuf2Addr_, padBuf2Size_);
    break;
  }
  case 0x13: // StartPAD — begin controller polling during VBlank
    padActive_ = true;
    BIOS_LOG("[BIOS] StartPAD() — polling active\n");
    break;
  case 0x14: // StopPAD — stop controller polling
    padActive_ = false;
    BIOS_LOG("[BIOS] StopPAD()\n");
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
    BIOS_LOG("[BIOS] SetDefaultExitFromException() [STUB]\n");
    break;
  case 0x19: { // SetCustomExitFromException
    uint32_t buf = ctx_.r[A0];
    BIOS_LOG("[BIOS] SetCustomExitFromException(handler: 0x{:08X})\n", buf);
    if (buf == 0) {
      customExceptionCallback_ = nullptr;
      customExceptionRegistered_.store(false, std::memory_order_release);
    } else {
      customExceptionCallback_ = [this, buf]() {
        hle_longjmp_emulator(ctx_, *ctx_.mem, buf);
      };
      customExceptionRegistered_.store(true, std::memory_order_release);
    }
    break;
  }
  case 0x20: // UnDeliverEvent
    BIOS_LOG("[BIOS] UnDeliverEvent(class: 0x{:X}, spec: 0x{:X}) [STUB]\n",
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
    BIOS_LOG("[BIOS] write(fd: {}, src: 0x{:08X}, len: {}) [STUB]\n",
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
    BIOS_LOG("[BIOS] AddDevice(device_info: 0x{:08X}) [STUB]\n", ctx_.r[A0]);
    ctx_.r[V0] = 1; // Success
    break;
  case 0x48: // RemoveDevice
    BIOS_LOG("[BIOS] RemoveDevice(device_info: 0x{:08X}) [STUB]\n",
               ctx_.r[A0]);
    ctx_.r[V0] = 1;
    break;
  case 0x4A: // InitCARD
    BIOS_LOG("[BIOS] InitCARD(pad_enable: {})\n", ctx_.r[A0]);
    break;
  case 0x4B: // StartCARD
    BIOS_LOG("[BIOS] StartCARD()\n");
    break;
  case 0x4C: // StopCARD
    BIOS_LOG("[BIOS] StopCARD()\n");
    break;
  case 0x56: // GetC0Table — return pointer to C0 jump table
    BIOS_LOG("[BIOS] GetC0Table() -> 0x{:08X}\n", ctx_.r[K1]);
    ctx_.r[V0] = ctx_.r[K1];
    break;
  case 0x57: // GetB0Table — return pointer to B0 jump table
    BIOS_LOG("[BIOS] GetB0Table() -> 0x{:08X}\n", ctx_.r[K0]);
    ctx_.r[V0] = ctx_.r[K0];
    break;
  case 0x42: // B0:42 — internal PsyQ CdInit verification (SetConf/cdioctl-like)
    // Called by CdInit after 2 rounds of _96_CdInitSubFunc+testEvent+GetStat.
    // Return value: 1 = success (CdInit succeeds), 0 = failure (enters 5-retry
    // hardware loop then prints "CdInit: Init failed").
    // Returning 1 here skips the retry loop and lets CdInit succeed.
    BIOS_LOG("[BIOS] B0:42 [STUB] (a0=0x{:08X}, a1=0x{:08X}, a2={})\n",
               ctx_.r[A0], ctx_.r[A1], ctx_.r[A2]);
    ctx_.r[V0] = 1; // success — skip retry loop
    break;
  case 0x5B: // ChangeClearPad
    BIOS_LOG("[BIOS] ChangeClearPad({}) [STUB]\n", ctx_.r[A0]);
    break;
  default:
    BIOS_LOG("[BIOS] Unimplemented B0 call: 0x{:02X} (A0: {:08X}, A1: "
               "{:08X}, A2: {:08X})\n",
               index, ctx_.r[A0], ctx_.r[A1], ctx_.r[A2]);
    break;
  }
}

// ─── Table C (0xC0) ──────────────────────────────────────

void Bios::handleC0(uint32_t index) {
  switch (index) {
  case 0x00: // InstallExceptionHandler — stub
    BIOS_LOG("[BIOS] InstallExceptionHandler() [STUB]\n");
    break;
  case 0x01: // SysEnqIntRP
    // Registers a handler in the interrupt priority chain.
    // On a real PS1 this would add to the exception dispatch chain at
    // the given priority level.  In our HLE, we deliver events directly
    // from the main thread, so the handler chain isn't needed.
    // We still acknowledge the call properly (return 0 = success).
    BIOS_LOG("[BIOS] SysEnqIntRP(priority: {}, handler: 0x{:08X})\n",
               ctx_.r[A0], ctx_.r[A1]);
    ctx_.r[V0] = 0;
    break;
  case 0x02: // SysDeqIntRP
    // Removes a handler from the interrupt priority chain.
    BIOS_LOG("[BIOS] SysDeqIntRP(priority: {}, handler: 0x{:08X})\n",
               ctx_.r[A0], ctx_.r[A1]);
    ctx_.r[V0] = 0;
    break;
  case 0x03: // SysInitMemory
    BIOS_LOG("[BIOS] SysInitMemory(addr: 0x{:08X}, size: {}) [STUB]\n",
               ctx_.r[A0], ctx_.r[A1]);
    break;
  case 0x07: // InstallExceptionHandlers
    BIOS_LOG("[BIOS] InstallExceptionHandlers() [STUB]\n");
    break;
  case 0x08: // SysInitKMem
    BIOS_LOG("[BIOS] SysInitKMem() [STUB]\n");
    break;
  case 0x0A: // ChangeClearRCnt
    // Controls whether root counter interrupts auto-clear after firing.
    // t (A0) = root counter index (0-3), flag (A1) = 0 or 1
    // Returns the old flag value.  We always return 0 (was auto-clear).
    // PsyQ VSync depends on this being called during init to set up RCnt3.
    BIOS_LOG("[BIOS] ChangeClearRCnt(t: {}, flag: {})\n", ctx_.r[A0],
               ctx_.r[A1]);
    ctx_.r[V0] = 0; // return old value
    break;
  case 0x0C: // InitDefInt
    BIOS_LOG("[BIOS] InitDefInt(priority: {}) [STUB]\n", ctx_.r[A0]);
    break;
  default:
    BIOS_LOG("[BIOS] Unimplemented C0 call: 0x{:02X} (A0: {:08X}, A1: "
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
// Mapping (empirically derived from Rayman openEvent calls):
//   Rayman opens class 0xF4000001 with 4 specs: {0x0004, 0x8000, 0x0100, 0x2000}
//   Matching against CDROM INT types:
//   INT1 (DataReady)   → event spec 0x0100  ← Rayman ID2 (sector read complete)
//   INT2 (Complete)    → event spec 0x8000  ← Rayman ID1 (init/cmd done)
//   INT3 (Acknowledge) → event spec 0x0004  ← Rayman ID0 (cmd accepted)
//   INT4 (DataEnd)     → event spec 0x0100  ← same as DataReady
//   INT5 (DiskError)   → event spec 0x2000  ← Rayman ID3 (error)

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
  // kernel polls BSS mirrors right after the port write — without the
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
  // before dispatching — `triggerCdromEvent` may re-enter via cdrom_->tick
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

  // ── HLE PsyQ CDROM interrupt handler variables ────
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
  // coverage landed — dropped entirely.
  //

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

  // ── Update PsyQ CD state in psyq_state() singleton ────────────────────
  //
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
  // observes the new state.  Single byte per slot — same width as the
  // pre-2.3 BIOS handler used to write.
  if (cdSyncMirror_ != 0)
    ctx_.mem->write8(cdSyncMirror_,
                     psyqSync.load(std::memory_order_acquire));
  if (cdReadyMirror_ != 0)
    ctx_.mem->write8(cdReadyMirror_,
                     psyqReady.load(std::memory_order_acquire));
  BIOS_LOG("[CDROM-HLE] INT{} → psyq_state.sync={} ready={}\n", cdIntType,
           psyqSync.load(std::memory_order_relaxed),
           psyqReady.load(std::memory_order_relaxed));

  // ── Dispatch CustomExceptionExit if registered ──
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
  // CD interrupt type, and fire the longjmp from drainPendingCallbacks() —
  // which the recompiler inserts into polling loops.  This gives the game time
  // to set up its polling state before the longjmp fires.
  //
  // Do NOT defer customExceptionExit for CD interrupts.
  //
  // CD commands in our implementation execute synchronously on the game
  // thread (writeRegister → executeCommand → pushResponse → callback).
  // Secondary responses (INT2) also fire synchronously via fireSecondaryNow()
  // when the game acks the primary interrupt.  The game's PsyQ polling code
  // reads the hardware interrupt flag register directly and processes the
  // response through register I/O — it does not need (and is disrupted by)
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

  // ── Defer CustomExceptionExit on VBlank ──────────────────────────────────
  //
  // On a real PS1, B0:0x19 (SetCustomExitFromException) installs a jmpbuf
  // that is triggered by ANY hardware exception, including VBlank.
  // Games like Tomba! use this to implement CD command polling: they
  // setjmp, send a CD command, and wait — the VBlank (or CD INT) fires
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
  uint32_t swapCb = ps1::psyq::psyq_state().gpuSwapCb;
  if (swapCb != 0) {
    eventSystem_.queueCallbackWithArg(swapCb,
                                      4); // a0 = 4 → "swap done"
  }

  // ── HLE PsyQ DrawSync status (GPU ordering table completion) ──
  //
  // Since the runtime GPU is fully synchronous (all GP0 commands process
  // inline), every OT slot is "complete" by the time the next VBlank fires.
  // Stamp psyq_state().drawSync.status[] with 2 (= CdlComplete sentinel
  // used by PsyQ DrawSync polling) so any future consumer reading the
  // singleton sees consistent state.  Native PsyQ DrawSync polling against
  // PS1 RAM is short-circuited by the libgpu HLE (`hle_DrawSync` returns 0
  // unconditionally), so no BSS write is needed anymore — the matcher
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
  // Drain any cross-thread CDROM IRQs first — they may set
  // cdIntPending_ / cdExceptionPending_ / event-system state, which the
  // rest of this method then consumes.  Phase 3.3.
  drainCdromEventQueue();

  eventSystem_.drainPendingCallbacks();

  // ── Deferred CD customExceptionExit dispatch ──────────────────────
  //
  // When a CD interrupt fires synchronously (during a CDROM register write),
  // triggerCdromEvent queues it here instead of longjmping immediately.
  // Now that the game is at a safe yield point (polling loop), we fire the
  // longjmp so the game's exception handler can process the CD result.
  //
  // Check for deferred exceptions (CD or VBlank) — fire at most one per
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
      return; // longjmp fired — game handles it
    }
  }

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

    // Save registers — callbacks clobber temps.
    auto saved = static_cast<ps1::CPUContext>(ctx_);

    if (intType == 1 || intType == 4) {
      // INT1 (DataReady) or INT4 (DataEnd) → call dataCb
      uint32_t dataCb = state.cdDataCb;
      int32_t remaining = static_cast<int32_t>(state.cdRemaining);
      static int hleDispatch = 0;
      hleDispatch++;
      if (s_biosVerbose && hleDispatch <= 20) {
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
      uint32_t notifyCb = state.cdNotifyCb;
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
