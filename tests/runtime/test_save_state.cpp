#include "runtime/save_state.h"
#include <cstdio>
#include <filesystem>
#include <gtest/gtest.h>

using namespace ps1;

class SaveStateTest : public ::testing::Test {
protected:
  std::string tmpPath;

  void SetUp() override {
    tmpPath = std::filesystem::temp_directory_path() / "ps1_test_savestate.sav";
  }
  void TearDown() override { std::remove(tmpPath.c_str()); }
};

TEST_F(SaveStateTest, SaveAndLoadPreservesMemory) {
  // Set up initial state
  Memory mem;
  gpu::GPU gpu;
  spu::SPU spu;
  DMA dma;
  cdrom::CdromController cdrom;
  Timers timers;
  InterruptController irq;
  input::InputController input;
  recomp_context ctx;
  ctx.reset();

  // Write test pattern to RAM
  mem.write32(0x00000000, 0xDEADBEEF);
  mem.write32(0x00000004, 0xCAFEBABE);
  mem.write8(0x00001000, 0x42);

  // Set CPU registers
  ctx.r[1] = 100;
  ctx.r[31] = 0x80010000;
  ctx.pc = 0x80020000;

  // Save
  ASSERT_TRUE(SaveState::save(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                              irq, input));

  // Verify file exists
  EXPECT_TRUE(std::filesystem::exists(tmpPath));
  EXPECT_TRUE(SaveState::isValid(tmpPath));

  // Reset everything
  mem.reset();
  ctx.reset();

  // Verify reset worked
  EXPECT_EQ(mem.read32(0x00000000), 0u);
  EXPECT_EQ(ctx.r[1], 0u);

  // Load
  ASSERT_TRUE(SaveState::load(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                              irq, input));

  // Verify restored state
  EXPECT_EQ(mem.read32(0x00000000), 0xDEADBEEFu);
  EXPECT_EQ(mem.read32(0x00000004), 0xCAFEBABEu);
  EXPECT_EQ(mem.read8(0x00001000), 0x42u);
  EXPECT_EQ(ctx.r[1], 100u);
  EXPECT_EQ(ctx.r[31], 0x80010000u);
  EXPECT_EQ(ctx.pc, 0x80020000u);
}

TEST_F(SaveStateTest, InvalidFileRejected) {
  // Try loading a non-existent file
  Memory mem;
  gpu::GPU gpu;
  spu::SPU spu;
  DMA dma;
  cdrom::CdromController cdrom;
  Timers timers;
  InterruptController irq;
  input::InputController input;
  recomp_context ctx;
  ctx.reset();

  EXPECT_FALSE(SaveState::load("/nonexistent/path.sav", ctx, mem, gpu, spu, dma,
                               cdrom, timers, irq, input));
}

TEST_F(SaveStateTest, IsValidChecks) {
  EXPECT_FALSE(SaveState::isValid("/nonexistent.sav"));

  // Create a file with wrong magic
  {
    std::ofstream f(tmpPath, std::ios::binary);
    f << "NOT_A_SAVE_STATE";
  }
  EXPECT_FALSE(SaveState::isValid(tmpPath));
}

TEST_F(SaveStateTest, SavePreservesCOP0) {
  Memory mem;
  gpu::GPU gpu;
  spu::SPU spu;
  DMA dma;
  cdrom::CdromController cdrom;
  Timers timers;
  InterruptController irq;
  input::InputController input;
  recomp_context ctx;
  ctx.reset();

  ctx.cop0[12] = 0x10000000; // SR
  ctx.cop0[13] = 0x00000400; // Cause

  ASSERT_TRUE(SaveState::save(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                              irq, input));
  ctx.reset();
  ASSERT_TRUE(SaveState::load(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                              irq, input));

  EXPECT_EQ(ctx.cop0[12], 0x10000000u);
  EXPECT_EQ(ctx.cop0[13], 0x00000400u);
}

TEST_F(SaveStateTest, SavePreservesIRQState) {
  Memory mem;
  gpu::GPU gpu;
  spu::SPU spu;
  DMA dma;
  cdrom::CdromController cdrom;
  Timers timers;
  InterruptController irq;
  input::InputController input;
  recomp_context ctx;
  ctx.reset();

  irq.writeIMask(IRQ_VBLANK | IRQ_CDROM);
  irq.raiseInterrupt(IRQ_VBLANK);

  ASSERT_TRUE(SaveState::save(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                              irq, input));

  irq.reset();
  EXPECT_FALSE(irq.hasPendingInterrupt());

  ASSERT_TRUE(SaveState::load(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                              irq, input));

  EXPECT_EQ(irq.readIMask(), (IRQ_VBLANK | IRQ_CDROM));
  EXPECT_TRUE(irq.readIStat() & IRQ_VBLANK);
}
