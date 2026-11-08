#include "runtime/save_state.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>
#include <gtest/gtest.h>

using namespace ps1;

class SaveStateTest : public ::testing::Test {
protected:
  std::string tmpPath;

  void SetUp() override {
    // Use a unique path per test instance to avoid parallel-test races.
    auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string name = std::string(info->test_case_name()) + "_" + info->name();
    tmpPath = (std::filesystem::temp_directory_path() /
               ("ps1_test_" + name + ".sav")).string();
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

// Header validation and device round trips

namespace {

std::string uniqueStatePath(const char *tag) {
  static int counter = 0;
  return (std::filesystem::temp_directory_path() /
          ("ps1_test_state_" + std::to_string(::getpid()) + "_" +
           std::to_string(++counter) + "_" + tag + ".sav"))
      .string();
}

struct Machine {
  std::unique_ptr<Memory> mem = std::make_unique<Memory>();
  std::unique_ptr<gpu::GPU> gpu = std::make_unique<gpu::GPU>();
  std::unique_ptr<spu::SPU> spu = std::make_unique<spu::SPU>();
  DMA dma;
  cdrom::CdromController cdrom;
  Timers timers;
  InterruptController irq;
  input::InputController input;
  recomp_context ctx;
  Machine() { ctx.reset(); }

  bool save(const std::string &p) {
    return SaveState::save(p, ctx, *mem, *gpu, *spu, dma, cdrom, timers, irq, input);
  }
  bool load(const std::string &p) {
    return SaveState::load(p, ctx, *mem, *gpu, *spu, dma, cdrom, timers, irq, input);
  }
};

void writeHeader(const std::string &path, const char magic[4], uint32_t version) {
  SaveStateHeader h;
  std::memcpy(h.magic, magic, 4);
  h.version = version;
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char *>(&h), sizeof h);
}

class SaveStateFile : public ::testing::Test {
protected:
  std::string path = uniqueStatePath("state");
  void TearDown() override { std::filesystem::remove(path); }
};

} // namespace

TEST_F(SaveStateFile, SavingToAnUnwritablePathFails) {
  Machine m;

  EXPECT_FALSE(m.save("/nonexistent_dir_ps1_test/state.sav"));
}

TEST_F(SaveStateFile, LoadRejectsAnUnsupportedVersionAndLeavesRamAlone) {
  writeHeader(path, "PS1S", 2);
  Machine m;
  m.mem->write32(0x100, 0x12345678);

  EXPECT_FALSE(m.load(path));
  EXPECT_EQ(m.mem->read32(0x100), 0x12345678u);
}

TEST_F(SaveStateFile, IsValidRejectsAnUnsupportedVersion) {
  writeHeader(path, "PS1S", 2);

  EXPECT_FALSE(SaveState::isValid(path));
}

TEST_F(SaveStateFile, IsValidRejectsATruncatedHeader) {
  {
    std::ofstream f(path, std::ios::binary);
    f << "PS";
  }

  EXPECT_FALSE(SaveState::isValid(path));
}

TEST_F(SaveStateFile, IsValidAcceptsABareCurrentHeader) {
  writeHeader(path, "PS1S", 1);

  EXPECT_TRUE(SaveState::isValid(path));
}

TEST_F(SaveStateFile, RoundTripRestoresDmaSpuAndVram) {
  Machine a;
  a.dma.writeRegister(0x1F8010C0, 0x00012340); // DMA4 MADR
  a.dma.writeRegister(0x1F8010C4, 0x00100010); // DMA4 BCR
  a.spu->writeSoundRam(0x1000, 0xABCD);
  a.gpu->writeGP0(0x020000FF); // fill 16x1 red at (0,0)
  a.gpu->writeGP0(0x00000000);
  a.gpu->writeGP0(0x00010010);
  a.irq.writeIMask(IRQ_SPU);
  ASSERT_TRUE(a.save(path));
  Machine b;

  ASSERT_TRUE(b.load(path));

  EXPECT_EQ(b.dma.readRegister(0x1F8010C0), 0x00012340u);
  EXPECT_EQ(b.dma.readRegister(0x1F8010C4), 0x00100010u);
  EXPECT_EQ(b.spu->readSoundRam(0x1000), 0xABCD);
  EXPECT_EQ(b.gpu->getVRAM()[0].raw, 0x001F);
  EXPECT_EQ(b.irq.readIMask(), static_cast<uint32_t>(IRQ_SPU));
}

TEST_F(SaveStateFile, CorruptedRamStillLoads) {
  // The checksum only warns; a damaged state is better than no state.
  Machine a;
  a.mem->write32(0x200, 0x11111111);
  ASSERT_TRUE(a.save(path));
  {
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(sizeof(SaveStateHeader) + 128 + 12 + 64 + 256 + 0x200);
    f.put(0x22);
  }
  Machine b;

  ASSERT_TRUE(b.load(path));

  EXPECT_EQ(b.mem->read8(0x200), 0x22);
}

class SaveStateBadMagic : public ::testing::TestWithParam<const char *> {
protected:
  std::string path = uniqueStatePath("magic");
  void TearDown() override { std::filesystem::remove(path); }
};

TEST_P(SaveStateBadMagic, IsValidRejectsIt) {
  writeHeader(path, GetParam(), 1);

  EXPECT_FALSE(SaveState::isValid(path));
}

TEST_P(SaveStateBadMagic, LoadRejectsIt) {
  writeHeader(path, GetParam(), 1);
  Machine m;

  EXPECT_FALSE(m.load(path));
}

// One wrong byte in each position of "PS1S".
INSTANTIATE_TEST_SUITE_P(EachMagicByte, SaveStateBadMagic,
                         ::testing::Values("XS1S", "PX1S", "PSXS", "PS1X"));
