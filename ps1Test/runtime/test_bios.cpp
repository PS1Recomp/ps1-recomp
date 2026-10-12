#include <vector>
#include "runtime/bios/bios.h"
#include "runtime/psyq/psyq_state.h"
#include "runtime/cdrom/virtual_fs.h"
#include "runtime/cpu_context.h"
#include "runtime/memory.h"
#include <gtest/gtest.h>
#include <string>

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

TEST_F(BiosTest, DrainPollsDmaCompletionCallbacks) {
  auto &st = ps1::psyq::psyq_state();
  st.reset();
  st.dmaCallback[4] = 0x800466A0;
  st.vsyncCounter.store(1, std::memory_order_release);
  g_dispatched.clear();
  g_testRecompDispatchHook = &recordingHook;

  bios->drainPendingCallbacks();

  g_testRecompDispatchHook = nullptr;
  ASSERT_EQ(g_dispatched.size(), 1u);
  EXPECT_EQ(g_dispatched[0], 0x800466A0u);
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
