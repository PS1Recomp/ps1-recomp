#include <gtest/gtest.h>
#include <runtime/overlay_manager.h>
#include <vector>

using namespace ps1;

// Basic Registration

TEST(OverlayManagerTest, RegisterOverlay) {
  OverlayManager mgr;
  mgr.registerOverlay("battle", 0x80040000, 0x8000, 0);
  mgr.registerOverlay("menu", 0x80040000, 0x4000, 1);

  EXPECT_EQ(mgr.overlayCount(), 2u);
  EXPECT_EQ(mgr.activeCount(), 0u);
}

// Activation

TEST(OverlayManagerTest, ActivateDeactivate) {
  OverlayManager mgr;
  mgr.registerOverlay("battle", 0x80040000, 0x8000, 0);

  EXPECT_FALSE(mgr.isActive("battle"));
  EXPECT_TRUE(mgr.activateOverlay("battle"));
  EXPECT_TRUE(mgr.isActive("battle"));
  EXPECT_EQ(mgr.activeCount(), 1u);

  EXPECT_TRUE(mgr.deactivateOverlay("battle"));
  EXPECT_FALSE(mgr.isActive("battle"));
  EXPECT_EQ(mgr.activeCount(), 0u);
}

TEST(OverlayManagerTest, ActivateNonexistent) {
  OverlayManager mgr;
  EXPECT_FALSE(mgr.activateOverlay("nonexistent"));
}

// Memory Write Notification

TEST(OverlayManagerTest, NotifyActivatesOverlay) {
  OverlayManager mgr;
  std::vector<std::pair<int, bool>> hookCalls;

  mgr.registerOverlay("battle", 0x80040000, 0x8000, 0);

  mgr.setDispatchHook([&](int idx, bool active) {
    hookCalls.push_back({idx, active});
  });

  // Write to the overlay's RAM region
  mgr.notifyMemWrite(0x80040000, 0x8000);

  EXPECT_TRUE(mgr.isActive("battle"));
  ASSERT_EQ(hookCalls.size(), 1u);
  EXPECT_EQ(hookCalls[0].first, 0);
  EXPECT_TRUE(hookCalls[0].second);
}

TEST(OverlayManagerTest, NotifyOutsideRegion) {
  OverlayManager mgr;
  bool called = false;

  mgr.registerOverlay("battle", 0x80040000, 0x8000, 0);
  mgr.setDispatchHook([&](int, bool) { called = true; });

  // Write outside the overlay region
  mgr.notifyMemWrite(0x80010000, 0x1000);

  EXPECT_FALSE(mgr.isActive("battle"));
  EXPECT_FALSE(called);
}

TEST(OverlayManagerTest, ConflictDeactivatesOldOverlay) {
  OverlayManager mgr;
  std::vector<std::pair<int, bool>> hookCalls;

  // Two overlays at the same address (Crash levels, etc.)
  mgr.registerOverlay("level1", 0x80040000, 0x8000, 0);
  mgr.registerOverlay("level2", 0x80040000, 0x8000, 1);

  mgr.setDispatchHook([&](int idx, bool active) {
    hookCalls.push_back({idx, active});
  });

  // Load level1
  mgr.notifyMemWrite(0x80040000, 0x8000);
  EXPECT_TRUE(mgr.isActive("level1"));
  EXPECT_FALSE(mgr.isActive("level2"));

  hookCalls.clear();

  // Load level2 into same region -- should deactivate level1
  mgr.notifyMemWrite(0x80040000, 0x8000);

  EXPECT_FALSE(mgr.isActive("level1"));
  EXPECT_TRUE(mgr.isActive("level2"));

  // Should have: deactivate(level1), activate(level2)
  ASSERT_GE(hookCalls.size(), 2u);

  // Find deactivation of level1
  bool foundDeactivate = false;
  bool foundActivate = false;
  for (const auto &[idx, active] : hookCalls) {
    if (idx == 0 && !active) foundDeactivate = true;
    if (idx == 1 && active) foundActivate = true;
  }
  EXPECT_TRUE(foundDeactivate);
  EXPECT_TRUE(foundActivate);
}

TEST(OverlayManagerTest, PartialOverlapActivates) {
  OverlayManager mgr;

  mgr.registerOverlay("code", 0x80040000, 0x4000, 0);

  // Write that partially overlaps the overlay region
  mgr.notifyMemWrite(0x80042000, 0x4000); // overlaps 0x80042000-0x80046000

  EXPECT_TRUE(mgr.isActive("code"));
}

TEST(OverlayManagerTest, RepeatedWriteDoesntDuplicateActivation) {
  OverlayManager mgr;
  int activateCount = 0;

  mgr.registerOverlay("code", 0x80040000, 0x4000, 0);
  mgr.setDispatchHook([&](int, bool active) {
    if (active) activateCount++;
  });

  // Write twice to same region
  mgr.notifyMemWrite(0x80040000, 0x4000);
  mgr.notifyMemWrite(0x80040000, 0x4000);

  // Should only activate once (already active)
  EXPECT_EQ(activateCount, 1);
}

// Forced activation and the dispatch hook

namespace {
using HookLog = std::vector<std::pair<int, bool>>;
} // namespace

TEST(OverlayManagerTest, ForcedActivationNotifiesTheHookOnce) {
  OverlayManager mgr;
  HookLog calls;
  mgr.registerOverlay("menu", 0x80050000, 0x1000, 7);
  mgr.setDispatchHook([&](int idx, bool on) { calls.push_back({idx, on}); });

  mgr.activateOverlay("menu");
  mgr.activateOverlay("menu"); // already active

  EXPECT_EQ(calls, (HookLog{{7, true}}));
}

TEST(OverlayManagerTest, ForcedDeactivationNotifiesTheHookOnce) {
  OverlayManager mgr;
  HookLog calls;
  mgr.registerOverlay("menu", 0x80050000, 0x1000, 7);
  mgr.activateOverlay("menu");
  mgr.setDispatchHook([&](int idx, bool on) { calls.push_back({idx, on}); });

  mgr.deactivateOverlay("menu");
  mgr.deactivateOverlay("menu"); // already inactive

  EXPECT_EQ(calls, (HookLog{{7, false}}));
}

TEST(OverlayManagerTest, DeactivatingAnUnknownOverlayFails) {
  OverlayManager mgr;
  mgr.registerOverlay("menu", 0x80050000, 0x1000, 0);

  EXPECT_FALSE(mgr.deactivateOverlay("battle"));
}

TEST(OverlayManagerTest, AnUnknownOverlayIsNeverActive) {
  OverlayManager mgr;
  mgr.registerOverlay("menu", 0x80050000, 0x1000, 0);
  mgr.activateOverlay("menu");

  EXPECT_FALSE(mgr.isActive("battle"));
}

TEST(OverlayManagerTest, ActivationWorksWithoutAHook) {
  OverlayManager mgr;
  mgr.registerOverlay("a", 0x80040000, 0x1000, 0);
  mgr.registerOverlay("b", 0x80040000, 0x1000, 1);

  mgr.notifyMemWrite(0x80040000, 4);
  mgr.notifyMemWrite(0x80040000, 4); // swaps in "b", deactivating "a"
  mgr.activateOverlay("a");
  mgr.deactivateOverlay("a");

  EXPECT_TRUE(mgr.isActive("b"));
  EXPECT_FALSE(mgr.isActive("a"));
}

TEST(OverlayManagerTest, ActiveOverlaysElsewhereAreLeftAlone) {
  OverlayManager mgr;
  mgr.registerOverlay("low", 0x80040000, 0x1000, 0);
  mgr.registerOverlay("high", 0x80060000, 0x1000, 1);
  mgr.activateOverlay("high");

  mgr.notifyMemWrite(0x80040000, 4);

  EXPECT_TRUE(mgr.isActive("low"));
  EXPECT_TRUE(mgr.isActive("high")) << "no overlap, no conflict";
}

TEST(OverlayManagerTest, AWriteEndingAtTheOverlayBaseDoesNotTouchIt) {
  OverlayManager mgr;
  mgr.registerOverlay("code", 0x80040000, 0x1000, 0);

  mgr.notifyMemWrite(0x8003F000, 0x1000); // [3F000, 40000) is just below

  EXPECT_FALSE(mgr.isActive("code"));
}

TEST(OverlayManagerTest, AWriteStartingAtTheOverlayEndDoesNotTouchIt) {
  OverlayManager mgr;
  mgr.registerOverlay("code", 0x80040000, 0x1000, 0);

  mgr.notifyMemWrite(0x80041000, 4);

  EXPECT_FALSE(mgr.isActive("code"));
}
