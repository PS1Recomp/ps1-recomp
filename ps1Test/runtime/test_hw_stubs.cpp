// Smoke test for ps1Runtime — Hardware Subsystems
// Validates that all hardware subsystem classes compile and basic API works

#include <gtest/gtest.h>
#include <runtime/cdrom.h>
#include <runtime/dma/dma.h>
#include <runtime/gpu.h>
#include <runtime/input/input.h>
#include <runtime/spu/spu.h>

TEST(HardwareStubs, GPUReset) {
  ps1::GPU gpu;
  EXPECT_NE(gpu.readGPUSTAT(), 0u);
  gpu.writeGP0(0);
  gpu.writeGP1(0);
  EXPECT_NE(gpu.vramPtr(), nullptr);
}

TEST(HardwareStubs, SPUReset) {
  ps1::spu::SPU spu;
  spu.reset();
  EXPECT_EQ(spu.readRegister(0x1F801DAA), 0u); // SPUCNT
  EXPECT_NE(spu.soundRamPtr(), nullptr);
}

TEST(HardwareStubs, CDROMStatus) {
  ps1::CDROM cdrom;
  EXPECT_EQ(cdrom.readStatus(), 0x18u);
  cdrom.writeCommand(0x01);
  EXPECT_EQ(cdrom.readResponse(), 0u);
}

TEST(HardwareStubs, DMAReset) {
  ps1::DMA dma;
  dma.reset();
  EXPECT_EQ(dma.readRegister(0x1F8010F0), 0x07654321u); // DPCR default
  EXPECT_FALSE(dma.hasInterrupt());
}

TEST(HardwareStubs, InputButtons) {
  ps1::input::InputController input;
  input.reset();
  EXPECT_EQ(input.buttonState(0), 0xFFFFu); // All released
  input.press(ps1::input::BTN_CROSS, 0);
  EXPECT_EQ(input.buttonState(0) & ps1::input::BTN_CROSS, 0u);
  input.release(ps1::input::BTN_CROSS, 0);
  EXPECT_NE(input.buttonState(0) & ps1::input::BTN_CROSS, 0u);
}
