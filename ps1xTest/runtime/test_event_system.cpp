#include "runtime/bios/event_system.h"
#include "runtime/cpu_context.h"
#include <gtest/gtest.h>

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
