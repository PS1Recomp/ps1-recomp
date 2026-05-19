#include "runtime/psyq/psyq_hle.h"
#include "runtime/emuptr.h"
#include "runtime/memory.h"
#include "runtime/psyq/psyq_state.h"
#include <chrono>
#include <fmt/format.h>
#include <thread>

namespace ps1::psyq {

// Module-level config
static HleConfig g_cfg;

void configure(const HleConfig &cfg) { g_cfg = cfg; }

const HleConfig &getConfig() { return g_cfg; }

// Helpers

static inline void drainOnce() {
  if (g_cfg.drainCallbacks)
    g_cfg.drainCallbacks();
}

// VSync
//
// PsyQ VSync(n):
//   n == 0 -> sync to next VBlank (wait for counter to change)
//   n  > 0 -> wait until n more VBlanks have elapsed
//   Returns the total VBlank counter value.
//
// Reads psyq_state().vsyncCounter, the C++-side singleton incremented by
// the host VBlank thread (~60 Hz, ~16.6 ms period).  Each iteration:
// drainOnce() pumps the BIOS callback queue (no-op fast path when empty),
// then sleep 100 us to yield CPU before the next atomic poll.  A 1 s
// wall-clock deadline (~60 frames at 60 Hz) guards against deadlock when
// the VBlank thread is not running.
//
// Phase 3.2: after the wait loop, exchange `psyq_state().vblankPending`
// to false.  When the flag was observed `true`, run `deliverVBlankEvent`
// on this (game) thread -- that is what now performs the actual PsyQ
// VBlank work (event-system triggers, queued swap callback, drawSync
// stamping) which used to run on the IRQ-context VBlank thread.  The
// exchange both observes and clears the flag atomically, so two
// consecutive `hle_VSync` calls without an intervening VBlank tick
// cannot drain the same VBlank twice.
//
void hle_VSync(recomp_context *ctx) {
  int32_t n = static_cast<int32_t>(ctx->r[A0]);
  uint32_t frames = (n <= 0) ? 1u : static_cast<uint32_t>(n);

  auto &counter = psyq_state().vsyncCounter;
  uint32_t start = counter.load(std::memory_order_acquire);
  uint32_t target = start + frames;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (counter.load(std::memory_order_acquire) < target &&
         std::chrono::steady_clock::now() < deadline) {
    drainOnce();
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }

  if (psyq_state().vblankPending.exchange(false, std::memory_order_acq_rel) &&
      g_cfg.deliverVBlankEvent) {
    g_cfg.deliverVBlankEvent();
  }

  ctx->r[V0] = counter.load(std::memory_order_acquire);
}

// DrawSync
//
// PsyQ DrawSync(mode):
//   mode 0 -> wait until GPU drawing is complete, return 0
//   mode 1 -> return number of primitives remaining (non-blocking)
//
// Since the runtime GPU processes GP0 commands synchronously, drawing is
// always "complete".  Return 0 for both modes.
//
void hle_DrawSync(recomp_context *ctx) {
  // Drain once to keep event queue healthy
  drainOnce();
  ctx->r[V0] = 0; // 0 = complete / 0 primitives remaining
}

// ResetGraph
//
// PsyQ ResetGraph(mode):
//   mode 0 -> reset + flush + clear display list
//   mode 3 -> flush only
//
// The runtime GPU has no queued command list to flush, so this is a NOP.
//
void hle_ResetGraph(recomp_context *ctx) {
  (void)ctx;
  // NOP: runtime GPU is synchronous, no list to flush
}

// ClearOTag
//
// PsyQ ClearOTag(ot, n):
//   Fills the first `n` entries of ordering table `ot` with end-of-list
//   terminators.  On real hardware each entry is a 24-bit linked-list pointer
//   that forms a chain; the last entry (ot[0]) must hold 0x00FFFFFF.
//
//   a0 = base address of the OT (uint32_t*)
//   a1 = number of entries
//
void hle_ClearOTag(recomp_context *ctx) {
  uint32_t base = ctx->r[A0];
  uint32_t n = ctx->r[A1];
  if (n == 0) {
    ctx->r[V0] = base;
    return;
  }
  // Fill [1 .. n-1] with self-pointers (each entry points to previous)
  for (uint32_t i = 0; i < n - 1; i++) {
    ctx->mem->write32(base + i * 4, base + (i - 1) * 4);
  }
  // Last entry = 0x00FFFFFF (end-of-list sentinel)
  ctx->mem->write32(base + (n - 1) * 4, 0x00FFFFFFu);
  ctx->r[V0] = base; // return pointer to ot
}

// ClearOTagR
//
// Same as ClearOTag but fills in reverse order -- entries are linked
// high-to-low so GPU traverses them from ot[n-1] down to ot[0].
//
void hle_ClearOTagR(recomp_context *ctx) {
  uint32_t base = ctx->r[A0];
  uint32_t n = ctx->r[A1];
  if (n == 0) {
    ctx->r[V0] = base;
    return;
  }
  // ot[n-1] = end-of-list
  ctx->mem->write32(base + (n - 1) * 4, 0x00FFFFFFu);
  // ot[i] links forward: ot[i] -> ot[i+1]
  for (uint32_t i = 0; i < n - 1; i++) {
    ctx->mem->write32(base + i * 4, base + (i + 1) * 4);
  }
  ctx->r[V0] = base;
}

// DrawOTag
//
// PsyQ DrawOTag(ot):
//   Traverses the ordering-table linked list and submits each primitive's
//   GP0 command words to the GPU, in order from tail (a0) to head.
//
//   Node format: header word [31:24]=word_count [23:0]=next_ptr_physical
//   Followed by word_count GP0 data words.
//   Terminal node: next_ptr == 0x00FFFFFF.
//
void hle_DrawOTag(recomp_context *ctx) {
  if (!g_cfg.writeGP0) {
    ctx->r[V0] = 0;
    return;
  }
  uint32_t ptr = ctx->r[A0];
  int safety = 0;
  while ((ptr & 0xFFFFFFu) != 0xFFFFFFu && safety++ < 100000) {
    uint32_t hdr = ctx->mem->read32(ptr);
    uint32_t wordCount = (hdr >> 24) & 0xFF;
    for (uint32_t i = 0; i < wordCount; i++) {
      g_cfg.writeGP0(ctx->mem->read32(ptr + 4 + i * 4));
    }
    uint32_t next = hdr & 0xFFFFFFu;
    if (next == 0xFFFFFFu)
      break;
    ptr = next | 0x80000000u; // restore KSEG0 bit
  }
  ctx->r[V0] = 0;
}

// SetDefDispEnv
//
// PsyQ SetDefDispEnv(env, x, y, w, h):
//   Initialises a DispEnv struct in PS1 RAM.
//
//   PS1 DispEnv layout (from PsyQ SDK headers):
//     +0  disp.x   (int16)  display area X in VRAM
//     +2  disp.y   (int16)  display area Y in VRAM
//     +4  disp.w   (int16)  display width
//     +6  disp.h   (int16)  display height
//     +8  screen.x (int16)  (unused / screen clip)
//     +10 screen.y (int16)
//     +12 screen.w (int16)
//     +14 screen.h (int16)
//     +16 isinter  (uint8)  interlace enable
//     +17 isrgb24  (uint8)  24bpp enable
//     +18 pad[2]
//
namespace {
struct DispEnv {
  int16_t disp_x, disp_y;     // 0, 2 -- VRAM display area origin
  int16_t disp_w, disp_h;     // 4, 6
  int16_t screen_x, screen_y; // 8, 10
  int16_t screen_w, screen_h; // 12, 14
  uint8_t isinter;            // 16
  uint8_t isrgb24;            // 17
  uint8_t pad[2];             // 18, 19
};
static_assert(sizeof(DispEnv) == 20, "PsyQ DispEnv is 20 bytes");
} // namespace

void hle_SetDefDispEnv(recomp_context *ctx) {
  ps1::emuptr<DispEnv> env(ctx->r[A0]);
  int16_t x = static_cast<int16_t>(ctx->r[A1]);
  int16_t y = static_cast<int16_t>(ctx->r[A2]);
  int16_t w = static_cast<int16_t>(ctx->r[A3]);
  // h is on the stack (5th argument)
  int16_t h = static_cast<int16_t>(ctx->mem->read32(ctx->r[SP] + 16));

  env->disp_x   = x;
  env->disp_y   = y;
  env->disp_w   = w;
  env->disp_h   = h;
  env->screen_x = 0;
  env->screen_y = 0;
  env->screen_w = w;
  env->screen_h = h;
  env->isinter  = 0; // non-interlaced
  env->isrgb24  = 0; // RGB15
  env->pad[0]   = 0;
  env->pad[1]   = 0;

  ctx->r[V0] = static_cast<uint32_t>(env);
}

// PutDispEnv
//
// PsyQ PutDispEnv(env):
//   Applies a DispEnv to the GPU by sending GP1 commands:
//     GP1(0x05) -- set display start (VRAM X/Y)
//     GP1(0x06) -- set horizontal display range
//     GP1(0x07) -- set vertical   display range
//     GP1(0x08) -- set display mode (width, height, interlace)
//
void hle_PutDispEnv(recomp_context *ctx) {
  if (!g_cfg.writeGP1) {
    return;
  }
  uint32_t envPtr = ctx->r[A0];
  int16_t vx   = static_cast<int16_t>(ctx->mem->read16(envPtr + 0));
  int16_t vy   = static_cast<int16_t>(ctx->mem->read16(envPtr + 2));
  int16_t vw   = static_cast<int16_t>(ctx->mem->read16(envPtr + 4));
  int16_t vh   = static_cast<int16_t>(ctx->mem->read16(envPtr + 6));
  uint8_t isinter = ctx->mem->read8(envPtr + 16);
  uint8_t isrgb24 = ctx->mem->read8(envPtr + 17);

  // GP1(0x05): display start address
  g_cfg.writeGP1(0x05000000u | (static_cast<uint32_t>(vy) << 10) |
                                 static_cast<uint32_t>(vx & 0x3FF));

  // GP1(0x06): horizontal display range [X1, X2]
  // NTSC: X1=0x260, X2=0x260+vw*8 (for 8-pix/line units)
  uint32_t x1 = 0x260u;
  uint32_t x2 = x1 + static_cast<uint32_t>(vw) * 8u;
  g_cfg.writeGP1(0x06000000u | (x2 << 12) | x1);

  // GP1(0x07): vertical display range [Y1, Y2]
  // NTSC: Y1=0x88, Y2=0x88+vh
  uint32_t y1 = 0x88u;
  uint32_t y2 = y1 + static_cast<uint32_t>(vh);
  g_cfg.writeGP1(0x07000000u | (y2 << 10) | y1);

  // GP1(0x08): display mode
  // Bits: [2:0]=hres (0=256,1=320,2=512,3=640), [3]=vres (0=240,1=480)
  //       [4]=video (0=NTSC), [5]=isrgb24, [6]=isinter
  uint32_t hmode = (vw >= 640) ? 3u : (vw >= 512) ? 2u : (vw >= 320) ? 1u : 0u;
  uint32_t vmode = (vh >= 480) ? 1u : 0u;
  uint32_t dispMode = hmode | (vmode << 3) | (static_cast<uint32_t>(isrgb24) << 5) |
                      (static_cast<uint32_t>(isinter) << 6);
  g_cfg.writeGP1(0x08000000u | dispMode);
}

// SetDefDrawEnv
//
// PsyQ SetDefDrawEnv(env, x, y, w, h):
//   Initialises a DrawEnv struct in PS1 RAM.
//
//   PS1 DrawEnv layout (from PsyQ SDK headers):
//     +0  clip.x   (int16)  clipping area X
//     +2  clip.y   (int16)  clipping area Y
//     +4  clip.w   (int16)  clipping width
//     +6  clip.h   (int16)  clipping height
//     +8  ofs[0]   (int16)  drawing X offset
//     +10 ofs[1]   (int16)  drawing Y offset
//     +12 tw ...   (8 bytes, texture window)
//     +20 tpage    (uint16) texture page
//     +22 dtd      (uint8)  dithering
//     +23 dfe      (uint8)  draw-to-display enable
//     ... (DR_TPAGE follows)
//
void hle_SetDefDrawEnv(recomp_context *ctx) {
  uint32_t envPtr = ctx->r[A0];
  int16_t  x  = static_cast<int16_t>(ctx->r[A1]);
  int16_t  y  = static_cast<int16_t>(ctx->r[A2]);
  int16_t  w  = static_cast<int16_t>(ctx->r[A3]);
  int16_t  h  = static_cast<int16_t>(ctx->mem->read32(ctx->r[SP] + 16));

  // clip rect
  ctx->mem->write16(envPtr + 0, static_cast<uint16_t>(x));
  ctx->mem->write16(envPtr + 2, static_cast<uint16_t>(y));
  ctx->mem->write16(envPtr + 4, static_cast<uint16_t>(w));
  ctx->mem->write16(envPtr + 6, static_cast<uint16_t>(h));
  // drawing offset = clip origin
  ctx->mem->write16(envPtr + 8,  static_cast<uint16_t>(x));
  ctx->mem->write16(envPtr + 10, static_cast<uint16_t>(y));
  // texture window: no restriction (all zeros)
  ctx->mem->write16(envPtr + 12, 0);
  ctx->mem->write16(envPtr + 14, 0);
  ctx->mem->write16(envPtr + 16, 0);
  ctx->mem->write16(envPtr + 18, 0);
  // tpage = 0 (default), dtd=1 (dithering), dfe=0
  ctx->mem->write16(envPtr + 20, 0);
  ctx->mem->write8(envPtr + 22, 1); // dtd
  ctx->mem->write8(envPtr + 23, 0); // dfe

  ctx->r[V0] = envPtr;
}

// PutDrawEnv
//
// PsyQ PutDrawEnv(env):
//   Applies a DrawEnv to the GPU by sending GP0 commands:
//     GP0(0xE1) -- texture page (tpage)
//     GP0(0xE2) -- texture window
//     GP0(0xE3) -- drawing area top-left
//     GP0(0xE4) -- drawing area bottom-right
//     GP0(0xE5) -- drawing offset
//
void hle_PutDrawEnv(recomp_context *ctx) {
  if (!g_cfg.writeGP0) {
    return;
  }
  uint32_t envPtr = ctx->r[A0];
  int16_t cx  = static_cast<int16_t>(ctx->mem->read16(envPtr + 0));
  int16_t cy  = static_cast<int16_t>(ctx->mem->read16(envPtr + 2));
  int16_t cw  = static_cast<int16_t>(ctx->mem->read16(envPtr + 4));
  int16_t ch  = static_cast<int16_t>(ctx->mem->read16(envPtr + 6));
  int16_t ox  = static_cast<int16_t>(ctx->mem->read16(envPtr + 8));
  int16_t oy  = static_cast<int16_t>(ctx->mem->read16(envPtr + 10));
  uint16_t tpage = ctx->mem->read16(envPtr + 20);
  uint8_t dtd = ctx->mem->read8(envPtr + 22);

  // GP0(0xE1): texture page + dithering
  uint32_t e1 = 0xE1000000u | (tpage & 0x7FFu) | (static_cast<uint32_t>(dtd) << 9);
  g_cfg.writeGP0(e1);

  // GP0(0xE3): drawing area top-left
  g_cfg.writeGP0(0xE3000000u | (static_cast<uint32_t>(cy & 0x1FF) << 10) |
                                 static_cast<uint32_t>(cx & 0x3FF));

  // GP0(0xE4): drawing area bottom-right (inclusive)
  int16_t x2 = static_cast<int16_t>(cx + cw - 1);
  int16_t y2 = static_cast<int16_t>(cy + ch - 1);
  g_cfg.writeGP0(0xE4000000u | (static_cast<uint32_t>(y2 & 0x1FF) << 10) |
                                 static_cast<uint32_t>(x2 & 0x3FF));

  // GP0(0xE5): drawing offset
  g_cfg.writeGP0(0xE5000000u |
                 (static_cast<uint32_t>(static_cast<int32_t>(oy) & 0x7FF) << 11) |
                 static_cast<uint32_t>(static_cast<int32_t>(ox) & 0x7FF));
}

} // namespace ps1::psyq
