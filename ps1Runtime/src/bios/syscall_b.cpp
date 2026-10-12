#include "bios_internal.h"
#include "runtime/bios/bios.h"
#include "runtime/bios/card_fs.h"
#include "runtime/input/input.h"
#include <fmt/format.h>

namespace ps1::bios {

using detail::readString;

// PS1 BIOS Table B (0xB0) -- event system, pad init, file I/O, memory card,
// table accessors, exception bookkeeping. Dispatched from Bios::executeB0()
// in bios.cpp.

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
  case 0x12: { // InitPAD -- store pad buffer addresses for VBlank polling
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
  case 0x13: // StartPAD -- begin controller polling during VBlank
    padActive_ = true;
    BIOS_LOG("[BIOS] StartPAD() -- polling active\n");
    break;
  case 0x14: // StopPAD -- stop controller polling
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
    ctx_.r[V0] = path.rfind("bu", 0) == 0
                     ? cardOpen(path, ctx_.r[A1])
                     : fileIo_.open(path.c_str(), ctx_.r[A1]);
  } break;
  case 0x33: // lseek
    if (static_cast<int32_t>(ctx_.r[A0]) >= kCardFdBase) {
      CardFile &f = cardFiles_[(ctx_.r[A0] - kCardFdBase) & 15];
      f.pos = (ctx_.r[A2] == 1 ? f.pos : 0) + ctx_.r[A1];
      ctx_.r[V0] = f.pos;
    } else {
      ctx_.r[V0] = fileIo_.lseek(ctx_.r[A0], ctx_.r[A1], ctx_.r[A2]);
    }
    break;
  case 0x34: // read
    ctx_.r[V0] = static_cast<int32_t>(ctx_.r[A0]) >= kCardFdBase
                     ? cardTransfer(ctx_.r[A0], ctx_.r[A1], ctx_.r[A2], false)
                     : fileIo_.read(ctx_.r[A0], ctx_.r[A1], ctx_.r[A2]);
    break;
  case 0x35: // write -- memory card files; anything else goes to stdout
    if (static_cast<int32_t>(ctx_.r[A0]) >= kCardFdBase) {
      ctx_.r[V0] = cardTransfer(ctx_.r[A0], ctx_.r[A1], ctx_.r[A2], true);
      break;
    }
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
    if (static_cast<int32_t>(ctx_.r[A0]) >= kCardFdBase) {
      cardFiles_[(ctx_.r[A0] - kCardFdBase) & 15].open = false;
      ctx_.r[V0] = ctx_.r[A0];
    } else {
      ctx_.r[V0] = fileIo_.close(ctx_.r[A0]);
    }
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
  case 0x4E: // _card_write(port, sector, src)
    ctx_.r[V0] = cardRequest(ctx_.r[A0], CardOp::Write, ctx_.r[A1], ctx_.r[A2]);
    break;
  case 0x4F: // _card_read(port, sector, dst)
    ctx_.r[V0] = cardRequest(ctx_.r[A0], CardOp::Read, ctx_.r[A1], ctx_.r[A2]);
    break;
  case 0x50: // _new_card -- ignore the card-changed flag on the next access
    cardChangeAcked_ = true;
    break;
  case 0x5C: // _card_status(slot)
  case 0x5D: // _card_wait(slot) -- I/O is never left busy here
    ctx_.r[V0] = cardStatus_[ctx_.r[A0] & 1];
    break;
  case 0x56: // GetC0Table -- return pointer to C0 jump table
    BIOS_LOG("[BIOS] GetC0Table() -> 0x{:08X}\n", ctx_.r[K1]);
    ctx_.r[V0] = ctx_.r[K1];
    break;
  case 0x57: // GetB0Table -- return pointer to B0 jump table
    BIOS_LOG("[BIOS] GetB0Table() -> 0x{:08X}\n", ctx_.r[K0]);
    ctx_.r[V0] = ctx_.r[K0];
    break;
  // B(42h) is firstfile2: libcard lists the card with it.  Only the memory
  // card device is searchable here.
  case 0x42: { // firstfile2(filename, direntry)
    std::string name;
    std::string path = readString(*ctx_.mem, ctx_.r[A0]);
    ctx_.r[V0] = 0;
    if (cardForPath(path, name)) {
      cardSearch_ = name;
      cardSearchSlot_ = path.size() > 2 && path[2] == '1';
      cardSearchNext_ = 0;
      ctx_.r[V0] = cardFindNext(ctx_.r[A1]);
    }
  } break;
  case 0x43: // nextfile(direntry)
    ctx_.r[V0] = cardFindNext(ctx_.r[A0]);
    break;
  case 0x45: { // erase(filename)
    std::string name;
    std::string path = readString(*ctx_.mem, ctx_.r[A0]);
    input::MemoryCard *card = cardForPath(path, name);
    const int block = card ? cardfs::find(*card, name) : -1;
    if (block >= 0)
      cardfs::erase(*card, block);
    ctx_.r[V0] = block >= 0;
  } break;
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

// One memory card access, finished on the spot (psx-spx kernelbios.md,
// "Memory Card" and "BIOS Event Summary").  Outcome is reported on both the
// higher-level SwCARD and the lower-level HwCARD events: 4 done, 100h no card,
// 2000h card changed since insertion.  Returns 0 only for a bad sector.
uint32_t Bios::cardRequest(uint32_t port, CardOp op, uint32_t sector,
                           uint32_t addr) {
  if (op != CardOp::Info && sector > 0x400) // sector 400h is accepted (BIOS bug)
    return 0;
  const uint32_t slot = (port >> 4) & 1;
  input::MemoryCard *card =
      input_ ? &input_->getMemoryCard(static_cast<int>(slot)) : nullptr;

  uint32_t spec = 0x0004;
  if (card == nullptr || !card->isPresent()) {
    spec = 0x0100;
    cardStatus_[slot] = 0x11;
  } else if (card->changed() && !cardChangeAcked_) {
    spec = 0x2000;
    cardStatus_[slot] = 0x21;
  } else if (sector < input::MemoryCard::NUM_SECTORS) {
    uint8_t buf[input::MemoryCard::SECTOR_SIZE];
    if (op == CardOp::Write) {
      for (uint32_t i = 0; i < sizeof(buf); ++i)
        buf[i] = ctx_.mem->read8(addr + i);
      card->writeSector(sector, buf);
    } else if (op == CardOp::Read) {
      card->readSector(sector, buf);
      for (uint32_t i = 0; i < sizeof(buf); ++i)
        ctx_.mem->write8(addr + i, buf[i]);
    }
    cardStatus_[slot] = 1;
  }
  if (op != CardOp::Info)
    cardChangeAcked_ = false;
  BIOS_LOG("[BIOS] card port=0x{:02X} op={} sector=0x{:X} -> event 0x{:X}\n",
           port, static_cast<int>(op), sector, spec);
  eventSystem_.triggerEvent(0xF4000001, spec);
  eventSystem_.triggerEvent(0xF0000011, spec);
  return 1;
}

input::MemoryCard *Bios::cardForPath(const std::string &path,
                                     std::string &name) {
  const std::size_t colon = path.find(':');
  if (input_ == nullptr || path.rfind("bu", 0) != 0 || colon == std::string::npos)
    return nullptr;
  input::MemoryCard &card = input_->getMemoryCard(path.size() > 2 && path[2] == '1');
  if (!card.isPresent())
    return nullptr;
  name = path.substr(colon + 1);
  return &card;
}

// open(): bit9 creates the file with bit16-31 blocks (psx-spx kernelbios.md).
int32_t Bios::cardOpen(const std::string &path, uint32_t mode) {
  std::string name;
  input::MemoryCard *card = cardForPath(path, name);
  int block = -1;
  if (card)
    block = (mode & 0x200) ? cardfs::create(*card, name, static_cast<int>(mode >> 16))
                           : cardfs::find(*card, name);
  BIOS_LOG("[BIOS] card open('{}', 0x{:X}) -> block {}\n", path, mode, block);
  if (block < 0)
    return -1;
  for (int32_t i = 0; i < 16; ++i)
    if (!cardFiles_[i].open) {
      cardFiles_[i] = {true, path.size() > 2 && path[2] == '1', block, 0,
                       cardfs::size(*card, block)};
      return kCardFdBase + i;
    }
  return -1;
}

int32_t Bios::cardTransfer(int32_t fd, uint32_t addr, int32_t len, bool write) {
  CardFile &f = cardFiles_[(fd - kCardFdBase) & 15];
  if (!f.open || input_ == nullptr || len < 0)
    return -1;
  input::MemoryCard &card = input_->getMemoryCard(f.slot);
  constexpr uint32_t kSector = input::MemoryCard::SECTOR_SIZE;
  uint8_t buf[kSector];
  int32_t done = 0;
  while (done < len && f.pos < f.size) {
    const int sector = cardfs::sectorAt(card, f.block, f.pos);
    if (sector < 0)
      break;
    const uint32_t at = f.pos % kSector;
    const uint32_t n = std::min<uint32_t>(kSector - at, static_cast<uint32_t>(len - done));
    card.readSector(static_cast<uint32_t>(sector), buf);
    for (uint32_t i = 0; i < n; ++i) {
      if (write)
        buf[at + i] = ctx_.mem->read8(addr + done + i);
      else
        ctx_.mem->write8(addr + done + i, buf[at + i]);
    }
    if (write)
      card.writeSector(static_cast<uint32_t>(sector), buf);
    f.pos += n;
    done += static_cast<int32_t>(n);
  }
  return done;
}

// Fills the BIOS direntry (name, attr 50h, size, next, first sector) for the
// next match of the firstfile2 search; 0 when there is none.
uint32_t Bios::cardFindNext(uint32_t direntry) {
  if (input_ == nullptr || cardSearchNext_ >= cardfs::kBlocks)
    return 0;
  const input::MemoryCard &card = input_->getMemoryCard(cardSearchSlot_);
  const int block = cardfs::find(card, cardSearch_, cardSearchNext_);
  if (block < 0) {
    cardSearchNext_ = cardfs::kBlocks;
    return 0;
  }
  cardSearchNext_ = block + 1;
  const std::string name = cardfs::name(card, block);
  for (uint32_t i = 0; i < 0x14; ++i)
    ctx_.mem->write8(direntry + i, i < name.size() ? name[i] : 0);
  ctx_.mem->write32(direntry + 0x14, 0x50);
  ctx_.mem->write32(direntry + 0x18, cardfs::size(card, block));
  ctx_.mem->write32(direntry + 0x1C, 0);
  ctx_.mem->write32(direntry + 0x20, (block + 1) * (cardfs::kBlockBytes / 128));
  ctx_.mem->write32(direntry + 0x24, 0);
  return direntry;
}

} // namespace ps1::bios
