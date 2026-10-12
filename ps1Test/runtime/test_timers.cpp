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
