#include "runtime/psyq/psyq_hle.h"
#include "runtime/bios/bios.h"
#include "runtime/emuptr.h"
#include "runtime/memory.h"
#include "runtime/metrics.h"
#include "runtime/psyq/psyq_libgpu.h"
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

void applyRootCounterTicks(recomp_context *ctx, uint32_t vblanks) {
  auto &st = psyq_state();
  if (st.rcntTickAddr == 0 || st.rcntTicksPerVBlank == 0 || vblanks == 0 ||
      ctx == nullptr || ctx->mem == nullptr)
    return;
  const uint32_t current = ctx->mem->read32(st.rcntTickAddr);
  ctx->mem->write32(st.rcntTickAddr,
                    current + st.rcntTicksPerVBlank * vblanks);
  ps1::metrics::count("rcnt.ticks", st.rcntTicksPerVBlank * vblanks);
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
    // The VBlank tick runs on this thread now (see Bios::setVBlankPump), so
    // the wait has to drive it -- nothing else advances the counter.
    if (ctx->bios)
      ctx->bios->pumpVBlank();
    drainOnce();
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }

  if (psyq_state().vblankPending.exchange(false, std::memory_order_acq_rel) &&
      g_cfg.deliverVBlankEvent) {
    g_cfg.deliverVBlankEvent();
  }

  const uint32_t end = counter.load(std::memory_order_acquire);
  applyRootCounterTicks(ctx, end - start);

  ctx->r[V0] = end;
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
// ResetGraph
//
// PsyQ `ResetGraph(mode)` resets the GPU *and* builds libgpu's own environment
// block in the game's BSS. This runtime's GPU is synchronous, so there is no
// command list to flush -- but the block still has to exist, because the rest
// of libgpu reads it. Leaving it zeroed is not neutral: the 28 slots below are
// "empty" markers, and zero means "slot 0" everywhere instead.
//
// Reference is the game's own `ResetGraph`, run as recompiled MIPS and dumped:
// the block is 32 words -- an info word, a resolution word (0x02000400), an
// enable flag, a pad, then 28 words of 0xFFFFFFFF. Confirmed identical in the
// reference recompilation of the same binary at the same point in the run.
//
// The block's address is game BSS, so it comes from the TOML (`[timing]
// gpu_env_addr`); zero leaves this a no-op, which is the pre-existing
// behaviour for games that have not been mapped.
void hle_ResetGraph(recomp_context *ctx) {
  const uint32_t env = ps1::psyq::psyq_state().gpuEnvAddr;
  if (env == 0 || ctx->mem == nullptr)
    return;

  ctx->mem->write32(env + 0, 0x00000100u);  // GPU info / type
  ctx->mem->write32(env + 4, 0x02000400u);  // default resolution
  ctx->mem->write32(env + 8, 0x00000001u);  // enabled
  ctx->mem->write32(env + 12, 0x00000000u); // pad
  for (uint32_t i = 0; i < kGpuEnvFreeSlots; ++i)
    ctx->mem->write32(env + 16 + i * 4, 0xFFFFFFFFu);
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
// Audited 2026-08-06: DrawOTag(p) itself (psyz decomp/src/libgpu/sys.c:354-360)
// just forwards to `addque2(cwc, p, 0, 0)`, and _cwc (sys.c:850-855) programs
// DMA2 in "linked-list" mode (CHCR=0x01000401) with MADR=p -- on real
// hardware the GPU's DMA controller itself walks the OT and streams the
// words to GP0, the CPU does not decode headers at all. This function is
// this project's software model of that DMA-controller traversal, so the
// node format it must match is the DMA2 linked-list format, not sys.c
// (which never decodes it in software). That format is documented directly
// in psx-spx (docs/dmachannels.md:173-190, "Linked List DMA"):
//   bits 0-23  = address of next node (or all-1s end marker, 0xFFFFFF)
//   bits 24-31 = number of extra (data) words in this node
// and in docs/graphicsprocessingunitgpu.md:1084-1136 ("Depth Ordering
// Table"), whose worked example confirms only the N data words after the
// header are sent to GP0 -- the header word itself is link-chain metadata,
// never pushed as a GP0 command. Below already matches this exactly: header
// decode (wordCount = bits 31:24, next = bits 23:0), per-node GP0 pushes
// limited to the N words following the header, KSEG0-bit restore on the
// 24-bit physical next-pointer, and termination on next == 0xFFFFFF. No
// change was needed; see DrawOTagEmptyListEmitsNothing /
// DrawOTagSubmitsSinglePrimitive / DrawOTagTraversesChain /
// DrawOTagHeaderWordItselfIsNeverPushedToGp0 in test_psyq_hle.cpp.
//
void hle_DrawOTag(recomp_context *ctx) {
  if (!g_cfg.writeGP0) {
    ctx->r[V0] = 0;
    return;
  }
  ps1::metrics::count("draw_otag.calls");
  // `PS1_OT_DUMP=<n>`: on the nth call, histogram what the ordering table
  // actually contains.  Missing on-screen content is either absent from the
  // table (the builder never emitted it) or present but dropped by the walk,
  // and only the table's own contents tell the two apart.
  static const long otDumpAt = []() {
    const char *e = std::getenv("PS1_OT_DUMP");
    return (e && *e) ? std::strtol(e, nullptr, 10) : 0;
  }();
  static long otCall = 0;
  const bool otDump = (otDumpAt > 0 && ++otCall == otDumpAt);
  uint32_t otOps[256] = {};
  uint32_t otEmpty = 0;

  uint32_t ptr = ctx->r[A0];
  int safety = 0;
  while ((ptr & 0xFFFFFFu) != 0xFFFFFFu && safety++ < 100000) {
    uint32_t hdr = ctx->mem->read32(ptr);
    uint32_t wordCount = (hdr >> 24) & 0xFF;
    ps1::metrics::count("draw_otag.nodes");
    ps1::metrics::count("draw_otag.words", wordCount);
    if (otDump) {
      if (wordCount == 0)
        ++otEmpty;
      else
        ++otOps[ctx->mem->read32(ptr + 4) >> 24];
    }
    for (uint32_t i = 0; i < wordCount; i++) {
      g_cfg.writeGP0(ctx->mem->read32(ptr + 4 + i * 4));
    }
    uint32_t next = hdr & 0xFFFFFFu;
    if (next == 0xFFFFFFu)
      break;
    ptr = next | 0x80000000u; // restore KSEG0 bit
  }
  if (otDump) {
    fmt::print(stderr, "[OT] call#{} nodes={} empty={} safety_hit={}\n", otCall,
               safety, otEmpty, safety >= 100000);
    for (int i = 0; i < 256; ++i)
      if (otOps[i])
        fmt::print(stderr, "[OT]   op 0x{:02X} x{}\n", i, otOps[i]);
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
//     +8  screen.x (int16)  screen display range -- NOT unused: PutDispEnv
//     +10 screen.y (int16)  builds GP1(06)/(07) from these four fields, and
//     +12 screen.w (int16)  substitutes its own defaults (2560 / 240) when
//     +14 screen.h (int16)  w/h are zero (sys.c:415-418)
//     +16 isinter  (uint8)  interlace enable
//     +17 isrgb24  (uint8)  24bpp enable
//     +18 pad[2]
//
// Audited 2026-08-09 against the psyz decomp reference (workspace clone,
// PS1Recomp-workspace/psyz/decomp/src/libgpu/ext.c:72-86): all four `screen`
// fields are set to 0, `isrgb24`/`isinter`/`pad1`/`pad0` to 0, and `env` is
// returned. See the note in the body on why seeding `screen` with w/h was
// wrong.
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

// Faithful port of the psyz decomp's CLAMP macro
// (decomp/include/common.h:25): `#define CLAMP(x, min, max) x < min ? min
// : (x > max ? max : x)`. Unlike std::clamp, this ternary has no precondition
// on min <= max -- sys.c's own `CLAMP(h_end, h_start + 0x50, 3290)` can be
// called with min > max whenever h_start is itself already near its ceiling
// (reachable from screen.x >= 269, an in-range int16 read straight out of
// game RAM), which is well-defined here (returns min) but is undefined
// behavior under std::clamp. Match the macro's exact branch order instead.
inline int32_t sourceClamp(int32_t x, int32_t lo, int32_t hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}

// VRAM dimensions used by get_cs/get_ce's CLAMP (sys.c:634-651, "info.w"/
// "info.h"). ResetGraph (sys.c:159-175) sets these from two lookup tables
// indexed by info.version (sys.c:107-108):
//   D_800B89A8[] = {1024, 1024, 1024, 1024, 1024};  // info.w
//   D_800B89BC[] = {512,  1024, 1024, 512,  1024};  // info.h
// For info.version 0 or 3 (the retail/1MB-VRAM branch this project's
// SetDrawMode/PutDrawEnv audits already established as the applicable one),
// both indices give {1024, 512} -- the same numbers this runtime already
// carries as ps1::gpu::GPU::VRAM_WIDTH/VRAM_HEIGHT (gpu/gpu.h:106-107).
// Restated here as local literals rather than #include-ing gpu.h: this
// module talks to the GPU exclusively through the writeGP0/writeGP1
// callbacks in HleConfig, never through a direct type dependency, and two
// constants don't warrant breaking that.
constexpr int32_t kInfoW = 1024;
constexpr int32_t kInfoH = 512;
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
  // All four screen fields are zero, not the w/h just written into disp
  // (ext.c:72-86: `env->screen.x = 0; env->screen.y = 0; env->screen.w = 0;
  // env->screen.h = 0;`). PutDispEnv builds GP1(06)/(07) *from* screen and
  // substitutes its own defaults for a zero w/h (2560 tenths of a pixel /
  // 240 lines, sys.c:416-417), so seeding screen with w/h skips those
  // defaults: for a 320x240 setup the reference gets h_end = 608 + 2560 =
  // 3168, while w=320 gave 608 + 3200 = 3808, clamped down to 3290.
  env->screen_x = 0;
  env->screen_y = 0;
  env->screen_w = 0;
  env->screen_h = 0;
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
// Audited 2026-08-06, amended 2026-08-06 after review, against the psyz
// decomp reference (workspace clone,
// PS1Recomp-workspace/psyz/decomp/src/libgpu/sys.c:399-461). PutDispEnv
// branches on info.version (GPU hardware type); the same retail/1MB-VRAM
// branch this project's PutDrawEnv audit established applies here
// (info.version 0/3) is used below. It also branches on GetVideoMode()
// (env->pad0, NTSC=0/PAL=1) -- honoured explicitly via getVideoMode()
// rather than assumed NTSC, since libetc_SetVideoMode is reachable from PS1
// code (psyq_libgpu.cpp) and a PAL title would otherwise get silently wrong
// values from code whose whole purpose is source fidelity:
//
//   GP1(0x05) -- sys.c:409-412: (disp.y & 0x3FF) << 10 | (disp.x & 0x3FF).
//     The pre-audit code masked vx but not vy; an unmasked negative vy can
//     shift into bits 24-26 and corrupt the 0x05 command nibble itself.
//   GP1(0x06)/(0x07) -- sys.c:413-427: computed from DISPENV.screen (NOT
//     .disp), with defaults when screen.w/h == 0 and hard clamps that
//     depend on pad0 (v_start offset 0x10/0x13, v bounds 256,258/310,312).
//     The pre-audit code used a fabricated disp.w/h-based formula that
//     matched neither branch, and (pre-amendment) this project's own first
//     audit pass hardcoded the NTSC constants instead of reading pad0.
//     sys.c's own CLAMP(h_end, h_start + 0x50, 3290) call is reachable with
//     its own min argument (h_start + 0x50) exceeding its max (3290)
//     whenever screen.x >= 269 -- CLAMP's ternary (common.h:25) is
//     well-defined there (returns min, "bug-compatible" with the source);
//     see sourceClamp() above and the std::clamp-precondition finding this
//     amendment fixes.
//   GP1(0x08) -- sys.c:404, 428-457: bit layout matches psx-spx's documented
//     GP1(08h) fields (docs/graphicsprocessingunitgpu.md, "GP1(08h) -
//     Display mode" table at line 770-779, GPUSTAT mapping 897-905) --
//     NOTE: this is the correct citation; an earlier revision of this
//     comment cited "line 377+", which is GP0(E1h) Draw Mode, a different
//     register. The layout matches the same one gpu.cpp's case 0x08
//     consumer assumes for bits 0-5 (Hres1/Vres/isrgb24/isinter), but NOT
//     for bit 6 (Hres2/368-mode): gpu.cpp:418-428 maps GP1(08h) bits 0-5
//     onto GPUSTAT bits 17-22 correctly, then lands bit 6 on GPUSTAT bit 23
//     (Display Enable) instead of bit 16 -- a real but separate defect in
//     gpu.cpp, out of this function's scope, logged for follow-up rather
//     than fixed here (neither Crash nor Rayman uses 368-wide mode). The
//     pre-audit code put isrgb24 at bit5 and isinter at bit6 (both wrong)
//     and never set the Hres2 or video-mode bits.
//     info.reverse (SetGraphReverse) is not tracked by this runtime, so
//     that bit (0x80) is never set -- no game exercises it here.
//
void hle_PutDispEnv(recomp_context *ctx) {
  uint32_t envPtr = ctx->r[A0];
  // `DISPENV* PutDispEnv(DISPENV* env)` returns env (sys.c:399, 460). Set
  // before the writeGP1 guard below: the return value does not depend on
  // whether this runtime has a GP1 sink wired up.
  ctx->r[V0] = envPtr;
  if (!g_cfg.writeGP1) {
    return;
  }
  int16_t vx   = static_cast<int16_t>(ctx->mem->read16(envPtr + 0));
  int16_t vy   = static_cast<int16_t>(ctx->mem->read16(envPtr + 2));
  int16_t vw   = static_cast<int16_t>(ctx->mem->read16(envPtr + 4));
  int16_t vh   = static_cast<int16_t>(ctx->mem->read16(envPtr + 6));
  int16_t sx   = static_cast<int16_t>(ctx->mem->read16(envPtr + 8));
  int16_t sy   = static_cast<int16_t>(ctx->mem->read16(envPtr + 10));
  int16_t sw   = static_cast<int16_t>(ctx->mem->read16(envPtr + 12));
  int16_t sh   = static_cast<int16_t>(ctx->mem->read16(envPtr + 14));
  uint8_t isinter = ctx->mem->read8(envPtr + 16);
  uint8_t isrgb24 = ctx->mem->read8(envPtr + 17);
  bool pal = getVideoMode() != 0; // sys.c: env->pad0 = GetVideoMode()

  // GP1(0x05): display start address
  g_cfg.writeGP1(0x05000000u | ((static_cast<uint32_t>(vy) & 0x3FFu) << 10) |
                                 (static_cast<uint32_t>(vx) & 0x3FFu));

  // GP1(0x06)/(0x07): horizontal/vertical display range from DISPENV.screen.
  // v_start offset and v clamp bounds depend on pad0 (sys.c:414-422).
  int32_t hStart = static_cast<int32_t>(sx) * 10 + 0x260;
  int32_t hEnd   = hStart + (sw != 0 ? static_cast<int32_t>(sw) * 10 : 2560);
  int32_t vStart = static_cast<int32_t>(sy) + (pal ? 0x13 : 0x10);
  int32_t vEnd   = vStart + (sh != 0 ? static_cast<int32_t>(sh) : 240);
  hStart = sourceClamp(hStart, 500, 3290);
  hEnd   = sourceClamp(hEnd, hStart + 0x50, 3290);
  vStart = sourceClamp(vStart, 0x10, pal ? 310 : 256);
  vEnd   = sourceClamp(vEnd, vStart + 2, pal ? 312 : 258);
  g_cfg.writeGP1(0x06000000u | ((static_cast<uint32_t>(hEnd) & 0xFFFu) << 12) |
                                 (static_cast<uint32_t>(hStart) & 0xFFFu));
  g_cfg.writeGP1(0x07000000u | ((static_cast<uint32_t>(vEnd) & 0x3FFu) << 10) |
                                 (static_cast<uint32_t>(vStart) & 0x3FFu));

  // GP1(0x08): display mode
  uint32_t mode = 0;
  if (pal) mode |= 0x08u; // video mode (sys.c:434-436)
  if (isrgb24) mode |= 0x10u;
  if (isinter) mode |= 0x20u;
  if (vw <= 280) {
    // Hres1 = 0 (256)
  } else if (vw <= 352) {
    mode |= 0x01u; // Hres1 = 1 (320)
  } else if (vw <= 400) {
    mode |= 0x40u; // Hres2 = 1 (368)
  } else if (vw <= 560) {
    mode |= 0x02u; // Hres1 = 2 (512)
  } else {
    mode |= 0x03u; // Hres1 = 3 (640)
  }
  if (vh > (pal ? 288 : 256)) {
    mode |= 0x24u; // Vres (bit2) + interlace (bit5) combo for >threshold-line modes
  }
  g_cfg.writeGP1(0x08000000u | mode);
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
// Audited 2026-08-06, amended 2026-08-06 after review, against the psyz
// decomp reference (workspace clone,
// PS1Recomp-workspace/psyz/decomp/src/libgpu/ext.c:46-69):
//   env->tpage = getTPage(0, 0, 640, 0)       -- always this fixed value,
//     independent of the x/y/w/h args, evaluating (via this project's own
//     already-audited GetTPage formula, psyq_libgpu.cpp) to
//     (640 & 0x3FF) >> 6 = 0x0A.
//   env->dfe = video_mode ? h <= 288 : h <= 256 -- honoured explicitly via
//     getVideoMode() rather than assumed NTSC (amended after review: an
//     earlier revision of this comment asserted GetVideoMode() "defaults to
//     and stays NTSC/0" as an invariant, but libetc_SetVideoMode is
//     reachable from PS1 code -- see hle_libgpu_SetVideoMode,
//     psyq_libgpu.cpp -- so a PAL title would silently get a wrong dfe from
//     code whose whole purpose is source fidelity).
// The pre-audit implementation hardcoded tpage=0 and dfe=0, ignoring both
// GetTPage and the height threshold.
void hle_SetDefDrawEnv(recomp_context *ctx) {
  uint32_t envPtr = ctx->r[A0];
  int16_t  x  = static_cast<int16_t>(ctx->r[A1]);
  int16_t  y  = static_cast<int16_t>(ctx->r[A2]);
  int16_t  w  = static_cast<int16_t>(ctx->r[A3]);
  int16_t  h  = static_cast<int16_t>(ctx->mem->read32(ctx->r[SP] + 16));
  bool pal = getVideoMode() != 0; // ext.c: video_mode = GetVideoMode()

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
  // tpage = getTPage(0, 0, 640, 0) = 0x0A (ext.c:67), dtd=1 (dithering)
  ctx->mem->write16(envPtr + 20, 0x000Au);
  ctx->mem->write8(envPtr + 22, 1); // dtd
  // dfe = video_mode ? h <= 288 : h <= 256 (ext.c:60-64)
  ctx->mem->write8(envPtr + 23, h <= (pal ? 288 : 256) ? 1 : 0);
  // isbg (+24) and the background colour r0/g0/b0 (+25..+27) close out the
  // struct: ext.c:56-58 sets `env->r0 = 0; env->g0 = 0; env->b0 = 0;` and
  // ext.c:68 sets `env->isbg = 0`. Offsets from the DRAWENV layout
  // (chrono-cross-decomp/include/psyq/libgpu.h:361-371: isbg at :368 (+24),
  // r0/g0/b0 at :369 (+25..+27)), which is
  // the same layout this function's tpage/dtd/dfe writes above already
  // follow. Without these four bytes a stack-allocated DRAWENV keeps
  // whatever garbage was on the stack, so PutDrawEnv's isbg auto-clear tail
  // (sys.c:576-619) would fire on an uninitialised flag and paint an
  // uninitialised colour.
  ctx->mem->write8(envPtr + 24, 0); // isbg
  ctx->mem->write8(envPtr + 25, 0); // r0
  ctx->mem->write8(envPtr + 26, 0); // g0
  ctx->mem->write8(envPtr + 27, 0); // b0

  ctx->r[V0] = envPtr;
}

// PutDrawEnv
//
// PsyQ PutDrawEnv(env):
//   Applies a DrawEnv to the GPU by sending GP0 commands, in this order
//   (SetDrawEnv2, sys.c:561-575 -- see the gap 2/3 amendment below):
//     GP0(0xE3) -- drawing area top-left
//     GP0(0xE4) -- drawing area bottom-right
//     GP0(0xE5) -- drawing offset
//     GP0(0xE1) -- texture page (tpage)
//     GP0(0xE2) -- texture window
//     GP0(0xE6) -- mask bit setting
//
// Audited 2026-07-28 against the psyz decomp reference (workspace clone,
// PS1Recomp-workspace/psyz/decomp/src/libgpu/sys.c): PutDrawEnv builds its
// GP0(0xE1) word via SetDrawEnv2 -> get_mode(env->dfe, env->dtd, env->tpage)
// (sys.c:362-373, 561-573), the same get_mode() this project's SetDrawMode
// audit already confirmed (sys.c:624-632, retail/1MB-VRAM branch --
// info.version 0/3, applicable to this target per that audit):
//   (dtd ? 0xE1000200 : 0xE1000000) | (dfe ? 0x400 : 0) | (tpage & 0x9FF)
// The pre-audit implementation read only dtd and masked tpage with 0x7FF
// (bits 0-10) instead of 0x9FF (bits 0-8 + bit 11). Per psx-spx (GP0(E1h)),
// bits 9-10 are exclusively Dither/Draw-to-display-area, supplied by
// dtd/dfe -- never by the raw tpage value -- so 0x7FF let stray high bits of
// env->tpage leak into those flags, and env->dfe (DRAWENV +0x17, offset 23;
// chrono-cross-decomp/include/psyq/libgpu.h:361-371) was never read at all.
//
// Amended 2026-08-07 (Task 4b, gap 1/3) against the same reference:
// get_cs/get_ce (sys.c:634-651, retail branch) additionally CLAMP the
// coordinates to the VRAM extent before packing, and mask y with 0x3FF (10
// bits), not 0x1FF (9 bits):
//   x = CLAMP(x, 0, info.w - 1); y = CLAMP(y, 0, info.h - 1);
//   return 0xE3000000 | ((y & 0x3FF) << 10) | (x & 0x3FF);   // get_cs -> E3
//   return 0xE4000000 | ((y & 0x3FF) << 10) | (x & 0x3FF);   // get_ce -> E4
// info.w/info.h are 1024/512 on the retail/1MB-VRAM branch (sys.c:107-108,
// 170-171 -- see kInfoW/kInfoH above). The pre-amendment implementation did
// neither: an out-of-range clip rect leaked unclamped, wrongly-masked bits
// straight into the GP0 words. Note the mask half of this fix (0x3FF vs
// 0x1FF) has no separately observable effect once the clamp is also applied
// -- a clamped y can never exceed info.h-1 = 511 = 0x1FF, so both masks
// agree on every value the clamp can produce; it is fixed anyway because it
// is a real, source-confirmed divergence in its own right, and because an
// unclamped call path (there is none today, but nothing enforces that)
// would make the two masks disagree.
//
// Amended 2026-08-07 (Task 4b, gap 2/3): SetDrawEnv2 (sys.c:561-575) pushes
// its GP0 words in this order: get_cs -> E3, get_ce -> E4, get_ofs -> E5,
// get_mode -> E1, get_tw -> E2, literal 0xE6000000 -> E6. The pre-amendment
// implementation emitted E1 first (E1, E3, E4, E5); it now emits E3, E4, E5,
// E1, matching the source for the four words this function currently
// builds. On real hardware order matters here: each GP0 word takes effect
// as it arrives, so the draw mode (E1) is applied only once the clip area
// (E3/E4) is already set, not before.
//
// Amended 2026-08-07 (Task 4b, gap 3/3): SetDrawEnv2 always also emits
// GP0(0xE2) Texture Window (get_tw(&env->tw), sys.c:574, 662-673) and the
// literal GP0(0xE6) Mask Bit Setting (sys.c:575), which this implementation
// now does too, at the tail of the SetDrawEnv2 order established by gap 2.
// The pre-amendment implementation emitted neither.
void hle_PutDrawEnv(recomp_context *ctx) {
  uint32_t envPtr = ctx->r[A0];
  // `DRAWENV* PutDrawEnv(DRAWENV* env)` returns env (sys.c:362, 372). Set
  // before the writeGP0 guard below: the return value does not depend on
  // whether this runtime has a GP0 sink wired up.
  ctx->r[V0] = envPtr;
  if (!g_cfg.writeGP0) {
    return;
  }
  int16_t cx  = static_cast<int16_t>(ctx->mem->read16(envPtr + 0));
  int16_t cy  = static_cast<int16_t>(ctx->mem->read16(envPtr + 2));
  int16_t cw  = static_cast<int16_t>(ctx->mem->read16(envPtr + 4));
  int16_t ch  = static_cast<int16_t>(ctx->mem->read16(envPtr + 6));
  int16_t ox  = static_cast<int16_t>(ctx->mem->read16(envPtr + 8));
  int16_t oy  = static_cast<int16_t>(ctx->mem->read16(envPtr + 10));
  uint16_t tpage = ctx->mem->read16(envPtr + 20);
  uint8_t dtd = ctx->mem->read8(envPtr + 22);
  uint8_t dfe = ctx->mem->read8(envPtr + 23);

  // Emission order matches SetDrawEnv2 (sys.c:561-575): get_cs -> E3,
  // get_ce -> E4, get_ofs -> E5, get_mode -> E1 (get_tw -> E2 and the
  // literal E6 word are gap 3, tracked separately below). On real hardware
  // each GP0 word takes effect as it arrives, so this order means the draw
  // mode (E1) is applied only after the clip area (E3/E4) already is, not
  // before -- the pre-fix implementation emitted E1 first.

  // GP0(0xE3): drawing area top-left. sys.c:634-641 (get_cs, retail branch):
  //   x = CLAMP(x, 0, info.w - 1); y = CLAMP(y, 0, info.h - 1);
  //   return 0xE3000000 | ((y & 0x3FF) << 10) | (x & 0x3FF);
  int32_t csX = sourceClamp(cx, 0, kInfoW - 1);
  int32_t csY = sourceClamp(cy, 0, kInfoH - 1);
  g_cfg.writeGP0(0xE3000000u | ((static_cast<uint32_t>(csY) & 0x3FFu) << 10) |
                                 (static_cast<uint32_t>(csX) & 0x3FFu));

  // GP0(0xE4): drawing area bottom-right (inclusive). sys.c:643-651
  // (get_ce, retail branch) -- same CLAMP/mask shape as get_cs above.
  int16_t x2 = static_cast<int16_t>(cx + cw - 1);
  int16_t y2 = static_cast<int16_t>(cy + ch - 1);
  int32_t ceX = sourceClamp(x2, 0, kInfoW - 1);
  int32_t ceY = sourceClamp(y2, 0, kInfoH - 1);
  g_cfg.writeGP0(0xE4000000u | ((static_cast<uint32_t>(ceY) & 0x3FFu) << 10) |
                                 (static_cast<uint32_t>(ceX) & 0x3FFu));

  // GP0(0xE5): drawing offset
  g_cfg.writeGP0(0xE5000000u |
                 (static_cast<uint32_t>(static_cast<int32_t>(oy) & 0x7FF) << 11) |
                 static_cast<uint32_t>(static_cast<int32_t>(ox) & 0x7FF));

  // GP0(0xE1): texture page + dithering + draw-to-display-area enable
  uint32_t e1 = (dtd ? 0xE1000200u : 0xE1000000u) |
                (dfe ? 0x400u : 0u) | (tpage & 0x9FFu);
  g_cfg.writeGP0(e1);

  // GP0(0xE2): texture window. sys.c:662-673 (get_tw), called as
  // get_tw(&env->tw) -- always the non-NULL branch here, since env->tw is a
  // struct field, not a pointer:
  //   code[0] = (rect->x & 0xFF) >> 3;     code[2] = (-rect->w & 0xFF) >> 3;
  //   code[1] = (rect->y & 0xFF) >> 3;     code[3] = (-rect->h & 0xFF) >> 3;
  //   return 0xE2000000 | (code[1]<<15) | (code[0]<<10) | (code[3]<<5) |
  //          code[2];
  // env->tw is the RECT at DRAWENV offset +12 (x,y,w,h, each int16 -- the
  // same offset SetDefDrawEnv above already writes as all-zero "no
  // restriction"). Cross-checked against psx-spx's documented GP0(E2h) bit
  // layout (workspace clone PS1Recomp-workspace/psx-spx.github.io/docs/
  // graphicsprocessingunitgpu.md:406-414): bits 0-4 Mask X, 5-9 Mask Y,
  // 10-14 Offset X, 15-19 Offset Y -- matching code[2]@0-4, code[3]@5-9,
  // code[0]@10-14, code[1]@15-19 above.
  int32_t twX = static_cast<int16_t>(ctx->mem->read16(envPtr + 12));
  int32_t twY = static_cast<int16_t>(ctx->mem->read16(envPtr + 14));
  int32_t twW = static_cast<int16_t>(ctx->mem->read16(envPtr + 16));
  int32_t twH = static_cast<int16_t>(ctx->mem->read16(envPtr + 18));
  uint32_t twCode0 = (static_cast<uint32_t>(twX) & 0xFFu) >> 3;
  uint32_t twCode1 = (static_cast<uint32_t>(twY) & 0xFFu) >> 3;
  uint32_t twCode2 = (static_cast<uint32_t>(-twW) & 0xFFu) >> 3;
  uint32_t twCode3 = (static_cast<uint32_t>(-twH) & 0xFFu) >> 3;
  g_cfg.writeGP0(0xE2000000u | (twCode1 << 15) | (twCode0 << 10) |
                 (twCode3 << 5) | twCode2);

  // GP0(0xE6): mask bit setting. sys.c:575 -- SetDrawEnv2 always pushes this
  // literal, with no dependency on any DRAWENV field.
  g_cfg.writeGP0(0xE6000000u);

  // Background clear, sys.c:576-619: with env->isbg set, fill the clip rect
  // (w/h clamped to 0..info-1) with env->r0/g0/b0.  A 64-aligned x and w use
  // GP0(02h), which ignores the offset; anything else a GP0(60h) rectangle,
  // which applies it, so the rect is shifted back by env->ofs first.  Crash's
  // level-load card relies on it to clear the map frame behind it.
  if (ctx->mem->read8(envPtr + 24) != 0) {
    const uint32_t color = static_cast<uint32_t>(ctx->mem->read8(envPtr + 27)) << 16 |
                           static_cast<uint32_t>(ctx->mem->read8(envPtr + 26)) << 8 |
                           ctx->mem->read8(envPtr + 25);
    const uint16_t w = cw < 0 ? 0 : static_cast<uint16_t>(std::min<int>(cw, kInfoW - 1));
    const uint16_t h = ch < 0 ? 0 : static_cast<uint16_t>(std::min<int>(ch, kInfoH - 1));
    const bool aligned = ((cx & 0x3F) | (w & 0x3F)) == 0;
    const int16_t rx = aligned ? cx : static_cast<int16_t>(cx - ox);
    const int16_t ry = aligned ? cy : static_cast<int16_t>(cy - oy);
    g_cfg.writeGP0((aligned ? 0x02000000u : 0x60000000u) | color);
    g_cfg.writeGP0(static_cast<uint32_t>(static_cast<uint16_t>(ry)) << 16 |
                   static_cast<uint16_t>(rx));
    g_cfg.writeGP0(static_cast<uint32_t>(h) << 16 | w);
  }
}

} // namespace ps1::psyq
