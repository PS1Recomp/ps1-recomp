// Smoke test for ps1xRuntime — Hardware Stubs
// Validates that GPU/SPU/CD-ROM/DMA/Input stubs compile and basic API works

#include <gtest/gtest.h>
#include <runtime/cdrom.h>
#include <runtime/dma.h>
#include <runtime/gpu.h>
#include <runtime/input.h>
#include <runtime/spu.h>

using namespace ps1;

TEST(HardwareStubs, GPUReset) {
  GPU gpu;
  EXPECT_NE(gpu.readGPUSTAT(), 0u);
  gpu.writeGP0(0);
  gpu.writeGP1(0);
  EXPECT_NE(gpu.vramPtr(), nullptr);
}

TEST(HardwareStubs, SPUReset) {
  SPU spu;
  EXPECT_EQ(spu.spuCtrl(), 0u);
  EXPECT_EQ(spu.readRegister(0), 0u);
  EXPECT_NE(spu.soundRamPtr(), nullptr);
}

TEST(HardwareStubs, CDROMStatus) {
  CDROM cdrom;
  EXPECT_EQ(cdrom.readStatus(), 0x18u);
  cdrom.writeCommand(0x01);
  EXPECT_EQ(cdrom.readResponse(), 0u);
}

TEST(HardwareStubs, DMAReset) {
  DMA dma;
  EXPECT_EQ(dma.dpcr(), 0x07654321u);
  EXPECT_EQ(dma.dicr(), 0u);
}

TEST(HardwareStubs, InputButtons) {
  Input input;
  EXPECT_EQ(input.buttonState(), 0xFFFFu); // All released
  input.press(Input::BTN_CROSS);
  EXPECT_EQ(input.buttonState() & Input::BTN_CROSS, 0u); // Pressed = 0
  input.release(Input::BTN_CROSS);
  EXPECT_NE(input.buttonState() & Input::BTN_CROSS, 0u); // Released = 1
}
