#include "runtime/bios/event_system.h"
#include "runtime/cpu_context.h"
#include "runtime/memory.h"
#include <gtest/gtest.h>
#include <utility>
#include <vector>

using namespace ps1::bios;

class EventSystemTest : public ::testing::Test {
protected:
  void SetUp() override { es = std::make_unique<EventSystem>(ctx); }

  recomp_context ctx{};
  std::unique_ptr<EventSystem> es;
};

TEST_F(EventSystemTest, OpenEventReturnsValidId) {
  uint32_t classId = 0x09; // CDROM
  uint32_t specId = 0x20;
  uint32_t mode = 0x2000;
  uint32_t handler = 0;

  uint32_t id1 = es->openEvent(classId, specId, mode, handler);
  uint32_t id2 = es->openEvent(classId, specId, mode, handler);

  EXPECT_EQ(id1, 0);
  EXPECT_EQ(id2, 1);
}

TEST_F(EventSystemTest, WaitEventAcknowledgesTrigger) {
  uint32_t id = es->openEvent(0x09, 0x20, 0x2000, 0);

  // Event is initially disabled, trigger should do nothing
  es->triggerEvent(0x09, 0x20);
  EXPECT_EQ(es->testEvent(id), 0);

  // Enable and trigger
  es->enableEvent(id);
  es->triggerEvent(0x09, 0x20);

  EXPECT_EQ(es->testEvent(id), 1);

  // TestEvent acknowledges, so it should be false now
  EXPECT_EQ(es->testEvent(id), 0);

  // Trigger again
  es->triggerEvent(0x09, 0x20);

  // WaitEvent also acknowledges
  EXPECT_EQ(es->waitEvent(id), 1);
  EXPECT_EQ(es->testEvent(id), 0);
}

TEST_F(EventSystemTest, CloseEventInactivatesEvent) {
  uint32_t id = es->openEvent(0x09, 0x20, 0x2000, 0);
  es->enableEvent(id);

  EXPECT_EQ(es->closeEvent(id), 1);

  es->triggerEvent(0x09, 0x20);

  // Should be inactive and untouched
  EXPECT_EQ(es->testEvent(id), 0);
}

// Contract: psx-spx kernelbios.md "BIOS Event Functions" (B(07h)..B(0Dh)).

TEST_F(EventSystemTest, OpenEventFailsWithFFFFFFFFhOnceTheTableIsFull) {
  // OpenEvent: "out: R2 = Event descriptor ..., or FFFFFFFFh if failed".
  // The table is finite (SYSTEM.CNF "EVENT ="), so opening keeps succeeding
  // only up to its size.
  uint32_t opened = 0;
  uint32_t id = 0;
  while (opened < 1000 &&
         (id = es->openEvent(0xF0000001, opened, 0x2000, 0)) != 0xFFFFFFFFu)
    ++opened;

  EXPECT_EQ(id, 0xFFFFFFFFu);
  EXPECT_GE(opened, 16u) << "fewer than the BIOS default of 16 events";
  EXPECT_EQ(es->openEvent(0xF0000001, 0x999, 0x2000, 0), 0xFFFFFFFFu);
}

TEST_F(EventSystemTest, TestAndWaitOfAnUnknownDescriptorReturnZero) {
  // An unused handle is never ready: TestEvent/WaitEvent report 0.
  es->openEvent(0x09, 0x20, 0x2000, 0);

  const uint32_t tested = es->testEvent(7);
  const uint32_t waited = es->waitEvent(7);

  EXPECT_EQ(tested, 0u);
  EXPECT_EQ(waited, 0u);
}

TEST_F(EventSystemTest, WaitEventOnADisabledEventReturnsZero) {
  // B(0Ah): "Returns 0 if the event is disabled."  A delivery to a disabled
  // event does not make it ready.
  const uint32_t id = es->openEvent(0x09, 0x20, 0x2000, 0);
  es->triggerEvent(0x09, 0x20);

  const uint32_t r = es->waitEvent(id);

  EXPECT_EQ(r, 0u);
}

TEST_F(EventSystemTest, DeliveryMatchesOnlyWhenClassAndSpecBothMatch) {
  // OpenEvent: "class,spec - triggers if BOTH values match".
  const uint32_t id = es->openEvent(0x09, 0x20, 0x2000, 0);
  es->enableEvent(id);

  es->triggerEvent(0x09, 0x40);
  es->triggerEvent(0x0A, 0x20);

  EXPECT_EQ(es->testEvent(id), 0u);
}

TEST_F(EventSystemTest, DisabledEventIgnoresNewDeliveriesUntilReEnabled) {
  const uint32_t id = es->openEvent(0x09, 0x20, 0x2000, 0);
  es->enableEvent(id);
  ASSERT_EQ(es->disableEvent(id), 1u);

  es->triggerEvent(0x09, 0x20);

  EXPECT_EQ(es->testEvent(id), 0u);
}

// Callback events (mode 1000h) run their function from the game thread's
// yield point; test_stubs.cpp routes recomp_dispatch through this hook.
extern void (*g_testRecompDispatchHook)(recomp_context *ctx, uint32_t addr);

namespace {
std::vector<uint32_t> g_evtPcs;
std::vector<std::pair<uint32_t, uint32_t>> g_evtArgs;
void evtRecord(recomp_context *c, uint32_t addr) {
  g_evtPcs.push_back(addr);
  g_evtArgs.emplace_back(c->r[4], c->r[5]);
  c->r[4] = 0xDEADBEEF; // a callback clobbers temporaries
}

class EventCallbackTest : public EventSystemTest {
protected:
  void SetUp() override {
    EventSystemTest::SetUp();
    ctx.mem = &mem;
    ctx.r[29] = 0x801FFF00;
    g_evtPcs.clear();
    g_evtArgs.clear();
    g_testRecompDispatchHook = &evtRecord;
  }
  void TearDown() override { g_testRecompDispatchHook = nullptr; }
  ps1::Memory mem;
};
} // namespace

TEST_F(EventCallbackTest, DeliveryToACallbackEventRunsItsFunction) {
  // B(07h): "Depending on the mode, either the callback function is called
  // (mode=1000h) ...".
  const uint32_t id = es->openEvent(0xF0000009, 0x20, 0x1000, 0x80046000);
  es->enableEvent(id);

  es->triggerEvent(0xF0000009, 0x20);
  ASSERT_TRUE(es->hasPendingCallbacks());
  es->drainPendingCallbacks();

  ASSERT_EQ(g_evtPcs.size(), 1u);
  EXPECT_EQ(g_evtPcs[0], 0x80046000u);
  EXPECT_FALSE(es->hasPendingCallbacks());
}

TEST_F(EventCallbackTest, CallbackEventWithoutAFunctionCallsNothing) {
  // OpenEvent: "func - Address of callback function (0=None)".
  const uint32_t id = es->openEvent(0xF0000009, 0x20, 0x1000, 0);
  es->enableEvent(id);

  es->triggerEvent(0xF0000009, 0x20);
  es->drainPendingCallbacks();

  EXPECT_TRUE(g_evtPcs.empty());
}

TEST_F(EventCallbackTest, QueuedCallbackReceivesBothArgumentsAndRestoresRegisters) {
  ctx.r[4] = 0x11111111;
  ctx.r[5] = 0x22222222;
  es->queueCallbackWithArgs(0x80047000, 3, 9);

  es->drainPendingCallbacks();

  ASSERT_EQ(g_evtArgs.size(), 1u);
  EXPECT_EQ(g_evtArgs[0], std::make_pair(3u, 9u));
  EXPECT_EQ(ctx.r[4], 0x11111111u) << "caller's a0 not restored";
  EXPECT_EQ(ctx.r[5], 0x22222222u) << "caller's a1 not restored";
}

TEST_F(EventCallbackTest, NullCallbackIsSkippedAndTheRestStillRun) {
  es->queueCallback(0);
  es->queueCallback(0x80047100);

  es->drainPendingCallbacks();

  ASSERT_EQ(g_evtPcs.size(), 1u);
  EXPECT_EQ(g_evtPcs[0], 0x80047100u);
}

TEST_F(EventCallbackTest, DispatchStatsCountEachHandlerSeparately) {
  es->queueCallback(0x80047100);
  es->queueCallback(0x80047200);
  es->queueCallback(0x80047200);

  es->drainPendingCallbacks();

  const auto *stats = es->callbackStats();
  EXPECT_EQ(stats[0].pc.load(), 0x80047100u);
  EXPECT_EQ(stats[0].calls.load(), 1u);
  EXPECT_EQ(stats[1].pc.load(), 0x80047200u);
  EXPECT_EQ(stats[1].calls.load(), 2u);
}
