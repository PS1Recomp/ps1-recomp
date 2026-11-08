#include "runtime/timers/timers.h"
#include <gtest/gtest.h>

using namespace ps1;

// Interrupt Controller Tests

TEST(InterruptController, InitialState) {
  InterruptController ic;
  EXPECT_EQ(ic.readIStat(), 0u);
  EXPECT_EQ(ic.readIMask(), 0u);
  EXPECT_FALSE(ic.hasPendingInterrupt());
}

TEST(InterruptController, RaiseAndMask) {
  InterruptController ic;
  ic.writeIMask(IRQ_VBLANK);
  ic.raiseInterrupt(IRQ_VBLANK);
  EXPECT_TRUE(ic.hasPendingInterrupt());
  EXPECT_TRUE(ic.shouldSignalCPU());
}

TEST(InterruptController, MaskedInterruptNotPending) {
  InterruptController ic;
  ic.writeIMask(0); // Nothing enabled
  ic.raiseInterrupt(IRQ_VBLANK);
  EXPECT_FALSE(ic.hasPendingInterrupt());
}

TEST(InterruptController, AcknowledgeInterrupt) {
  InterruptController ic;
  ic.writeIMask(IRQ_VBLANK | IRQ_CDROM);
  ic.raiseInterrupt(IRQ_VBLANK);
  ic.raiseInterrupt(IRQ_CDROM);
  EXPECT_TRUE(ic.hasPendingInterrupt());

  // Acknowledge VBlank only (write 0 to bit 0, keep bit 2)
  ic.writeIStat(~IRQ_VBLANK);
  EXPECT_TRUE(ic.hasPendingInterrupt()); // CDROM still pending

  // Acknowledge CDROM
  ic.writeIStat(~IRQ_CDROM);
  EXPECT_FALSE(ic.hasPendingInterrupt());
}

TEST(InterruptController, MultipleIRQSources) {
  InterruptController ic;
  ic.writeIMask(0x7FF); // Enable all
  ic.raiseInterrupt(IRQ_TMR0);
  ic.raiseInterrupt(IRQ_TMR1);
  ic.raiseInterrupt(IRQ_DMA);
  EXPECT_EQ(ic.readIStat(), (IRQ_TMR0 | IRQ_TMR1 | IRQ_DMA));
}

// Timer Tests

TEST(Timer, InitialState) {
  Timers timers;
  EXPECT_EQ(timers.readRegister(0x1F801100), 0); // Counter 0
  EXPECT_EQ(timers.readRegister(0x1F801110), 0); // Counter 1
  EXPECT_EQ(timers.readRegister(0x1F801120), 0); // Counter 2
}

TEST(Timer, CounterIncrement) {
  Timers timers;
  timers.tick(100);
  uint16_t val = timers.readRegister(0x1F801100);
  EXPECT_EQ(val, 100);
}

TEST(Timer, TargetIRQ) {
  Timers timers;
  // Set TMR0 target to 50
  timers.writeRegister(0x1F801108, 50);
  // Set mode: IRQ on target, reset on target
  timers.writeRegister(0x1F801104, (1 << 3) | (1 << 4));

  uint32_t irqs = timers.tick(50);
  EXPECT_NE(irqs & IRQ_TMR0, 0u); // Should fire
}

TEST(Timer, OverflowIRQ) {
  Timers timers;
  // Set mode: IRQ on overflow
  timers.writeRegister(0x1F801104, (1 << 5));

  // Tick close to overflow
  timers.writeRegister(0x1F801100, 0xFFF0); // start near max
  // Need to tick 16 more to overflow
  uint32_t irqs = timers.tick(16);
  EXPECT_NE(irqs & IRQ_TMR0, 0u);
}

TEST(Timer, WriteModResetsCounter) {
  Timers timers;
  timers.tick(100);
  EXPECT_EQ(timers.readRegister(0x1F801100), 100);

  // Writing mode resets counter
  timers.writeRegister(0x1F801104, 0);
  EXPECT_EQ(timers.readRegister(0x1F801100), 0);
}

TEST(Timer, ResetOnTarget) {
  Timers timers;
  timers.writeRegister(0x1F801108, 10);       // target = 10
  timers.writeRegister(0x1F801104, (1 << 3)); // reset on target

  timers.tick(15);
  // Counter should have reset at 10, so now at 5
  uint16_t val = timers.readRegister(0x1F801100);
  EXPECT_EQ(val, 5);
}

// VBlank IRQ Test

TEST(Vblank, IRQRaised) {
  InterruptController ic;
  ic.writeIMask(IRQ_VBLANK);
  ic.raiseInterrupt(IRQ_VBLANK);
  EXPECT_TRUE(ic.hasPendingInterrupt());
}

// Timer::tick(n) jumps straight to the next target/overflow event instead of
// counting cycle by cycle (it runs ~1.7M cycles per frame).  It must leave the
// timer exactly where n single-cycle ticks would, and report the same IRQ.
TEST(Timer, BulkTickMatchesSingleCycleTicks) {
  uint32_t seed = 12345;
  auto rnd = [&seed]() { return seed = seed * 1664525u + 1013904223u; };
  for (int trial = 0; trial < 400; ++trial) {
    Timer bulk, step;
    const uint16_t mode = static_cast<uint16_t>(rnd() & 0x78); // bits 3..6
    const uint16_t target = static_cast<uint16_t>(trial % 5 == 0 ? 0 : rnd());
    const uint16_t start = static_cast<uint16_t>(rnd());
    for (Timer *t : {&bulk, &step}) {
      t->writeMode(mode);
      t->writeTarget(target);
      t->writeCounter(start);
    }
    for (int chunk = 0; chunk < 6; ++chunk) {
      const uint32_t n = rnd() % 200000;
      bool firedStep = false;
      for (uint32_t i = 0; i < n; ++i)
        firedStep |= step.tick(1);
      ASSERT_EQ(bulk.tick(n), firedStep) << "trial " << trial << " chunk " << chunk;
      ASSERT_EQ(bulk.readCounter(), step.readCounter()) << "trial " << trial;
      ASSERT_EQ(bulk.readMode(), step.readMode()) << "trial " << trial;
    }
  }
}

// Register map and clock sources
//
// Contract: psx-spx timers.md "1F801100h+N*10h - Timer 0..2".

namespace {
constexpr uint32_t counterReg(uint32_t t) { return 0x1F801100 + t * 0x10; }
constexpr uint32_t modeReg(uint32_t t) { return 0x1F801104 + t * 0x10; }
constexpr uint32_t targetReg(uint32_t t) { return 0x1F801108 + t * 0x10; }
constexpr uint16_t kSourceAlt = 1u << 8; // clock source 1
constexpr uint16_t kSourceDiv8 = 2u << 8;
constexpr uint16_t kIrqOnTarget = 1u << 4;
constexpr uint16_t kIrqOnOverflow = 1u << 5;
constexpr uint16_t kIrqRepeat = 1u << 6;
constexpr uint16_t kResetOnTarget = 1u << 3;
} // namespace

TEST(Timer, TargetRegisterReadsBack) {
  Timers timers;

  timers.writeRegister(targetReg(2), 0x1234);

  EXPECT_EQ(timers.readRegister(targetReg(2)), 0x1234);
}

TEST(Timer, CounterRegisterIsWritable) {
  Timers timers;

  timers.writeRegister(counterReg(1), 0x0042);

  EXPECT_EQ(timers.readRegister(counterReg(1)), 0x0042);
}

TEST(Timer, ModeRegisterReadsBackItsControlBits) {
  Timers timers;

  timers.writeRegister(modeReg(0), kIrqOnTarget | kResetOnTarget);

  EXPECT_EQ(timers.readRegister(modeReg(0)) & 0x3FF, kIrqOnTarget | kResetOnTarget);
}

TEST(Timer, ThereIsNoFourthTimer) {
  Timers timers;

  timers.writeRegister(0x1F801138, 0x1234); // would be timer 3 target

  EXPECT_EQ(timers.readRegister(0x1F801138), 0);
  EXPECT_EQ(timers.readRegister(targetReg(0)), 0) << "must not alias timer 0";
}

TEST(Timer, UnusedRegisterSlotReadsZero) {
  Timers timers;

  EXPECT_EQ(timers.readRegister(0x1F80110C), 0);
}

TEST(Timer, ReachingTheTargetSetsModeBit11) {
  Timers timers;
  timers.writeRegister(modeReg(0), 0);
  timers.writeRegister(targetReg(0), 10);

  timers.tick(10);

  EXPECT_NE(timers.readRegister(modeReg(0)) & (1u << 11), 0u);
  EXPECT_EQ(timers.readRegister(modeReg(0)) & (1u << 12), 0u) << "no wrap yet";
}

TEST(Timer, WrappingPastFfffSetsModeBit12) {
  Timers timers;
  timers.writeRegister(modeReg(0), 0);
  timers.writeRegister(counterReg(0), 0xFFFF);

  timers.tick(1);

  EXPECT_NE(timers.readRegister(modeReg(0)) & (1u << 12), 0u);
}

TEST(Timer, WritingTheModeClearsTheReachedFlags) {
  Timers timers;
  timers.writeRegister(targetReg(0), 10);
  timers.tick(10);

  timers.writeRegister(modeReg(0), 0);

  EXPECT_EQ(timers.readRegister(modeReg(0)) & (3u << 11), 0u);
}

TEST(Timer, Timer0CountsDotclockPulsesWhenSourceIsDotclock) {
  Timers timers;
  timers.writeRegister(modeReg(0), kSourceAlt);

  timers.tick(1000, false, true); // one dotclock pulse
  timers.tick(1000, false, false); // system clock only

  EXPECT_EQ(timers.readRegister(counterReg(0)), 1);
}

TEST(Timer, Timer1CountsHblanksWhenSourceIsHblank) {
  Timers timers;
  timers.writeRegister(modeReg(1), kSourceAlt);

  timers.tick(1000, true, false);
  timers.tick(1000, true, false);
  timers.tick(1000, false, false);

  EXPECT_EQ(timers.readRegister(counterReg(1)), 2);
}

TEST(Timer, Timer1CountsTheSystemClockBySource0) {
  Timers timers;

  timers.tick(300);

  EXPECT_EQ(timers.readRegister(counterReg(1)), 300);
}

TEST(Timer, Timer2DividesTheSystemClockByEightForSource2) {
  Timers timers;
  timers.writeRegister(modeReg(2), kSourceDiv8);

  timers.tick(800);

  EXPECT_EQ(timers.readRegister(counterReg(2)), 100);
}

TEST(Timer, Timer2DividesTheSystemClockByEightForSource3) {
  Timers timers;
  timers.writeRegister(modeReg(2), 3u << 8);

  timers.tick(80);

  EXPECT_EQ(timers.readRegister(counterReg(2)), 10);
}

TEST(Timer, Timer2CountsTheSystemClockForSource1) {
  Timers timers;
  timers.writeRegister(modeReg(2), kSourceAlt);

  timers.tick(80);

  EXPECT_EQ(timers.readRegister(counterReg(2)), 80);
}

TEST(Timer, Timer1TargetIrqIsReportedAsTmr1) {
  Timers timers;
  timers.writeRegister(targetReg(1), 20);
  timers.writeRegister(modeReg(1), kIrqOnTarget);

  const uint32_t irqs = timers.tick(20);

  EXPECT_EQ(irqs, static_cast<uint32_t>(IRQ_TMR1));
}

TEST(Timer, Timer2TargetIrqIsReportedAsTmr2) {
  Timers timers;
  timers.writeRegister(targetReg(2), 20);
  timers.writeRegister(modeReg(2), kIrqOnTarget);

  const uint32_t irqs = timers.tick(20);

  EXPECT_EQ(irqs, static_cast<uint32_t>(IRQ_TMR2));
}

TEST(Timer, OneShotIrqFiresOnlyOnceUntilTheModeIsRewritten) {
  Timers timers;
  timers.writeRegister(targetReg(0), 10);
  timers.writeRegister(modeReg(0), kIrqOnTarget | kResetOnTarget);

  const uint32_t first = timers.tick(10);
  const uint32_t second = timers.tick(10);

  EXPECT_EQ(first, static_cast<uint32_t>(IRQ_TMR0));
  EXPECT_EQ(second, 0u);
}

TEST(Timer, RepeatIrqFiresOnEveryTargetHit) {
  Timers timers;
  timers.writeRegister(targetReg(0), 10);
  timers.writeRegister(modeReg(0), kIrqOnTarget | kResetOnTarget | kIrqRepeat);

  timers.tick(10);
  const uint32_t second = timers.tick(10);

  EXPECT_EQ(second, static_cast<uint32_t>(IRQ_TMR0));
}

TEST(Timer, OverflowIrqIsNotRaisedWhenOnlyTargetIrqIsEnabled) {
  Timers timers;
  timers.writeRegister(targetReg(0), 0x100);
  timers.writeRegister(modeReg(0), kIrqOnTarget);
  timers.writeRegister(counterReg(0), 0xFFF0);

  const uint32_t irqs = timers.tick(0x20); // wraps, stays below the target

  EXPECT_EQ(irqs, 0u);
}

TEST(Timer, OverflowIrqWithRepeatFiresOnEveryWrap) {
  Timers timers;
  timers.writeRegister(modeReg(0), kIrqOnOverflow | kIrqRepeat);

  timers.tick(0x10000);
  const uint32_t second = timers.tick(0x10000);

  EXPECT_EQ(second, static_cast<uint32_t>(IRQ_TMR0));
}

TEST(InterruptController, MaskKeepsOnlyTheElevenSources) {
  InterruptController ic;

  ic.writeIMask(0xFFFFFFFF);

  EXPECT_EQ(ic.readIMask(), 0x7FFu);
}

TEST(InterruptController, ResetClearsStatusAndMask) {
  InterruptController ic;
  ic.writeIMask(IRQ_DMA);
  ic.raiseInterrupt(IRQ_DMA);

  ic.reset();

  EXPECT_EQ(ic.readIStat(), 0u);
  EXPECT_EQ(ic.readIMask(), 0u);
}
