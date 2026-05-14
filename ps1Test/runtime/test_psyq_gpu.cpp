// Tests for the Group 1.A libgpu/libetc/libgs HLEs:
//   SetDispMask, LoadImage, StoreImage, MoveImage, ClearImage,
//   DrawSyncCallback, VSyncCallback, SetVideoMode, GetVideoMode,
//   GsInitGraph, GsDefDispBuff, GsSetWorkBase, GsSortClear.
//
// The libgpu transfer/clear ops translate PsyQ args into GP0/GP1 commands.
// The fixture installs a recording HleConfig so each test can assert the exact
// command sequence written by the HLE.

#include "runtime/cpu_context.h"
#include "runtime/memory.h"
#include "runtime/psyq/psyq_hle.h"
#include "runtime/psyq/psyq_libgpu.h"
#include "runtime/psyq/psyq_registry.h"
#include "runtime/psyq/psyq_state.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

using namespace ps1;
using namespace ps1::psyq;

namespace {

class PsyqGpuTest : public ::testing::Test {
protected:
  Memory mem;
  recomp_context ctx;
  std::vector<uint32_t> gp0;
  std::vector<uint32_t> gp1;

  void SetUp() override {
    ctx.reset();
    ctx.mem = &mem;
    gp0.clear();
    gp1.clear();
    HleConfig cfg;
    cfg.writeGP0 = [this](uint32_t w) { gp0.push_back(w); };
    cfg.writeGP1 = [this](uint32_t w) { gp1.push_back(w); };
    configure(cfg);
    psyq_state().reset();
  }

  void TearDown() override { psyq_state().reset(); }

  // Write a 4-int16 PsyqRect at `p`.
  void writeRect(uint32_t p, int16_t x, int16_t y, int16_t w, int16_t h) {
    mem.write16(p + 0, static_cast<uint16_t>(x));
    mem.write16(p + 2, static_cast<uint16_t>(y));
    mem.write16(p + 4, static_cast<uint16_t>(w));
    mem.write16(p + 6, static_cast<uint16_t>(h));
  }
};

} // namespace

// SetDispMask

TEST_F(PsyqGpuTest, SetDispMaskEnableSendsGP1_03_With0) {
  ctx.r[A0] = 1; // enable
  hle_libgpu_SetDispMask(&ctx);
  ASSERT_EQ(gp1.size(), 1u);
  EXPECT_EQ(gp1[0], 0x03000000u); // bit 0 = 0 → display ON
}

TEST_F(PsyqGpuTest, SetDispMaskDisableSendsGP1_03_With1) {
  ctx.r[A0] = 0;
  hle_libgpu_SetDispMask(&ctx);
  ASSERT_EQ(gp1.size(), 1u);
  EXPECT_EQ(gp1[0], 0x03000001u);
}

// LoadImage

TEST_F(PsyqGpuTest, LoadImageEmitsCpuToVramHeaderAndData) {
  // 4×2 pixels = 8 px = 4 words of pixel data.
  uint32_t rectP = 0x80100000u;
  uint32_t srcP  = 0x80100100u;
  writeRect(rectP, 320, 0, 4, 2);
  uint32_t pixels[4] = {0x12345678u, 0x9ABCDEF0u, 0x11112222u, 0x33334444u};
  for (int i = 0; i < 4; ++i) mem.write32(srcP + i * 4, pixels[i]);

  ctx.r[A0] = rectP;
  ctx.r[A1] = srcP;
  hle_libgpu_LoadImage(&ctx);

  ASSERT_EQ(gp0.size(), 3u + 4u);
  EXPECT_EQ(gp0[0], 0xA0000000u);              // CPU→VRAM cmd
  EXPECT_EQ(gp0[1], (0u << 16) | 320u);        // Y|X
  EXPECT_EQ(gp0[2], (2u << 16) | 4u);          // H|W
  for (int i = 0; i < 4; ++i)
    EXPECT_EQ(gp0[3 + i], pixels[i]);
}

TEST_F(PsyqGpuTest, LoadImageOddPixelCountRoundsUpDataWords) {
  // 3×1 pixels = 3 px → ceil(3/2)=2 words.
  uint32_t rectP = 0x80100000u;
  uint32_t srcP  = 0x80100100u;
  writeRect(rectP, 0, 0, 3, 1);
  mem.write32(srcP + 0, 0xCAFEBABEu);
  mem.write32(srcP + 4, 0xDEADBEEFu);

  ctx.r[A0] = rectP;
  ctx.r[A1] = srcP;
  hle_libgpu_LoadImage(&ctx);
  ASSERT_EQ(gp0.size(), 5u); // 3 hdr + 2 data
  EXPECT_EQ(gp0[3], 0xCAFEBABEu);
  EXPECT_EQ(gp0[4], 0xDEADBEEFu);
}

TEST_F(PsyqGpuTest, LoadImageZeroSizeIsNoop) {
  uint32_t rectP = 0x80100000u;
  writeRect(rectP, 0, 0, 0, 0);
  ctx.r[A0] = rectP;
  ctx.r[A1] = 0x80100100u;
  hle_libgpu_LoadImage(&ctx);
  EXPECT_TRUE(gp0.empty());
}

// StoreImage

TEST_F(PsyqGpuTest, StoreImageEmitsVramToCpuHeader) {
  uint32_t rectP = 0x80100000u;
  writeRect(rectP, 64, 240, 16, 8);
  ctx.r[A0] = rectP;
  ctx.r[A1] = 0x80100200u;
  hle_libgpu_StoreImage(&ctx);
  ASSERT_EQ(gp0.size(), 3u);
  EXPECT_EQ(gp0[0], 0xC0000000u);
  EXPECT_EQ(gp0[1], (240u << 16) | 64u);
  EXPECT_EQ(gp0[2], (8u   << 16) | 16u);
}

// MoveImage

TEST_F(PsyqGpuTest, MoveImageEmitsVramToVramSequence) {
  uint32_t rectP = 0x80100000u;
  writeRect(rectP, 0, 0, 32, 32);
  ctx.r[A0] = rectP;
  ctx.r[A1] = 320; // dx
  ctx.r[A2] = 256; // dy
  hle_libgpu_MoveImage(&ctx);
  ASSERT_EQ(gp0.size(), 4u);
  EXPECT_EQ(gp0[0], 0x80000000u);
  EXPECT_EQ(gp0[1], (0u   << 16) | 0u);
  EXPECT_EQ(gp0[2], (256u << 16) | 320u);
  EXPECT_EQ(gp0[3], (32u  << 16) | 32u);
}

// ClearImage

TEST_F(PsyqGpuTest, ClearImageEmitsFillRect) {
  uint32_t rectP = 0x80100000u;
  writeRect(rectP, 0, 0, 320, 240);
  ctx.r[A0] = rectP;
  ctx.r[A1] = 0x12; // r
  ctx.r[A2] = 0x34; // g
  ctx.r[A3] = 0x56; // b
  hle_libgpu_ClearImage(&ctx);
  ASSERT_EQ(gp0.size(), 3u);
  EXPECT_EQ(gp0[0], 0x02000000u | (0x56u << 16) | (0x34u << 8) | 0x12u);
  EXPECT_EQ(gp0[1], (0u   << 16) | 0u);
  EXPECT_EQ(gp0[2], (240u << 16) | 320u);
}

// Sync callbacks

TEST_F(PsyqGpuTest, DrawSyncCallbackStoresAndReturnsPrev) {
  ctx.r[A0] = 0x80012340u;
  ctx.r[V0] = 0xDEADu;
  hle_libgpu_DrawSyncCallback(&ctx);
  // First call: previous should be 0 (registry just-installed).
  EXPECT_EQ(ctx.r[V0], 0u);

  ctx.r[A0] = 0xBEEFCAFEu;
  hle_libgpu_DrawSyncCallback(&ctx);
  EXPECT_EQ(ctx.r[V0], 0x80012340u);
}

TEST_F(PsyqGpuTest, VSyncCallbackStoresAndReturnsPrev) {
  ctx.r[A0] = 0x80044000u;
  hle_libgpu_VSyncCallback(&ctx);
  // First install returns previous (0 — singleton was reset in SetUp).
  EXPECT_EQ(ctx.r[V0], 0u);
  // The HLE writes through to psyq_state().gpuSwapCb so bios.cpp's VBlank
  // dispatch can find the swap callback without per-game BSS configuration.
  EXPECT_EQ(psyq_state().gpuSwapCb, 0x80044000u);

  // Subsequent install returns whatever we just wrote.
  ctx.r[A0] = 0;
  hle_libgpu_VSyncCallback(&ctx);
  EXPECT_EQ(ctx.r[V0], 0x80044000u);
  EXPECT_EQ(psyq_state().gpuSwapCb, 0u);
}

// Video mode

TEST_F(PsyqGpuTest, SetGetVideoModeRoundTrip) {
  ctx.r[A0] = 0; // NTSC
  hle_libgpu_SetVideoMode(&ctx);
  hle_libgpu_GetVideoMode(&ctx);
  EXPECT_EQ(ctx.r[V0], 0u);

  ctx.r[A0] = 1; // PAL
  hle_libgpu_SetVideoMode(&ctx);
  // Prev should have been NTSC=0
  EXPECT_EQ(ctx.r[V0], 0u);
  hle_libgpu_GetVideoMode(&ctx);
  EXPECT_EQ(ctx.r[V0], 1u);

  // Restore to NTSC so other tests start clean.
  ctx.r[A0] = 0;
  hle_libgpu_SetVideoMode(&ctx);
}

// libgs stubs

TEST_F(PsyqGpuTest, LibgsStubsAreNoopAndDoNotCrash) {
  ctx.r[A0] = 0; ctx.r[A1] = 0; ctx.r[A2] = 0; ctx.r[A3] = 0;
  EXPECT_NO_FATAL_FAILURE(hle_libgs_GsInitGraph(&ctx));
  EXPECT_NO_FATAL_FAILURE(hle_libgs_GsDefDispBuff(&ctx));
  EXPECT_NO_FATAL_FAILURE(hle_libgs_GsSetWorkBase(&ctx));
  EXPECT_NO_FATAL_FAILURE(hle_libgs_GsSortClear(&ctx));
  EXPECT_TRUE(gp0.empty());
  EXPECT_TRUE(gp1.empty());
}

// Registry wiring

TEST_F(PsyqGpuTest, RegistryDispatchesAllNewNames) {
  psyq_register_libgpu_extras();
  // Each name must dispatch without aborting. For HLEs that read RAM (rect /
  // src), give them a valid empty rect so no actual GP0 traffic is generated.
  uint32_t rectP = 0x80100400u;
  writeRect(rectP, 0, 0, 0, 0);

  const char *names[] = {
      "libgpu_SetDispMask",      "libgpu_LoadImage",
      "libgpu_StoreImage",       "libgpu_MoveImage",
      "libgpu_ClearImage",       "libgpu_DrawSyncCallback",
      "libetc_VSyncCallback",    "libetc_SetVideoMode",
      "libetc_GetVideoMode",
      "libgs_GsInitGraph",       "libgs_GsDefDispBuff",
      "libgs_GsSetWorkBase",     "libgs_GsSortClear",
  };
  for (const char *n : names) {
    ctx.reset();
    ctx.mem = &mem;
    ctx.r[A0] = rectP; // safe for both rect-takers and arg-takers
    EXPECT_NO_FATAL_FAILURE(psyq_dispatch(n, &ctx))
        << "dispatch failed for: " << n;
  }
}
