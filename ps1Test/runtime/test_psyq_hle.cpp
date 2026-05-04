// Tests for ps1::psyq HLE implementations
// Covers: VSync, DrawSync, ResetGraph, ClearOTag, ClearOTagR, DrawOTag,
//         SetDefDispEnv, PutDispEnv, SetDefDrawEnv, PutDrawEnv

#include "runtime/psyq/psyq_hle.h"
#include "runtime/cpu_context.h"
#include "runtime/memory.h"
#include "runtime/psyq/psyq_state.h"
#include <gtest/gtest.h>
#include <vector>

using namespace ps1;
using namespace ps1::psyq;

// ── Fixture ──────────────────────────────────────────────────────────────────

class PsyqHleTest : public ::testing::Test {
protected:
    Memory       mem;
    recomp_context ctx;

    // GP0/GP1 capture
    std::vector<uint32_t> gp0Words;
    std::vector<uint32_t> gp1Words;

    // Each drainCallbacks call simulates one VBlank tick.
    int drainCalls = 0;

    void SetUp() override {
        mem.reset();
        ctx.reset();
        ctx.mem = &mem;
        ctx.r[SP] = 0x801FF000; // stack pointer
        gp0Words.clear();
        gp1Words.clear();
        drainCalls = 0;
        psyq_state().reset();

        HleConfig cfg;
        cfg.writeGP0 = [this](uint32_t w) { gp0Words.push_back(w); };
        cfg.writeGP1 = [this](uint32_t w) { gp1Words.push_back(w); };
        cfg.drainCallbacks = [this]() {
            ++drainCalls;
            psyq_state().vsyncCounter.fetch_add(1, std::memory_order_release);
        };
        configure(cfg);
    }

    void TearDown() override {
        psyq_state().reset();
    }
};

// ── DrawSync ─────────────────────────────────────────────────────────────────

TEST_F(PsyqHleTest, DrawSyncMode0ReturnsZero) {
    ctx.r[A0] = 0; // mode 0 = wait for completion
    hle_DrawSync(&ctx);
    EXPECT_EQ(ctx.r[V0], 0u);
}

TEST_F(PsyqHleTest, DrawSyncMode1ReturnsZero) {
    ctx.r[A0] = 1; // mode 1 = query remaining
    hle_DrawSync(&ctx);
    EXPECT_EQ(ctx.r[V0], 0u);
}

// ── ResetGraph ────────────────────────────────────────────────────────────────

TEST_F(PsyqHleTest, ResetGraphIsNop) {
    ctx.r[A0] = 0;
    hle_ResetGraph(&ctx);
    // No crash, no GP0/GP1 commands emitted
    EXPECT_TRUE(gp0Words.empty());
    EXPECT_TRUE(gp1Words.empty());
}

// ── VSync ─────────────────────────────────────────────────────────────────────

TEST_F(PsyqHleTest, VSyncWaitsOneFrame) {
    ctx.r[A0] = 1;
    hle_VSync(&ctx);
    EXPECT_GE(drainCalls, 1);
    EXPECT_EQ(ctx.r[V0], psyq_state().vsyncCounter.load());
    EXPECT_GE(psyq_state().vsyncCounter.load(), 1u);
}

TEST_F(PsyqHleTest, VSyncWaitsMultipleFrames) {
    ctx.r[A0] = 3;
    hle_VSync(&ctx);
    EXPECT_GE(psyq_state().vsyncCounter.load(), 3u);
    EXPECT_EQ(ctx.r[V0], psyq_state().vsyncCounter.load());
}

TEST_F(PsyqHleTest, VSyncZeroWaitsOneFrame) {
    ctx.r[A0] = 0; // n=0 means "wait for next VBlank"
    hle_VSync(&ctx);
    EXPECT_GE(psyq_state().vsyncCounter.load(), 1u);
}

TEST_F(PsyqHleTest, VSyncReturnsCounterPostAdvance) {
    psyq_state().vsyncCounter.store(100, std::memory_order_release);
    ctx.r[A0] = 2;
    hle_VSync(&ctx);
    EXPECT_GE(psyq_state().vsyncCounter.load(), 102u);
    EXPECT_EQ(ctx.r[V0], psyq_state().vsyncCounter.load());
}

// Phase 3.2: the VBlank thread no longer calls Bios::triggerVBlankEvent()
// directly — it only sets `psyq_state().vblankPending = true`.  hle_VSync,
// running on the game thread, exchanges the flag at the end of its wait
// and invokes `deliverVBlankEvent` exactly once when the flag was set.
//
// Two consecutive hle_VSync calls without an intervening VBlank thread
// tick must NOT both observe the same pending flag — the first call
// drains it, the second sees `false`.  This test wires
// `deliverVBlankEvent` to a counter to assert the flag is delivered
// once and only once.
TEST_F(PsyqHleTest, VBlankPendingDeliveredOncePerFlagRaise) {
    int vblankDeliveries = 0;
    HleConfig cfg = getConfig();
    cfg.deliverVBlankEvent = [&vblankDeliveries]() { ++vblankDeliveries; };
    configure(cfg);

    // Simulate one VBlank thread tick: counter bumped, flag raised.
    psyq_state().vsyncCounter.store(0, std::memory_order_release);
    psyq_state().vblankPending.store(true, std::memory_order_release);

    // First hle_VSync(1) drains the flag and fires delivery once.
    ctx.r[A0] = 1;
    hle_VSync(&ctx);
    EXPECT_EQ(vblankDeliveries, 1);
    EXPECT_FALSE(psyq_state().vblankPending.load(std::memory_order_acquire));

    // Second hle_VSync(1) — no new VBlank thread tick happened, so the
    // flag must remain false and delivery count must NOT increment.
    // The drainCallbacks fixture-hook still bumps the counter inside the
    // wait loop so the call returns; only the deliverVBlankEvent path is
    // exercised here.
    ctx.r[A0] = 1;
    hle_VSync(&ctx);
    EXPECT_EQ(vblankDeliveries, 1)
        << "Same VBlank delivered twice across consecutive hle_VSync calls";
    EXPECT_FALSE(psyq_state().vblankPending.load(std::memory_order_acquire));

    // Re-raise the flag (simulates the next VBlank thread tick) and
    // confirm a third call now delivers a fresh VBlank.
    psyq_state().vblankPending.store(true, std::memory_order_release);
    ctx.r[A0] = 1;
    hle_VSync(&ctx);
    EXPECT_EQ(vblankDeliveries, 2);
    EXPECT_FALSE(psyq_state().vblankPending.load(std::memory_order_acquire));
}

// Negative case: if no VBlank ever fired, hle_VSync must not call
// deliverVBlankEvent at all.
TEST_F(PsyqHleTest, VSyncSkipsDeliveryWhenFlagNeverRaised) {
    int vblankDeliveries = 0;
    HleConfig cfg = getConfig();
    cfg.deliverVBlankEvent = [&vblankDeliveries]() { ++vblankDeliveries; };
    configure(cfg);

    // Flag stays false throughout.
    EXPECT_FALSE(psyq_state().vblankPending.load(std::memory_order_acquire));

    ctx.r[A0] = 1;
    hle_VSync(&ctx);
    EXPECT_EQ(vblankDeliveries, 0);
}

// ── ClearOTag ─────────────────────────────────────────────────────────────────

TEST_F(PsyqHleTest, ClearOTagFillsEndMarker) {
    // Write a 4-entry OT starting at 0x1000
    const uint32_t base = 0x1000;
    ctx.r[A0] = base;
    ctx.r[A1] = 4;
    hle_ClearOTag(&ctx);

    // Last entry (ot[3]) must be end-of-list
    EXPECT_EQ(mem.read32(base + 3 * 4), 0x00FFFFFFu);
    // Return value = base
    EXPECT_EQ(ctx.r[V0], base);
}

TEST_F(PsyqHleTest, ClearOTagNZeroIsNop) {
    ctx.r[A0] = 0x2000;
    ctx.r[A1] = 0;
    hle_ClearOTag(&ctx);
    EXPECT_EQ(ctx.r[V0], 0x2000u);
}

// ── ClearOTagR ────────────────────────────────────────────────────────────────

TEST_F(PsyqHleTest, ClearOTagRLastEntryIsEndMarker) {
    const uint32_t base = 0x2000;
    ctx.r[A0] = base;
    ctx.r[A1] = 4;
    hle_ClearOTagR(&ctx);

    // Last entry (ot[3]) = end-of-list
    EXPECT_EQ(mem.read32(base + 3 * 4), 0x00FFFFFFu);
}

TEST_F(PsyqHleTest, ClearOTagRLinksForward) {
    const uint32_t base = 0x2000;
    ctx.r[A0] = base;
    ctx.r[A1] = 3;
    hle_ClearOTagR(&ctx);

    // ot[0] should point to ot[1]
    EXPECT_EQ(mem.read32(base + 0 * 4), base + 1 * 4);
    // ot[1] should point to ot[2]
    EXPECT_EQ(mem.read32(base + 1 * 4), base + 2 * 4);
    // ot[2] = end
    EXPECT_EQ(mem.read32(base + 2 * 4), 0x00FFFFFFu);
}

TEST_F(PsyqHleTest, ClearOTagRNZeroIsNop) {
    ctx.r[A0] = 0x3000;
    ctx.r[A1] = 0;
    hle_ClearOTagR(&ctx);
    EXPECT_EQ(ctx.r[V0], 0x3000u);
}

// ── DrawOTag ─────────────────────────────────────────────────────────────────

TEST_F(PsyqHleTest, DrawOTagEmptyListEmitsNothing) {
    // Write a single terminal node at 0x1000
    const uint32_t base = 0x1000;
    // header: 0 words, next = 0xFFFFFF (end)
    mem.write32(base, 0x00FFFFFFu);

    ctx.r[A0] = base;
    hle_DrawOTag(&ctx);

    EXPECT_TRUE(gp0Words.empty());
    EXPECT_EQ(ctx.r[V0], 0u);
}

TEST_F(PsyqHleTest, DrawOTagSubmitsSinglePrimitive) {
    // Node at 0x1000: 2 GP0 words, next = end
    const uint32_t base = 0x1000;
    // header: [31:24]=2 (word_count=2), [23:0]=0xFFFFFF (terminal)
    mem.write32(base + 0, (2u << 24) | 0x00FFFFFFu);
    mem.write32(base + 4, 0x01234567u); // GP0 word 0
    mem.write32(base + 8, 0x89ABCDEFu); // GP0 word 1

    ctx.r[A0] = base;
    hle_DrawOTag(&ctx);

    ASSERT_EQ(gp0Words.size(), 2u);
    EXPECT_EQ(gp0Words[0], 0x01234567u);
    EXPECT_EQ(gp0Words[1], 0x89ABCDEFu);
}

TEST_F(PsyqHleTest, DrawOTagTraversesChain) {
    // Node B at 0x2000 (tail, processed first): 1 word, next = end
    mem.write32(0x2000, (1u << 24) | 0x00FFFFFFu);
    mem.write32(0x2004, 0xAAAAAAAAu);

    // Node A at 0x1000 (head): 1 word, next = B (0x2000, without KSEG0 bit)
    mem.write32(0x1000, (1u << 24) | 0x002000u);
    mem.write32(0x1004, 0xBBBBBBBBu);

    ctx.r[A0] = 0x1000; // start at A
    hle_DrawOTag(&ctx);

    // A's word should come first, then B's
    ASSERT_EQ(gp0Words.size(), 2u);
    EXPECT_EQ(gp0Words[0], 0xBBBBBBBBu); // from A
    EXPECT_EQ(gp0Words[1], 0xAAAAAAAAu); // from B
}

// ── SetDefDispEnv ─────────────────────────────────────────────────────────────

TEST_F(PsyqHleTest, SetDefDispEnvWritesFields) {
    const uint32_t env = 0x3000;
    ctx.r[A0] = env;
    ctx.r[A1] = 320; // x
    ctx.r[A2] = 0;   // y
    ctx.r[A3] = 320; // w
    // h = 240 passed on stack at SP+16
    mem.write32(ctx.r[SP] + 16, 240);

    hle_SetDefDispEnv(&ctx);

    EXPECT_EQ(mem.read16(env + 0),  320u); // disp.x
    EXPECT_EQ(mem.read16(env + 2),  0u);   // disp.y
    EXPECT_EQ(mem.read16(env + 4),  320u); // disp.w
    EXPECT_EQ(mem.read16(env + 6),  240u); // disp.h
    EXPECT_EQ(mem.read8(env + 16),  0u);   // isinter = 0
    EXPECT_EQ(mem.read8(env + 17),  0u);   // isrgb24 = 0
    EXPECT_EQ(ctx.r[V0],            env);  // returns env ptr
}

// ── PutDispEnv ────────────────────────────────────────────────────────────────

TEST_F(PsyqHleTest, PutDispEnvEmitsFourGP1Commands) {
    const uint32_t env = 0x4000;
    // Write a 320x240 display at (0,0)
    mem.write16(env + 0,  0);   // vx
    mem.write16(env + 2,  0);   // vy
    mem.write16(env + 4,  320); // vw
    mem.write16(env + 6,  240); // vh
    mem.write8(env + 16,  0);   // isinter
    mem.write8(env + 17,  0);   // isrgb24

    ctx.r[A0] = env;
    hle_PutDispEnv(&ctx);

    // GP1(0x05), GP1(0x06), GP1(0x07), GP1(0x08)
    ASSERT_EQ(gp1Words.size(), 4u);
    EXPECT_EQ(gp1Words[0] >> 24, 0x05u);
    EXPECT_EQ(gp1Words[1] >> 24, 0x06u);
    EXPECT_EQ(gp1Words[2] >> 24, 0x07u);
    EXPECT_EQ(gp1Words[3] >> 24, 0x08u);
}

// ── SetDefDrawEnv ─────────────────────────────────────────────────────────────

TEST_F(PsyqHleTest, SetDefDrawEnvWritesClipAndOffset) {
    const uint32_t env = 0x5000;
    ctx.r[A0] = env;
    ctx.r[A1] = 0;   // x
    ctx.r[A2] = 0;   // y
    ctx.r[A3] = 320; // w
    mem.write32(ctx.r[SP] + 16, 240); // h

    hle_SetDefDrawEnv(&ctx);

    EXPECT_EQ(mem.read16(env + 0), 0u);    // clip.x
    EXPECT_EQ(mem.read16(env + 2), 0u);    // clip.y
    EXPECT_EQ(mem.read16(env + 4), 320u);  // clip.w
    EXPECT_EQ(mem.read16(env + 6), 240u);  // clip.h
    EXPECT_EQ(mem.read16(env + 8), 0u);    // ofs.x = clip.x
    EXPECT_EQ(mem.read16(env + 10), 0u);   // ofs.y = clip.y
    EXPECT_EQ(mem.read8(env + 22), 1u);    // dtd = 1 (dithering on by default)
    EXPECT_EQ(mem.read8(env + 23), 0u);    // dfe = 0
    EXPECT_EQ(ctx.r[V0], env);
}

// ── PutDrawEnv ────────────────────────────────────────────────────────────────

TEST_F(PsyqHleTest, PutDrawEnvEmitsGP0Commands) {
    const uint32_t env = 0x6000;
    // 320x240 draw area at (0,0), no dithering
    mem.write16(env + 0,  0);   // clip.x
    mem.write16(env + 2,  0);   // clip.y
    mem.write16(env + 4,  320); // clip.w
    mem.write16(env + 6,  240); // clip.h
    mem.write16(env + 8,  0);   // ofs.x
    mem.write16(env + 10, 0);   // ofs.y
    mem.write16(env + 20, 0);   // tpage
    mem.write8(env + 22,  0);   // dtd = 0

    ctx.r[A0] = env;
    hle_PutDrawEnv(&ctx);

    // Expect GP0(0xE1), GP0(0xE3), GP0(0xE4), GP0(0xE5)
    ASSERT_EQ(gp0Words.size(), 4u);
    EXPECT_EQ(gp0Words[0] >> 24, 0xE1u); // tpage
    EXPECT_EQ(gp0Words[1] >> 24, 0xE3u); // draw area top-left
    EXPECT_EQ(gp0Words[2] >> 24, 0xE4u); // draw area bottom-right
    EXPECT_EQ(gp0Words[3] >> 24, 0xE5u); // draw offset
}

TEST_F(PsyqHleTest, PutDrawEnvBottomRightEncoding) {
    const uint32_t env = 0x7000;
    // 256x240 draw area at (0,0)
    mem.write16(env + 0,  0);
    mem.write16(env + 2,  0);
    mem.write16(env + 4,  256); // w
    mem.write16(env + 6,  240); // h
    mem.write16(env + 8,  0);
    mem.write16(env + 10, 0);
    mem.write16(env + 20, 0);
    mem.write8(env + 22,  0);

    ctx.r[A0] = env;
    hle_PutDrawEnv(&ctx);

    // GP0(0xE4): bottom-right = (w-1, h-1) = (255, 239)
    // bits: [19:10]=y, [9:0]=x
    uint32_t e4 = gp0Words[2];
    uint32_t bx = e4 & 0x3FF;
    uint32_t by = (e4 >> 10) & 0x1FF;
    EXPECT_EQ(bx, 255u);
    EXPECT_EQ(by, 239u);
}
