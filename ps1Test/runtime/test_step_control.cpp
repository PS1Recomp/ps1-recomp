// Tests: lockstep control (PS1_STEP), the turn-based interface for scripted
// players.
#include "runtime/step_control.h"

#include <gtest/gtest.h>

#include <map>
#include <sstream>
#include <string>
#include <vector>

using ps1::StepControl;

namespace {

struct FakeHost {
  std::map<uint32_t, uint32_t> ram;
  std::vector<uint16_t> buttons;
  std::vector<std::string> shots;
  uint16_t hostPad = 0;

  StepControl::Hooks hooks() {
    return {[this](uint32_t a) { return ram[a]; },
            [this](uint32_t a, uint32_t v) { ram[a] = v; },
            [this](const std::string &p) {
              shots.push_back(p);
              return true;
            },
            [this](uint16_t m) { buttons.push_back(m); },
            [](uint32_t addr, uint32_t len) { return uint64_t{addr} * 1000 + len; },
            [this] { return hostPad; }};
  }
};

// Runs VBlanks until the controller blocks for input it does not have (EOF)
// and returns how many it let through.
int runUntilEof(StepControl &step, uint32_t firstVsync = 0) {
  int ticks = 0;
  for (uint32_t v = firstVsync; step.beforeVBlank(v); ++v)
    ++ticks;
  return ticks;
}

} // namespace

TEST(StepControl, ReportsTheFrameAndWaitsBeforeTheFirstVBlank) {
  std::istringstream in("");
  std::ostringstream out;
  FakeHost host;
  StepControl step(in, out, host.hooks());

  EXPECT_FALSE(step.beforeVBlank(7)); // no command: the session is over
  EXPECT_EQ(out.str(), "frame 7\n");
}

TEST(StepControl, StepLetsExactlyNVBlanksThroughThenReportsAgain) {
  std::istringstream in("step 3 0\n");
  std::ostringstream out;
  FakeHost host;
  StepControl step(in, out, host.hooks());

  EXPECT_EQ(runUntilEof(step, 10), 3);
  EXPECT_EQ(out.str(), "frame 10\nframe 13\n");
}

TEST(StepControl, StepHoldsTheGivenButtonsForItsFrames) {
  std::istringstream in("step 2 4040\nstep 1 0\n");
  std::ostringstream out;
  FakeHost host;
  StepControl step(in, out, host.hooks());

  runUntilEof(step);
  // The last 0 is the release when the session ends.
  EXPECT_EQ(host.buttons, (std::vector<uint16_t>{0x4040, 0x0000, 0x0000}));
}

TEST(StepControl, PeekAndPokeAnswerWithoutAdvancingTime) {
  std::istringstream in("poke 80056714 c\npeek 80056714\nstep 1 0\n");
  std::ostringstream out;
  FakeHost host;
  StepControl step(in, out, host.hooks());

  EXPECT_EQ(runUntilEof(step), 1);
  EXPECT_EQ(host.ram[0x80056714], 0xCu);
  EXPECT_EQ(out.str(), "frame 0\nok\nc\nframe 1\n");
}

TEST(StepControl, ShotWritesThroughTheHost) {
  std::istringstream in("shot /tmp/f.ppm\n");
  std::ostringstream out;
  FakeHost host;
  StepControl step(in, out, host.hooks());

  runUntilEof(step);
  EXPECT_EQ(host.shots, (std::vector<std::string>{"/tmp/f.ppm"}));
  EXPECT_EQ(out.str(), "frame 0\nok\n");
}

TEST(StepControl, QuitEndsTheSessionAndReleasesTheButtons) {
  std::istringstream in("step 1 8\nquit\nstep 5 0\n");
  std::ostringstream out;
  FakeHost host;
  StepControl step(in, out, host.hooks());

  EXPECT_EQ(runUntilEof(step), 1);
  EXPECT_EQ(host.buttons.back(), 0u);
}

TEST(StepControl, MalformedCommandIsReportedAndIgnored) {
  std::istringstream in("jump\nstep x 0\nstep 1 0\n");
  std::ostringstream out;
  FakeHost host;
  StepControl step(in, out, host.hooks());

  EXPECT_EQ(runUntilEof(step), 1);
  EXPECT_EQ(out.str(), "frame 0\nerr jump\nerr step x 0\nframe 1\n");
}

// Real hardware ticks VBlank by CPU time.  Without a clock, a VBlank happens
// when the game waits for one (VSync), or after `spinPolls` yield-point polls
// of a loop that never calls VSync (a loader waiting on the CD-ROM).  A
// yield-point poll in the middle of a frame's work must not cost a VBlank.
TEST(StepControl, PollsFromBusyCodeCountTowardOneVBlank) {
  std::istringstream in("step 2 0\n");
  std::ostringstream out;
  FakeHost host;
  StepControl step(in, out, host.hooks(), /*spinPolls=*/3);

  EXPECT_FALSE(step.poll(0, false)); // busy, not yet a VBlank's worth
  EXPECT_FALSE(step.poll(0, false));
  EXPECT_TRUE(step.poll(0, false));  // third poll: report, read step 2, tick 1
  EXPECT_TRUE(step.poll(1, true));   // VSync wait ticks at once: 2 of 2
  EXPECT_FALSE(step.ended());
  EXPECT_EQ(out.str(), "frame 0\n");
}

// `hash` fingerprints guest RAM so two runs can be compared frame by frame.
TEST(StepControl, HashAnswersWithTheHostFingerprint) {
  std::istringstream in("hash 80010000 200000\n");
  std::ostringstream out;
  FakeHost host;
  StepControl step(in, out, host.hooks());

  runUntilEof(step);
  EXPECT_EQ(out.str(), "frame 0\n" + [] {
    std::ostringstream h;
    h << std::hex << (0x80010000ull * 1000 + 0x200000) << '\n';
    return h.str();
  }());
}

// `hostpad` reports what the player is holding on the host, so a client can
// play in lockstep (and record the exact per-frame input) at normal speed.
TEST(StepControl, HostpadReportsTheHostButtons) {
  std::istringstream in("hostpad\n");
  std::ostringstream out;
  FakeHost host;
  host.hostPad = 0x4010;
  StepControl step(in, out, host.hooks());

  runUntilEof(step);
  EXPECT_EQ(out.str(), "frame 0\n4010\n");
}
