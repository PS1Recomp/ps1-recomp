// Tests for ps1Runtime -- host input bindings and the VRAM dump
// The SDL event loop and the VRAM dump commands in main_host.cpp forward to
// these, so the mappings are checked here without a window.

#include <gtest/gtest.h>
#include <runtime/gpu/vram_dump.h>
#include <runtime/input/host_bindings.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

using namespace ps1::input;

// Keyboard

TEST(HostKeyBindings, EveryKeyIsBoundOnce) {
  std::set<SDL_Keycode> keys;

  for (const auto &b : keyBindings())
    keys.insert(b.key);

  EXPECT_EQ(keys.size(), keyBindings().size());
}

TEST(HostKeyBindings, EveryDigitalPadButtonHasAKey) {
  uint16_t covered = 0;

  for (const auto &b : keyBindings())
    covered |= b.button;

  EXPECT_EQ(covered, 0xFFFF) << "a PS1 pad button has no key";
}

TEST(HostKeyBindings, PressingABoundKeyHoldsItsButton) {
  InputController input;
  HeldButtons held;

  const KeyBinding *b = applyKey(SDLK_RETURN, true, input, held);

  ASSERT_NE(b, nullptr);
  EXPECT_EQ(b->button, BTN_START);
  EXPECT_EQ(held.mask(), BTN_START);
}

TEST(HostKeyBindings, ReleasingTheKeyReleasesTheButton) {
  InputController input;
  HeldButtons held;
  applyKey(SDLK_z, true, input, held);

  applyKey(SDLK_z, false, input, held);

  EXPECT_EQ(held.mask(), 0);
}

TEST(HostKeyBindings, PressReachesTheGamePadAsActiveLow) {
  InputController input;
  HeldButtons held;

  applyKey(SDLK_z, true, input, held);
  input.latchFrame();

  EXPECT_EQ(input.buttonState() & BTN_CROSS, 0) << "PS1 pad bits are 0 when pressed";
}

TEST(HostKeyBindings, UnboundKeyChangesNothing) {
  InputController input;
  HeldButtons held;
  applyKey(SDLK_UP, true, input, held);

  const KeyBinding *b = applyKey(SDLK_F9, true, input, held);

  EXPECT_EQ(b, nullptr);
  EXPECT_EQ(held.mask(), BTN_UP);
}

// GameController buttons

TEST(HostControllerButtons, FaceButtonsMapByPosition) {
  // Xbox A/B/X/Y sit where the PlayStation Cross/Circle/Square/Triangle do.
  EXPECT_EQ(controllerButton(SDL_CONTROLLER_BUTTON_A), BTN_CROSS);
  EXPECT_EQ(controllerButton(SDL_CONTROLLER_BUTTON_B), BTN_CIRCLE);
  EXPECT_EQ(controllerButton(SDL_CONTROLLER_BUTTON_X), BTN_SQUARE);
  EXPECT_EQ(controllerButton(SDL_CONTROLLER_BUTTON_Y), BTN_TRIANGLE);
}

TEST(HostControllerButtons, ShouldersStickClicksAndMenuButtonsMap) {
  EXPECT_EQ(controllerButton(SDL_CONTROLLER_BUTTON_LEFTSHOULDER), BTN_L1);
  EXPECT_EQ(controllerButton(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER), BTN_R1);
  EXPECT_EQ(controllerButton(SDL_CONTROLLER_BUTTON_LEFTSTICK), BTN_L3);
  EXPECT_EQ(controllerButton(SDL_CONTROLLER_BUTTON_RIGHTSTICK), BTN_R3);
  EXPECT_EQ(controllerButton(SDL_CONTROLLER_BUTTON_BACK), BTN_SELECT);
  EXPECT_EQ(controllerButton(SDL_CONTROLLER_BUTTON_START), BTN_START);
}

TEST(HostControllerButtons, DpadMapsToDpad) {
  EXPECT_EQ(controllerButton(SDL_CONTROLLER_BUTTON_DPAD_UP), BTN_UP);
  EXPECT_EQ(controllerButton(SDL_CONTROLLER_BUTTON_DPAD_DOWN), BTN_DOWN);
  EXPECT_EQ(controllerButton(SDL_CONTROLLER_BUTTON_DPAD_LEFT), BTN_LEFT);
  EXPECT_EQ(controllerButton(SDL_CONTROLLER_BUTTON_DPAD_RIGHT), BTN_RIGHT);
}

TEST(HostControllerButtons, ButtonsThePs1PadLacksMapToNothing) {
  EXPECT_EQ(controllerButton(SDL_CONTROLLER_BUTTON_GUIDE), 0);
}

// GameController axes

TEST(HostControllerAxes, LeftStickPastTheDeadZoneHoldsADirection) {
  InputController input;
  HeldButtons held;

  applyControllerAxis(SDL_CONTROLLER_AXIS_LEFTX, -20000, input, held);
  applyControllerAxis(SDL_CONTROLLER_AXIS_LEFTY, 20000, input, held);

  EXPECT_EQ(held.mask(), BTN_LEFT | BTN_DOWN);
}

TEST(HostControllerAxes, LeftStickInsideTheDeadZoneHoldsNothing) {
  InputController input;
  HeldButtons held;

  applyControllerAxis(SDL_CONTROLLER_AXIS_LEFTX, 12000, input, held);
  applyControllerAxis(SDL_CONTROLLER_AXIS_LEFTY, -12000, input, held);

  EXPECT_EQ(held.mask(), 0);
}

TEST(HostControllerAxes, SwingingTheStickAcrossReleasesTheOldDirection) {
  InputController input;
  HeldButtons held;
  applyControllerAxis(SDL_CONTROLLER_AXIS_LEFTY, -20000, input, held);

  applyControllerAxis(SDL_CONTROLLER_AXIS_LEFTY, 20000, input, held);

  EXPECT_EQ(held.mask(), BTN_DOWN);
}

TEST(HostControllerAxes, StickBackToCentreReleasesBothDirections) {
  InputController input;
  HeldButtons held;
  applyControllerAxis(SDL_CONTROLLER_AXIS_LEFTX, 20000, input, held);

  applyControllerAxis(SDL_CONTROLLER_AXIS_LEFTX, 0, input, held);

  EXPECT_EQ(held.mask(), 0);
}

TEST(HostControllerAxes, TriggersPastHalfTravelActAsL2AndR2) {
  InputController input;
  HeldButtons held;

  applyControllerAxis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, 9000, input, held);
  applyControllerAxis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 30000, input, held);

  EXPECT_EQ(held.mask(), BTN_L2 | BTN_R2);
}

TEST(HostControllerAxes, LightlyPressedTriggerReleasesL2) {
  InputController input;
  HeldButtons held;
  applyControllerAxis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, 30000, input, held);

  applyControllerAxis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, 8000, input, held);

  EXPECT_EQ(held.mask(), 0);
}

TEST(HostControllerAxes, RightStickIsIgnored) {
  InputController input;
  HeldButtons held;

  applyControllerAxis(SDL_CONTROLLER_AXIS_RIGHTX, -30000, input, held);

  EXPECT_EQ(held.mask(), 0);
}

// VRAM dump

namespace {

class VramDumpTest : public ::testing::Test {
protected:
  std::string path;

  void SetUp() override {
    auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
    path = (std::filesystem::temp_directory_path() /
            ("ps1_vramdump_" + std::to_string(::getpid()) + "_" + info->name() + ".ppm"))
               .string();
  }
  void TearDown() override { std::remove(path.c_str()); }

  std::vector<uint8_t> readFile() const {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
  }
};

constexpr size_t kHeaderSize = sizeof("P6\n1024 512\n255\n") - 1;

} // namespace

TEST_F(VramDumpTest, WritesAFullSizeBinaryPpm) {
  ps1::gpu::GPU gpu;

  bool ok = ps1::gpu::writeVramPpm(gpu, path.c_str());

  ASSERT_TRUE(ok);
  auto bytes = readFile();
  ASSERT_EQ(bytes.size(), kHeaderSize + 1024u * 512u * 3u);
  EXPECT_EQ(std::string(bytes.begin(), bytes.begin() + kHeaderSize), "P6\n1024 512\n255\n");
}

TEST_F(VramDumpTest, ScalesFiveBitChannelsToEightBits) {
  // ABGR1555: red in bits 0-4, green 5-9, blue 10-14 (psx-spx GPU VRAM).
  std::vector<uint8_t> vram(1024 * 512 * 2, 0);
  const uint16_t pixel = 0x1F | (0x10 << 5) | (0x01 << 10);
  vram[0] = pixel & 0xFF;
  vram[1] = pixel >> 8;
  ps1::gpu::GPU gpu;
  gpu.loadVram(vram.data());

  ps1::gpu::writeVramPpm(gpu, path.c_str());

  auto bytes = readFile();
  ASSERT_GE(bytes.size(), kHeaderSize + 6);
  EXPECT_EQ(bytes[kHeaderSize + 0], 0xF8); // red 31 << 3
  EXPECT_EQ(bytes[kHeaderSize + 1], 0x80); // green 16 << 3
  EXPECT_EQ(bytes[kHeaderSize + 2], 0x08); // blue 1 << 3
  EXPECT_EQ(bytes[kHeaderSize + 3], 0x00) << "the next pixel is untouched";
}

TEST_F(VramDumpTest, UnwritablePathFailsWithoutCreatingAFile) {
  ps1::gpu::GPU gpu;
  const std::string bad = path + ".missing_dir/vram.ppm";

  bool ok = ps1::gpu::writeVramPpm(gpu, bad.c_str());

  EXPECT_FALSE(ok);
  EXPECT_FALSE(std::filesystem::exists(bad));
}
