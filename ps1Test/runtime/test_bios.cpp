#include <vector>
#include "runtime/bios/bios.h"
#include "runtime/cdrom/cdrom_controller.h"
#include "runtime/dma/dma.h"
#include "runtime/psyq/psyq_state.h"
#include "runtime/cdrom/virtual_fs.h"
#include "runtime/cpu_context.h"
#include "runtime/memory.h"
#include <gtest/gtest.h>
#include <unistd.h>
#include <filesystem>
#include <string>
#include <cstdlib>

using namespace ps1::bios;
using namespace ps1::cdrom;
using namespace ps1;

class BiosTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Set up execution context
    ctx.mem = &mem;
    // A yield point only dispatches when the guest has a usable stack, so
    // give the fixture one -- the top of main RAM, where PS1 stacks live.
    ctx.r29 = 0x801FFFF0;

    fs = std::make_unique<VirtualFs>();
    bios = std::make_unique<Bios>(ctx, *fs, mem);
  }

  // Write a string to emulated memory
  void writeString(uint32_t addr, const std::string &str) {
    for (size_t i = 0; i < str.length(); ++i) {
      mem.write8(addr + i, str[i]);
    }
    mem.write8(addr + str.length(), '\0');
  }

  // Read a string from emulated memory
  std::string readString(uint32_t addr) {
    std::string str;
    char c;
    while ((c = mem.read8(addr++)) != '\0') {
      str += c;
    }
    return str;
  }

  Memory mem;
  recomp_context ctx{};
  std::unique_ptr<VirtualFs> fs;
  std::unique_ptr<Bios> bios;
};

TEST_F(BiosTest, StrcmpReturnsCorrectly) {
  writeString(0x80000000, "apple");
  writeString(0x80000100, "apple");
  writeString(0x80000200, "banana");

  // Test equal
  ctx.r[T1] = 0x17; // strcmp A0
  ctx.r[A0] = 0x80000000;
  ctx.r[A1] = 0x80000100;
  bios->executeA0();
  EXPECT_EQ(ctx.r[V0], 0);

  // Test not equal
  ctx.r[T1] = 0x17;
  ctx.r[A0] = 0x80000000;
  ctx.r[A1] = 0x80000200;
  bios->executeA0();
  EXPECT_LT(static_cast<int32_t>(ctx.r[V0]), 0);
}

TEST_F(BiosTest, StrcpyCopiesString) {
  writeString(0x80000000, "hello world");

  ctx.r[T1] = 0x19;       // strcpy A0
  ctx.r[A0] = 0x80000100; // dest
  ctx.r[A1] = 0x80000000; // src
  bios->executeA0();

  EXPECT_EQ(ctx.r[V0], 0x80000100);
  EXPECT_EQ(readString(0x80000100), "hello world");
}

TEST_F(BiosTest, StrlenCountsCorrectly) {
  writeString(0x80000000, "test length");

  ctx.r[T1] = 0x1B; // strlen A0
  ctx.r[A0] = 0x80000000;
  bios->executeA0();

  EXPECT_EQ(ctx.r[V0], 11);
}

TEST_F(BiosTest, MemcpyCopiesBytes) {
  mem.write8(0x80000000, 0xAA);
  mem.write8(0x80000001, 0xBB);
  mem.write8(0x80000002, 0xCC);

  ctx.r[T1] = 0x2A;       // memcpy A0
  ctx.r[A0] = 0x80000100; // dst
  ctx.r[A1] = 0x80000000; // src
  ctx.r[A2] = 3;          // len
  bios->executeA0();

  EXPECT_EQ(ctx.r[V0], 0x80000100);
  EXPECT_EQ(mem.read8(0x80000100), 0xAA);
  EXPECT_EQ(mem.read8(0x80000101), 0xBB);
  EXPECT_EQ(mem.read8(0x80000102), 0xCC);
}

TEST_F(BiosTest, MemsetFillsBytes) {
  ctx.r[T1] = 0x2B;       // memset A0
  ctx.r[A0] = 0x80000000; // dst
  ctx.r[A1] = 0x55;       // val (only lower 8 bits used)
  ctx.r[A2] = 4;          // len
  bios->executeA0();

  EXPECT_EQ(ctx.r[V0], 0x80000000);
  EXPECT_EQ(mem.read8(0x80000000), 0x55);
  EXPECT_EQ(mem.read8(0x80000001), 0x55);
  EXPECT_EQ(mem.read8(0x80000002), 0x55);
  EXPECT_EQ(mem.read8(0x80000003), 0x55);
}

// Re-entrant drain
//
// A dispatched callback is recompiled game code, and the recompiler injects a
// drain at every backward branch -- so a callback with a loop in it calls back
// into drainPendingCallbacks and gets dispatched again. Measured in Crash
// Bandicoot as 561 million nested calls against 153 thousand real yield points,
// with the game thread never returning to its main loop.
//
// Here the dispatch hook stands in for that callback: it bumps the VSync
// counter (so the interrupt tick would re-queue) and re-enters the drain.
// Without the guard this recurses until the stack gives out.
extern void (*g_testRecompDispatchHook)(recomp_context *ctx, uint32_t addr);

namespace {
Bios *g_reentryBios = nullptr;
int g_reentryDispatches = 0;

void reentrantHook(recomp_context * /*ctx*/, uint32_t /*addr*/) {
  if (++g_reentryDispatches > 50)
    return; // runaway guard for the failing case
  ps1::psyq::psyq_state().vsyncCounter.fetch_add(1, std::memory_order_release);
  g_reentryBios->drainPendingCallbacks();
}
} // namespace

TEST_F(BiosTest, DrainPendingCallbacksRefusesToReenter) {
  auto &st = ps1::psyq::psyq_state();
  st.reset();
  st.intrCallback[4] = 0x80046000;
  st.vsyncCounter.store(1, std::memory_order_release);

  g_reentryBios = bios.get();
  g_reentryDispatches = 0;
  g_testRecompDispatchHook = &reentrantHook;

  bios->drainPendingCallbacks();

  g_testRecompDispatchHook = nullptr;
  g_reentryBios = nullptr;

  EXPECT_EQ(g_reentryDispatches, 1)
      << "nested drain dispatched the callback again";
  st.reset();
}

// Which callbacks the per-frame interrupt tick delivers.
//
// libsnd's `SsStart` in VSync tick mode hooks the sequencer onto IRQ line 0
// (VBlank) through `InterruptCallback(0, _SsTrapIntrVSync)`; a tick that only
// walks the timer lines never runs it and the music never plays.  The SPU
// library's transfer-completion poller arrives through `DMACallback(4, fn)`,
// not `InterruptCallback` -- it has to keep running once it lands in the DMA
// table, or the sound effects stop.
namespace {
std::vector<uint32_t> g_dispatched;
void recordingHook(recomp_context * /*ctx*/, uint32_t addr) {
  g_dispatched.push_back(addr);
}
} // namespace

TEST_F(BiosTest, DrainTicksVBlankLineCallbackOncePerFrame) {
  auto &st = ps1::psyq::psyq_state();
  st.reset();
  st.intrCallback[0] = 0x8004B828;
  st.vsyncCounter.store(1, std::memory_order_release);
  g_dispatched.clear();
  g_testRecompDispatchHook = &recordingHook;

  bios->drainPendingCallbacks();
  bios->drainPendingCallbacks(); // same frame: no second tick

  g_testRecompDispatchHook = nullptr;
  ASSERT_EQ(g_dispatched.size(), 1u);
  EXPECT_EQ(g_dispatched[0], 0x8004B828u);
  st.reset();
}

// A DMA completion callback fires only for a channel that finished a transfer:
// CD, MDEC and GPU handlers treat any call as "transfer done".
TEST_F(BiosTest, DrainDeliversDmaCallbackOnlyForCompletedChannel) {
  auto &st = ps1::psyq::psyq_state();
  st.reset();
  ps1::DMA dma;
  dma.setMemory(&mem);
  bios->setDma(&dma);
  st.dmaCallback[4] = 0x800466A0;
  st.dmaCallback[6] = 0x80001000;
  g_dispatched.clear();
  g_testRecompDispatchHook = &recordingHook;

  st.vsyncCounter.store(1, std::memory_order_release);
  bios->drainPendingCallbacks(); // nothing transferred yet
  EXPECT_TRUE(g_dispatched.empty());

  // OTC clear of 4 entries on channel 6 (a DPCR write enables it).
  dma.writeRegister(0x1F8010F0, 0);
  dma.writeRegister(0x1F8010E0, 0x00010000);
  dma.writeRegister(0x1F8010E4, 4);
  dma.writeRegister(0x1F8010E8, 0x11000002);
  st.vsyncCounter.store(2, std::memory_order_release);
  bios->drainPendingCallbacks();
  st.vsyncCounter.store(3, std::memory_order_release);
  bios->drainPendingCallbacks(); // already delivered

  g_testRecompDispatchHook = nullptr;
  bios->setDma(nullptr);
  ASSERT_EQ(g_dispatched.size(), 1u);
  EXPECT_EQ(g_dispatched[0], 0x80001000u);
  st.reset();
}

// Callbacks run on libetc's interrupt stack, as PSY-Q's `startIntr` arranges,
// not on whatever $sp the interrupted code holds.  Crash's level decompressor
// points $sp into the compressed buffer and reads the table just below it; a
// callback frame pushed there corrupted the stream and the loop never ended.
namespace {
uint32_t g_callbackSp = 0;
void spRecordingHook(recomp_context *ctx, uint32_t /*addr*/) {
  g_callbackSp = ctx->r29;
}
} // namespace

TEST_F(BiosTest, CallbacksRunOnTheInterruptStack) {
  auto &st = ps1::psyq::psyq_state();
  st.reset();
  st.intrStackTop = 0x8005494C;
  st.intrCallback[0] = 0x8004B828;
  st.vsyncCounter.store(1, std::memory_order_release);
  g_callbackSp = 0;
  g_testRecompDispatchHook = &spRecordingHook;

  const uint32_t interruptedSp = ctx.r29;
  bios->drainPendingCallbacks();

  g_testRecompDispatchHook = nullptr;
  EXPECT_EQ(g_callbackSp, 0x8005494Cu);
  EXPECT_EQ(ctx.r29, interruptedSp) << "interrupted $sp not restored";
  st.reset();
}

// Hand-written PS1 assembly may save the register file to a context block and
// then use $sp as a general-purpose register -- Crash's model transform parks
// $sp in the scratchpad and packs GTE operands through it.  A yield point
// reached in that window must not dispatch, or the callback's prologue writes
// its frame over a data value.  The work stays queued for the next yield
// point that does have a stack.
TEST_F(BiosTest, DrainDefersWhileGuestHasNoStack) {
  auto &st = ps1::psyq::psyq_state();
  st.reset();
  st.intrCallback[4] = 0x80046000;
  st.vsyncCounter.store(1, std::memory_order_release);

  g_reentryBios = bios.get();
  g_reentryDispatches = 0;
  g_testRecompDispatchHook = &reentrantHook;

  const uint32_t savedSp = ctx.r29;
  ctx.r29 = 0x0905A8E4; // a GTE operand, not an address
  bios->drainPendingCallbacks();
  EXPECT_EQ(g_reentryDispatches, 0) << "dispatched without a guest stack";

  ctx.r29 = 0x1F8000E0; // scratchpad: live working storage, not a stack
  bios->drainPendingCallbacks();
  EXPECT_EQ(g_reentryDispatches, 0) << "dispatched onto the scratchpad";

  // Restoring the stack releases the deferred callback.
  ctx.r29 = savedSp;
  bios->drainPendingCallbacks();
  EXPECT_EQ(g_reentryDispatches, 1) << "deferred callback was never delivered";

  g_testRecompDispatchHook = nullptr;
  g_reentryBios = nullptr;
  ctx.r29 = savedSp;
  st.reset();
}

// Memory card BIOS functions (psx-spx kernelbios.md, "Memory Card"):
// _card_info A(ABh), _card_write B(4Eh), _card_read B(4Fh), _new_card B(50h).
// Completion is reported through the SwCARD (F4000001h) and HwCARD
// (F0000011h) events: 4 = done, 100h = no card, 2000h = card changed,
// 8000h = error.
#include "runtime/input/input.h"

class BiosCardTest : public BiosTest {
protected:
  void SetUp() override {
    BiosTest::SetUp();
    bios->setInputController(&input);
    // The libcard boot sequence: SwCARD then HwCARD, each with the four specs.
    for (uint32_t cls : {0xF4000001u, 0xF0000011u})
      for (uint32_t spec : {0x0004u, 0x8000u, 0x0100u, 0x2000u}) {
        ctx.r[A0] = cls;
        ctx.r[A1] = spec;
        ctx.r[A2] = 0x2000;
        ctx.r[A3] = 0;
        b0(0x08);
        ctx.r[A0] = ctx.r[V0];
        b0(0x0C); // EnableEvent
      }
  }
  void b0(uint32_t fn) { ctx.r[T1] = fn; bios->executeB0(); }
  void a0(uint32_t fn) { ctx.r[T1] = fn; bios->executeA0(); }
  // Event ids follow the open order above: SwCARD 0..3, HwCARD 4..7.
  bool fired(uint32_t id) { ctx.r[A0] = id; b0(0x0B); return ctx.r[V0] == 1; }

  input::InputController input;
};

TEST_F(BiosCardTest, CardInfoReportsAFreshlyInsertedCardAsChanged) {
  ctx.r[A0] = 0x00; // port: slot 1
  a0(0xAB);
  EXPECT_TRUE(fired(3)) << "SwCARD 2000h (card changed)";
  EXPECT_FALSE(fired(0)) << "must not also report done";
  EXPECT_FALSE(fired(1)) << "must not also report an error";
}

TEST_F(BiosCardTest, WriteTestAcknowledgesTheCardChange) {
  // card_write_test: _new_card, then a dummy write to sector 3Fh.
  b0(0x50);
  ctx.r[A0] = 0x00; ctx.r[A1] = 0x3F; ctx.r[A2] = 0x80010000;
  b0(0x4E);
  EXPECT_EQ(ctx.r[V0], 1u);
  EXPECT_TRUE(fired(0)) << "SwCARD done";
  EXPECT_TRUE(fired(4)) << "HwCARD done";

  ctx.r[A0] = 0x00;
  a0(0xAB);
  EXPECT_TRUE(fired(0)) << "card no longer reported as changed";
  EXPECT_FALSE(fired(3));
}

TEST_F(BiosCardTest, WriteWithoutAcknowledgingTheChangeIsRefused) {
  const uint8_t before = input.getMemoryCard(0).image()[5 * 128];
  mem.write8(0x80010000, static_cast<uint8_t>(before ^ 0xFF));
  ctx.r[A0] = 0x00; ctx.r[A1] = 5; ctx.r[A2] = 0x80010000;
  b0(0x4E);
  EXPECT_TRUE(fired(3)) << "SwCARD 2000h (card changed)";
  EXPECT_EQ(input.getMemoryCard(0).image()[5 * 128], before);
}

TEST_F(BiosCardTest, SectorWriteReadsBack) {
  for (uint32_t i = 0; i < 128; ++i)
    mem.write8(0x80010000 + i, static_cast<uint8_t>(i * 3));
  b0(0x50);
  ctx.r[A0] = 0x00; ctx.r[A1] = 5; ctx.r[A2] = 0x80010000;
  b0(0x4E);
  ASSERT_TRUE(fired(0));

  ctx.r[A0] = 0x00; ctx.r[A1] = 5; ctx.r[A2] = 0x80020000;
  b0(0x4F);
  EXPECT_EQ(ctx.r[V0], 1u);
  EXPECT_TRUE(fired(0));
  for (uint32_t i = 0; i < 128; ++i)
    ASSERT_EQ(mem.read8(0x80020000 + i), static_cast<uint8_t>(i * 3)) << i;
}

TEST_F(BiosCardTest, MissingCardTimesOut) {
  input.getMemoryCard(1).setPresent(false);
  ctx.r[A0] = 0x10; // port: slot 2
  a0(0xAB);
  EXPECT_TRUE(fired(2)) << "SwCARD 100h";
}

TEST_F(BiosCardTest, InvalidSectorIsRejected) {
  b0(0x50);
  ctx.r[A0] = 0x00; ctx.r[A1] = 0x401; ctx.r[A2] = 0x80010000;
  b0(0x4E);
  EXPECT_EQ(ctx.r[V0], 0u);
}

TEST_F(BiosCardTest, CdromInterruptsDoNotFireCardEvents) {
  bios->triggerCdromEvent(3);
  bios->triggerCdromEvent(2);
  for (uint32_t id = 0; id < 8; ++id)
    EXPECT_FALSE(fired(id)) << "event " << id;
}

// Memory card files ("bu00:" / "bu10:"): open/read/write/close/erase plus the
// firstfile/nextfile directory search, over the card's own directory (psx-spx
// controllersandmemorycards.md "Memory Card Data Format").
TEST_F(BiosCardTest, FirstfileOnAnEmptyCardFindsNothing) {
  writeString(0x80030000, "bu00:BASCUS-94900*");
  ctx.r[A0] = 0x80030000; ctx.r[A1] = 0x80031000;
  b0(0x42);
  EXPECT_EQ(ctx.r[V0], 0u);
}

TEST_F(BiosCardTest, SaveFileRoundTrips) {
  writeString(0x80030000, "bu00:BASCUS-94900CRASH");
  for (uint32_t i = 0; i < 256; ++i)
    mem.write8(0x80040000 + i, static_cast<uint8_t>(i ^ 0x5A));

  ctx.r[A0] = 0x80030000; ctx.r[A1] = 0x200 | 0x2 | (1u << 16); // create, 1 block
  b0(0x32);
  const int32_t fd = static_cast<int32_t>(ctx.r[V0]);
  ASSERT_GE(fd, 0);
  ctx.r[A0] = fd; ctx.r[A1] = 0x80040000; ctx.r[A2] = 256;
  b0(0x35);
  EXPECT_EQ(ctx.r[V0], 256u);
  ctx.r[A0] = fd;
  b0(0x36);

  // The directory now lists it, with its size.
  writeString(0x80030100, "bu00:BASCUS-94900*");
  ctx.r[A0] = 0x80030100; ctx.r[A1] = 0x80031000;
  b0(0x42);
  ASSERT_EQ(ctx.r[V0], 0x80031000u);
  EXPECT_EQ(readString(0x80031000), "BASCUS-94900CRASH");
  EXPECT_EQ(mem.read32(0x80031018), 0x2000u);
  ctx.r[A0] = 0x80031000;
  b0(0x43);
  EXPECT_EQ(ctx.r[V0], 0u) << "only one file";

  // Directory frame 1 is a valid first-block entry.
  const auto &img = input.getMemoryCard(0).image();
  EXPECT_EQ(img[128], 0x51);
  uint8_t x = 0;
  for (int i = 0; i < 127; ++i) x ^= img[128 + i];
  EXPECT_EQ(img[128 + 127], x) << "directory frame checksum";

  ctx.r[A0] = 0x80030000; ctx.r[A1] = 0x1;
  b0(0x32);
  const int32_t rfd = static_cast<int32_t>(ctx.r[V0]);
  ASSERT_GE(rfd, 0);
  ctx.r[A0] = rfd; ctx.r[A1] = 0x80050000; ctx.r[A2] = 256;
  b0(0x34);
  EXPECT_EQ(ctx.r[V0], 256u);
  for (uint32_t i = 0; i < 256; ++i)
    ASSERT_EQ(mem.read8(0x80050000 + i), static_cast<uint8_t>(i ^ 0x5A)) << i;
}

TEST_F(BiosCardTest, OpeningAMissingFileFails) {
  writeString(0x80030000, "bu00:NOPE");
  ctx.r[A0] = 0x80030000; ctx.r[A1] = 0x1;
  b0(0x32);
  EXPECT_EQ(static_cast<int32_t>(ctx.r[V0]), -1);
}

TEST_F(BiosCardTest, EraseFreesTheFile) {
  writeString(0x80030000, "bu00:BASCUS-94900CRASH");
  ctx.r[A0] = 0x80030000; ctx.r[A1] = 0x200 | (2u << 16);
  b0(0x32);
  ctx.r[A0] = ctx.r[V0];
  b0(0x36);

  ctx.r[A0] = 0x80030000;
  b0(0x45);
  EXPECT_EQ(ctx.r[V0], 1u);
  ctx.r[A0] = 0x80030000; ctx.r[A1] = 0x80031000;
  b0(0x42);
  EXPECT_EQ(ctx.r[V0], 0u);
  // Both blocks free again: a 15-block file fits.
  ctx.r[A0] = 0x80030000; ctx.r[A1] = 0x200 | (15u << 16);
  b0(0x32);
  EXPECT_GE(static_cast<int32_t>(ctx.r[V0]), 0);
}

// Card filenames hold 20 characters.  Crash names its save with 21
// ("BASCUS-9490000214533P"), so the stored name is the first 20 -- and opening
// with the full 21-character name must still find it, as on the BIOS.
TEST_F(BiosCardTest, NameLongerThanTwentyCharactersReopens) {
  writeString(0x80030000, "bu00:BASCUS-9490000214533P");
  ctx.r[A0] = 0x80030000; ctx.r[A1] = 0x200 | 0x2 | (1u << 16);
  b0(0x32);
  ASSERT_GE(static_cast<int32_t>(ctx.r[V0]), 0);
  ctx.r[A0] = ctx.r[V0];
  b0(0x36);

  ctx.r[A0] = 0x80030000; ctx.r[A1] = 0x1;
  b0(0x32);
  EXPECT_GE(static_cast<int32_t>(ctx.r[V0]), 0) << "save not found again";
}

// A corrupted card whose block chain points back on itself must not hang the
// erase: the walk stops after one pass over the directory.
TEST_F(BiosCardTest, EraseStopsOnALoopingBlockChain) {
  writeString(0x80030000, "bu00:BASCUS-94900CRASH");
  ctx.r[A0] = 0x80030000; ctx.r[A1] = 0x200 | (1u << 16);
  b0(0x32);
  ctx.r[A0] = ctx.r[V0];
  b0(0x36);
  auto &card = input.getMemoryCard(0);
  uint8_t frame[input::MemoryCard::SECTOR_SIZE];
  card.readSector(1, frame);
  frame[8] = 0; frame[9] = 0; // block 0 -> block 0
  card.writeSector(1, frame);

  ctx.r[A0] = 0x80030000;
  b0(0x45);
  EXPECT_EQ(ctx.r[V0], 1u);
}

TEST_F(BiosCardTest, LseekFromEndAndInvalidWhence) {
  writeString(0x80030000, "bu00:BASCUS-94900CRASH");
  ctx.r[A0] = 0x80030000; ctx.r[A1] = 0x200 | 0x2 | (1u << 16);
  b0(0x32);
  const uint32_t fd = ctx.r[V0];
  ctx.r[A0] = fd; ctx.r[A1] = static_cast<uint32_t>(-128); ctx.r[A2] = 2;
  b0(0x33);
  EXPECT_EQ(ctx.r[V0], 0x2000u - 128) << "SEEK_END counts from the file size";
  ctx.r[A0] = fd; ctx.r[A1] = 0; ctx.r[A2] = 7;
  b0(0x33);
  EXPECT_EQ(static_cast<int32_t>(ctx.r[V0]), -1);
  ctx.r[A0] = fd; ctx.r[A1] = 0; ctx.r[A2] = 1;
  b0(0x33);
  EXPECT_EQ(ctx.r[V0], 0x2000u - 128) << "bad whence moved the position";
}

// Card handles are 16..31.  Larger values are not aliases of open files.
TEST_F(BiosCardTest, FdBeyondTheCardRangeIsRejected) {
  writeString(0x80030000, "bu00:BASCUS-94900CRASH");
  ctx.r[A0] = 0x80030000; ctx.r[A1] = 0x200 | 0x2 | (1u << 16);
  b0(0x32);
  const uint32_t fd = ctx.r[V0];
  ASSERT_EQ(fd, 16u);
  for (uint32_t fn : {0x33u, 0x34u, 0x35u, 0x36u}) {
    ctx.r[A0] = fd + 16; ctx.r[A1] = 0x80040000; ctx.r[A2] = 16;
    b0(fn);
    EXPECT_EQ(static_cast<int32_t>(ctx.r[V0]), -1) << std::hex << fn;
  }
  ctx.r[A0] = fd; ctx.r[A1] = 0x80040000; ctx.r[A2] = 16;
  b0(0x35);
  EXPECT_EQ(ctx.r[V0], 16u) << "the real handle was closed through an alias";
}

// CD data/notify callbacks are interrupt handlers too: they get the interrupt
// stack, not the $sp of whatever loop was interrupted.
TEST_F(BiosTest, CdCallbacksRunOnTheInterruptStack) {
  auto &st = ps1::psyq::psyq_state();
  st.reset();
  st.intrStackTop = 0x8005494C;
  st.cdDataCb = 0x80047000;
  ps1::cdrom::CdromController cdrom;
  bios->setCdromController(&cdrom);
  g_callbackSp = 0;
  g_testRecompDispatchHook = &spRecordingHook;

  const uint32_t interruptedSp = ctx.r29;
  bios->queueCdromEvent(1); // INT1, data ready
  bios->drainPendingCallbacks();

  g_testRecompDispatchHook = nullptr;
  bios->setCdromController(nullptr);
  EXPECT_EQ(g_callbackSp, 0x8005494Cu);
  EXPECT_EQ(ctx.r29, interruptedSp);
  st.reset();
}

// Writes reach the backing file when the BIOS call returns (the runtime exits
// without destructors), now flushed once per call rather than per sector.
TEST_F(BiosCardTest, FileWriteReachesTheBackingFile) {
  const std::string path =
      (std::filesystem::temp_directory_path() /
       ("ps1_test_BiosCardTest_FileWriteReachesTheBackingFile_" +
        std::to_string(getpid()) + ".mcd")).string();
  std::remove(path.c_str());
  ASSERT_TRUE(input.getMemoryCard(0).attachFile(path));
  writeString(0x80030000, "bu00:BASCUS-94900CRASH");
  for (uint32_t i = 0; i < 256; ++i)
    mem.write8(0x80040000 + i, static_cast<uint8_t>(i));
  ctx.r[A0] = 0x80030000; ctx.r[A1] = 0x200 | 0x2 | (1u << 16);
  b0(0x32);
  ctx.r[A0] = ctx.r[V0]; ctx.r[A1] = 0x80040000; ctx.r[A2] = 256;
  b0(0x35);

  input::MemoryCard onDisk;
  ASSERT_TRUE(onDisk.loadFromFile(path));
  EXPECT_EQ(onDisk.image(), input.getMemoryCard(0).image());
  std::remove(path.c_str());
}

// More memory card behaviour (psx-spx kernelbios.md "Memory Card" functions,
// controllersandmemorycards.md "Memory Card Data Format").

TEST_F(BiosCardTest, CardLoadReadsAFreshlyInsertedCardWithoutAChangeError) {
  // A(ACh) _card_load reads the directory; loading the "MC" id is one of the
  // accesses B(50h) _new_card describes as ignoring the card-changed flag.
  ctx.r[A0] = 0x00;
  a0(0xAC);
  EXPECT_EQ(ctx.r[V0], 1u);
  EXPECT_TRUE(fired(0)) << "SwCARD done";
  EXPECT_FALSE(fired(3)) << "reported as changed";
}

TEST_F(BiosCardTest, CardStatusIsReadyAfterACompletedRead) {
  // B(5Ch) _card_status: "01h=ready".
  b0(0x50);
  ctx.r[A0] = 0x00; ctx.r[A1] = 5; ctx.r[A2] = 0x80020000;
  b0(0x4F);
  ctx.r[A0] = 0;
  b0(0x5C);
  EXPECT_EQ(ctx.r[V0], 0x01u);
}

TEST_F(BiosCardTest, CardStatusReportsATimeoutWhenNoCardIsInserted) {
  // B(5Ch) _card_status: "11h=failed/timeout (eg. when no cartridge
  // inserted)".  Slot = port/10h.
  input.getMemoryCard(1).setPresent(false);
  ctx.r[A0] = 0x10; ctx.r[A1] = 5; ctx.r[A2] = 0x80020000;
  b0(0x4F);
  ctx.r[A0] = 1;
  b0(0x5C);
  EXPECT_EQ(ctx.r[V0], 0x11u);
}

TEST_F(BiosCardTest, Sector400hIsAcceptedLikeTheBios) {
  // B(4Eh)/B(4Fh): "for some strange reason, probably a BUG, the function
  // also accepts sector 400h".
  b0(0x50);
  ctx.r[A0] = 0x00; ctx.r[A1] = 0x400; ctx.r[A2] = 0x80020000;
  b0(0x4F);
  EXPECT_EQ(ctx.r[V0], 1u);
}

TEST_F(BiosCardTest, CardAccessWithoutAControllerReportsNoCard) {
  bios->setInputController(nullptr);
  ctx.r[A0] = 0x00; ctx.r[A1] = 5; ctx.r[A2] = 0x80020000;
  b0(0x4F);
  EXPECT_EQ(ctx.r[V0], 1u);
  EXPECT_TRUE(fired(2)) << "SwCARD 100h (no card)";
}

TEST_F(BiosCardTest, CardFilesAreUnavailableWithoutAController) {
  bios->setInputController(nullptr);
  writeString(0x80030000, "bu00:BASCUS-94900CRASH");
  ctx.r[A0] = 0x80030000; ctx.r[A1] = 0x200 | (1u << 16);
  b0(0x32);
  const int32_t fd = static_cast<int32_t>(ctx.r[V0]);
  ctx.r[A0] = 0x80030000; ctx.r[A1] = 0x80031000;
  b0(0x42);
  const uint32_t first = ctx.r[V0];
  ctx.r[A0] = 0x80031000;
  b0(0x43);

  EXPECT_EQ(fd, -1);
  EXPECT_EQ(first, 0u);
  EXPECT_EQ(ctx.r[V0], 0u);
}

TEST_F(BiosCardTest, CreatingAFileInAnEmptySlotFails) {
  input.getMemoryCard(1).setPresent(false);
  writeString(0x80030000, "bu10:BASCUS-94900CRASH");
  ctx.r[A0] = 0x80030000; ctx.r[A1] = 0x200 | (1u << 16);
  b0(0x32);
  EXPECT_EQ(static_cast<int32_t>(ctx.r[V0]), -1);
}

TEST_F(BiosCardTest, CardPathWithoutADeviceSeparatorFails) {
  writeString(0x80030000, "bu00BASCUS-94900CRASH");
  ctx.r[A0] = 0x80030000; ctx.r[A1] = 0x200 | (1u << 16);
  b0(0x32);
  EXPECT_EQ(static_cast<int32_t>(ctx.r[V0]), -1);
}

class BiosCardFileTest : public BiosCardTest {
protected:
  int32_t create(uint32_t blocks, const std::string &path =
                                      "bu00:BASCUS-94900CRASH") {
    writeString(0x80030000, path);
    ctx.r[A0] = 0x80030000; ctx.r[A1] = 0x200 | 0x2 | (blocks << 16);
    b0(0x32);
    return static_cast<int32_t>(ctx.r[V0]);
  }
  int32_t openExisting(const std::string &path = "bu00:BASCUS-94900CRASH") {
    writeString(0x80030000, path);
    ctx.r[A0] = 0x80030000; ctx.r[A1] = 0x1;
    b0(0x32);
    return static_cast<int32_t>(ctx.r[V0]);
  }
  int32_t call(uint32_t fn, uint32_t a, uint32_t b = 0, uint32_t c = 0) {
    ctx.r[A0] = a; ctx.r[A1] = b; ctx.r[A2] = c;
    b0(fn);
    return static_cast<int32_t>(ctx.r[V0]);
  }
  uint32_t firstfile(const std::string &pattern) {
    writeString(0x80030100, pattern);
    return static_cast<uint32_t>(call(0x42, 0x80030100, 0x80031000));
  }
};

TEST_F(BiosCardFileTest, ClosingACardFileTwiceFails) {
  // B(36h) close: "Returns r2=fd (or r2=-1 if failed)".
  const int32_t fd = create(1);
  ASSERT_GE(fd, 0);
  EXPECT_EQ(call(0x36, fd), fd);

  EXPECT_EQ(call(0x36, fd), -1);
}

TEST_F(BiosCardFileTest, LseekOnAClosedCardFileFails) {
  const int32_t fd = create(1);
  call(0x36, fd);

  EXPECT_EQ(call(0x33, fd, 0, 0), -1);
}

TEST_F(BiosCardFileTest, AtMostSixteenFilesCanBeOpen) {
  // B(32h) open: "The PSX can have a maximum of 16 files open at any time".
  call(0x36, create(1));
  for (int i = 0; i < 16; ++i)
    ASSERT_GE(openExisting(), 0) << i;

  EXPECT_EQ(openExisting(), -1);
}

TEST_F(BiosCardFileTest, LseekBeforeTheStartOfTheFileFailsAndKeepsThePosition) {
  const int32_t fd = create(1);
  call(0x33, fd, 0x10, 0);

  EXPECT_EQ(call(0x33, fd, static_cast<uint32_t>(-0x20), 1), -1);
  EXPECT_EQ(call(0x33, fd, 0, 1), 0x10);
}

TEST_F(BiosCardFileTest, ReadAtTheEndOfTheFileReturnsZero) {
  const int32_t fd = create(1);
  ASSERT_EQ(call(0x33, fd, 0x2000, 0), 0x2000);

  EXPECT_EQ(call(0x34, fd, 0x80040000, 16), 0);
}

TEST_F(BiosCardFileTest, ReadWithANegativeLengthFails) {
  // B(34h) read: "-1 if failed".
  const int32_t fd = create(1);

  EXPECT_EQ(call(0x34, fd, 0x80040000, static_cast<uint32_t>(-1)), -1);
}

TEST_F(BiosCardFileTest, CreatingAFileThatAlreadyExistsFails) {
  call(0x36, create(1));

  EXPECT_EQ(create(1), -1);
}

TEST_F(BiosCardFileTest, CreatingMoreBlocksThanTheCardHoldsFails) {
  // A card holds 15 data blocks (directory frames 1..15).
  EXPECT_EQ(create(16), -1);
}

TEST_F(BiosCardFileTest, MultiBlockFileDataContinuesInTheNextBlock) {
  // A file spans its block chain: offset 2000h is the start of block two.
  const int32_t fd = create(2);
  ASSERT_GE(fd, 0);
  for (uint32_t i = 0; i < 0x80; ++i)
    mem.write8(0x80040000 + i, static_cast<uint8_t>(0xC0 ^ i));
  call(0x33, fd, 0x2000, 0);
  ASSERT_EQ(call(0x35, fd, 0x80040000, 0x80), 0x80);

  call(0x33, fd, 0x2010, 0);
  ASSERT_EQ(call(0x34, fd, 0x80050000, 4), 4);

  EXPECT_EQ(mem.read8(0x80050000), 0xC0 ^ 0x10);
  EXPECT_EQ(mem.read8(0x80050003), 0xC0 ^ 0x13);
}

TEST_F(BiosCardFileTest, FirstfileWithAPatternThatDiffersFindsNothing) {
  call(0x36, create(1));

  EXPECT_EQ(firstfile("bu00:BESLES*"), 0u);
}

TEST_F(BiosCardFileTest, FirstfileQuestionMarkMatchesOneCharacter) {
  // psx-spx firstfile: wildcards "?" (any character) and "*" (rest).
  call(0x36, create(1));

  EXPECT_EQ(firstfile("bu00:BASCUS-94900CRAS?"), 0x80031000u);
  EXPECT_EQ(readString(0x80031000), "BASCUS-94900CRASH");
}

TEST_F(BiosCardFileTest, NextfileKeepsReturningZeroOnceTheSearchEnded) {
  call(0x36, create(1));
  ASSERT_NE(firstfile("bu00:*"), 0u);
  ASSERT_EQ(call(0x43, 0x80031000), 0);

  EXPECT_EQ(call(0x43, 0x80031000), 0);
}

TEST_F(BiosCardFileTest, SlotTwoHasItsOwnDirectory) {
  // "bu10:" is the card in slot 2 (port 10h).
  call(0x36, create(1, "bu10:BASCUS-94900CRASH"));

  EXPECT_EQ(firstfile("bu00:*"), 0u);
  EXPECT_EQ(firstfile("bu10:*"), 0x80031000u);
}

// Host VBlank pump driven from the game thread's yield points.

TEST_F(BiosTest, YieldPointsPumpTheVBlankClockPeriodically) {
  int pumps = 0;
  bool sawWaiting = false;
  bios->setVBlankPump([&](bool waiting) {
    ++pumps;
    sawWaiting |= waiting;
  });

  for (int i = 0; i < 2048; ++i)
    bios->drainPendingCallbacks();

  EXPECT_EQ(pumps, 2);
  EXPECT_FALSE(sawWaiting) << "a yield point is not an idle VSync wait";
}

TEST_F(BiosTest, PumpOnlyAdvancesTheClockButDispatchesNothing) {
  auto &st = ps1::psyq::psyq_state();
  st.reset();
  st.intrCallback[0] = 0x8004B828;
  st.vsyncCounter.store(1, std::memory_order_release);
  g_dispatched.clear();
  g_testRecompDispatchHook = &recordingHook;
  int pumps = 0;
  bios->setVBlankPump([&](bool) { ++pumps; });

  for (int i = 0; i < 1024; ++i)
    bios->pumpOnly();

  g_testRecompDispatchHook = nullptr;
  EXPECT_EQ(pumps, 1);
  EXPECT_TRUE(g_dispatched.empty());
  st.reset();
}

// PS1_SP_GUARD=1: a guest $sp outside RAM and the scratchpad is reported at
// the next yield point, at most eight times.
namespace {
struct ScopedEnv {
  explicit ScopedEnv(const char *name) : name_(name) { setenv(name, "1", 1); }
  ~ScopedEnv() { unsetenv(name_); }
  const char *name_;
};
} // namespace

TEST_F(BiosTest, StackGuardReportsABadStackPointerAtMostEightTimes) {
  std::unique_ptr<Bios> guarded;
  {
    ScopedEnv env("PS1_SP_GUARD");
    guarded = std::make_unique<Bios>(ctx, *fs, mem);
  }
  ctx.r29 = 0x0905A8E4; // a data value, not an address

  testing::internal::CaptureStderr();
  for (int i = 0; i < 12; ++i)
    guarded->drainPendingCallbacks();
  const std::string err = testing::internal::GetCapturedStderr();

  std::size_t reports = 0;
  for (std::size_t p = 0; (p = err.find("guest $sp=0x0905A8E4", p)) !=
                          std::string::npos; ++p)
    ++reports;
  EXPECT_EQ(reports, 8u);
}

TEST_F(BiosTest, StackGuardAlsoWatchesPumpOnlyYieldPoints) {
  std::unique_ptr<Bios> guarded;
  {
    ScopedEnv env("PS1_SP_GUARD");
    guarded = std::make_unique<Bios>(ctx, *fs, mem);
  }
  ctx.r29 = 0x0905A8E4;

  testing::internal::CaptureStderr();
  guarded->pumpOnly();
  const std::string err = testing::internal::GetCapturedStderr();

  EXPECT_NE(err.find("guest $sp=0x0905A8E4"), std::string::npos) << err;
}

TEST_F(BiosTest, StackGuardAcceptsRamAndScratchpadStacks) {
  std::unique_ptr<Bios> guarded;
  {
    ScopedEnv env("PS1_SP_GUARD");
    guarded = std::make_unique<Bios>(ctx, *fs, mem);
  }

  testing::internal::CaptureStderr();
  ctx.r29 = 0x801FFFF0;
  guarded->drainPendingCallbacks();
  ctx.r29 = 0x1F8003F0;
  guarded->drainPendingCallbacks();
  const std::string err = testing::internal::GetCapturedStderr();

  EXPECT_EQ(err.find("[SP-GUARD]"), std::string::npos) << err;
}
