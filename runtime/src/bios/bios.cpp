#include "runtime/bios/bios.h"
#include <fmt/core.h>

namespace ps1::bios {

Bios::Bios(recomp_context &ctx, cdrom::VirtualFs &fs, Memory &mem)
    : ctx_(ctx), heap_(*ctx.mem), fileIo_(fs, mem) {
  fmt::print("[BIOS] Initialized.\n");
}

Bios::~Bios() = default;

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

void Bios::handleA0(uint32_t index) {
  switch (index) {
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
    // Find end of dst
    while (ctx_.mem->read8(dst) != '\0') {
      dst++;
    }
    // Copy src to dst
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
  case 0x33: // malloc
    ctx_.r[V0] = heap_.malloc(ctx_.r[A0]);
    break;
  case 0x34: // free
    heap_.free(ctx_.r[A0]);
    break;
  case 0x39: // InitHeap
    heap_.initHeap(ctx_.r[A0], ctx_.r[A1]);
    break;
  case 0x3F:
    stub_printf();
    break;
  default:
    fmt::print("[BIOS] Unimplemented A0 call: 0x{:02X} (A0: {:08X}, A1: "
               "{:08X}, A2: {:08X})\n",
               index, ctx_.r[A0], ctx_.r[A1], ctx_.r[A2]);
    break;
  }
}

void Bios::handleB0(uint32_t index) {
  switch (index) {
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
  case 0x32: // open
  {
    std::string path;
    uint32_t addr = ctx_.r[A0];
    char c;
    while ((c = ctx_.mem->read8(addr++)) != '\0') {
      path += c;
    }
    ctx_.r[V0] = fileIo_.open(path.c_str(), ctx_.r[A1]);
  } break;
  case 0x33: // lseek
    ctx_.r[V0] = fileIo_.lseek(ctx_.r[A0], ctx_.r[A1], ctx_.r[A2]);
    break;
  case 0x34: // read
    ctx_.r[V0] = fileIo_.read(ctx_.r[A0], ctx_.r[A1], ctx_.r[A2]);
    break;
  case 0x36: // close
    ctx_.r[V0] = fileIo_.close(ctx_.r[A0]);
    break;
  case 0x3D: // std_out_putchar
    fmt::print("{}", static_cast<char>(ctx_.r[A0]));
    break;
  case 0x3E: // std_out_puts
  {
    uint32_t addr = ctx_.r[A0];
    char c;
    while ((c = ctx_.mem->read8(addr++)) != '\0') {
      fmt::print("{}", c);
    }
    fmt::print("\n");
  } break;
  case 0x4A: // InitCARD
    fmt::print("[BIOS] InitCARD(pad_enable: {})\n", ctx_.r[A0]);
    break;
  case 0x4B: // StartCARD
    fmt::print("[BIOS] StartCARD()\n");
    break;
  default:
    fmt::print("[BIOS] Unimplemented B0 call: 0x{:02X} (A0: {:08X}, A1: "
               "{:08X}, A2: {:08X})\n",
               index, ctx_.r[A0], ctx_.r[A1], ctx_.r[A2]);
    break;
  }
}

void Bios::handleC0(uint32_t index) {
  switch (index) {
  default:
    fmt::print("[BIOS] Unimplemented C0 call: 0x{:02X} (A0: {:08X}, A1: "
               "{:08X}, A2: {:08X})\n",
               index, ctx_.r[A0], ctx_.r[A1], ctx_.r[A2]);
    break;
  }
}

void Bios::stub_printf() {
  // Very rudimentary printf stub. In a real emulator, we'd read the format
  // string from RAM at ctx_.r[A0], and loop through arguments. For now we just
  // print where it pointed.
  fmt::print("printf(0x{:08X})\n", ctx_.r[A0]);
}

} // namespace ps1::bios
