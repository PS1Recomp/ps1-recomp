// ps1Runtime -- PS1 Hardware Simulation Runtime
// Full system integration: CPU, GPU, SPU, CD-ROM, DMA, Input, MDEC, Timers
// Game code runs in separate thread; main thread handles SDL events + rendering

#include "runtime/ps1_runtime_macros.h"
#include <SDL2/SDL.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <execinfo.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fmt/format.h>
#include <iostream>
#include <ps1recomp/elf_parser.h>
#include <runtime/bios/bios.h>
#include <runtime/psyq/psyq_hle.h>
#include <runtime/psyq/psyq_registry.h>
#include <runtime/psyq/psyq_state.h>
#include <runtime/cdrom/cdrom_controller.h>
#include <runtime/cdrom/virtual_fs.h>
#include <runtime/cpu_context.h>
#include <runtime/dma/dma.h>
#include <runtime/emuptr.h>
#include <runtime/gpu/gpu.h>
#include <runtime/gpu/renderer_opengl.h>
#include <algorithm>
#include <execinfo.h>
#include <pthread.h>
#include <runtime/input/input.h>
#include <runtime/mdec/mdec.h>
#include <runtime/memory.h>
#include <runtime/metrics.h>
#include <runtime/spu/spu.h>
#include <runtime/timers/timers.h>
#include <string>
#include <thread>
#include <toml.hpp>
#include <vector>

// SDL2 Audio Callback
static ps1::spu::SPU *g_spu = nullptr;

// ---------------------------------------------------------------------------
// Write guard (`PS1_WRITE_GUARD=<guest addr>[,<pages>]`)
//
// Makes a span of guest RAM read-only and reports the *host* stack of whoever
// writes it.  Unlike a polled canary or a printf probe, this costs nothing
// until a write to the span happens, so it does not perturb the timing of the
// bug it is hunting -- which matters here, because every active probe tried so
// far suppressed the corruption instead of catching it.
//
// N-shot.  The span is re-protected after each offending store, so legitimate
// writers no longer consume the whole instrument before the interesting one
// runs.  Re-arming works by letting the store retry with the x86 trap flag
// set: the store completes, the resulting SIGTRAP re-protects the span.
//
// Reports are deduplicated by faulting host PC, so a writer in a loop is
// reported once and counted thereafter.  Two budgets bound the cost:
//   PS1_WRITE_GUARD_SHOTS=<n>   distinct PCs to report with a stack (def. 8)
//   PS1_WRITE_GUARD_FAULTS=<n>  total faults before the guard gives up and
//                               unprotects for good (def. 20000)
//   PS1_WRITE_GUARD_BYTES=<n>   report only writes landing in the first <n>
//                               bytes from the requested address.  mprotect
//                               works a page at a time, so watching one word
//                               inside a busy page otherwise drowns in its
//                               neighbours' traffic (def. the whole span).
// ---------------------------------------------------------------------------
namespace {
uint8_t *g_guardBase = nullptr;
std::size_t g_guardLen = 0;
uint8_t *g_guardRamBase = nullptr;

constexpr int kGuardMaxPcs = 64;
struct GuardWriter {
  uintptr_t pc;
  uint32_t firstOffset;
  unsigned long count;
  uint32_t lastValue; // word left behind by this writer's most recent store
};
GuardWriter g_guardWriters[kGuardMaxPcs];
volatile sig_atomic_t g_guardWriterCount = 0;
volatile sig_atomic_t g_guardStepping = 0;
// Set by the SIGSEGV handler so the SIGTRAP handler, which runs once the
// store has actually completed, can read back what was written.
volatile sig_atomic_t g_guardPendingSlot = -1;
uint8_t *g_guardPendingAddr = nullptr;
unsigned long g_guardFaults = 0;
unsigned g_guardShotLimit = 8;
unsigned long g_guardFaultBudget = 20000;
unsigned g_guardShots = 0;
uint8_t *g_guardWatchLo = nullptr; // reported window inside the guarded span
uint8_t *g_guardWatchHi = nullptr;
unsigned long g_guardIgnored = 0;

// Re-protect the span once the offending store has completed.  The SIGSEGV
// handler sets the trap flag before returning, so exactly one guest
// instruction runs before this fires.
void guardSigtrap(int sig, siginfo_t * /*info*/, void *ctx) {
  if (!g_guardStepping) {
    signal(sig, SIG_DFL);
    return;
  }
  g_guardStepping = 0;
  auto *uc = static_cast<ucontext_t *>(ctx);
  uc->uc_mcontext.gregs[REG_EFL] &= ~static_cast<greg_t>(0x100);

  const int slot = g_guardPendingSlot;
  g_guardPendingSlot = -1;
  if (slot >= 0 && g_guardPendingAddr != nullptr) {
    uint32_t v;
    memcpy(&v, g_guardPendingAddr, sizeof v);
    g_guardWriters[slot].lastValue = v;
    // A guest pointer outside the 2 MB of main RAM cannot be a valid target,
    // so shout about it even when this writer's stack was already reported.
    const uint32_t phys = v & 0x1FFFFFFFu;
    if (v >= 0x80000000u && phys >= 0x200000u) {
      char buf[128];
      int n = snprintf(buf, sizeof buf,
                       "[WGUARD] !! valor fora da RAM: 0x%08X escrito em "
                       "0x8%07X por pc=%p\n",
                       v, static_cast<uint32_t>(g_guardPendingAddr - g_guardRamBase),
                       reinterpret_cast<void *>(g_guardWriters[slot].pc));
      ssize_t ignored = write(2, buf, n);
      (void)ignored;
    }
  }
  mprotect(g_guardBase, g_guardLen, PROT_READ);
}

// Census of everyone who wrote the span, in a form usable from a signal
// handler.  The run that matters is the one that crashes, and a crash never
// reaches the orderly shutdown path.
void guardCensusToStderr() {
  char buf[192];
  int n = snprintf(buf, sizeof buf,
                   "[WGUARD] censo: %d escritor(es), %lu falta(s), "
                   "%lu fora da janela\n",
                   static_cast<int>(g_guardWriterCount), g_guardFaults,
                   g_guardIgnored);
  ssize_t ignored = write(2, buf, n);
  for (int i = 0; i < g_guardWriterCount; i++) {
    n = snprintf(buf, sizeof buf,
                 "[WGUARD]   pc=%p 1a em 0x8%07X x%lu ultimo=0x%08X\n",
                 reinterpret_cast<void *>(g_guardWriters[i].pc),
                 g_guardWriters[i].firstOffset, g_guardWriters[i].count,
                 g_guardWriters[i].lastValue);
    ignored = write(2, buf, n);
  }
  (void)ignored;
}

void guardSigsegv(int sig, siginfo_t *info, void *ctx) {
  uint8_t *fault = static_cast<uint8_t *>(info->si_addr);
  if (fault < g_guardBase || fault >= g_guardBase + g_guardLen) {
    // Not ours -- this is the crash we are hunting, or an unrelated one.
    // Report what the guard saw before the default action kills us.
    guardCensusToStderr();
    signal(sig, SIG_DFL);
    return;
  }
  mprotect(g_guardBase, g_guardLen, PROT_READ | PROT_WRITE);

  auto *uc = static_cast<ucontext_t *>(ctx);
  const uintptr_t pc = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
  const uint32_t off = static_cast<uint32_t>(fault - g_guardRamBase);
  g_guardFaults++;

  if (fault < g_guardWatchLo || fault >= g_guardWatchHi) {
    // Same page, different word.  Count it and re-arm without reporting.
    g_guardIgnored++;
    if (g_guardFaults < g_guardFaultBudget) {
      uc->uc_mcontext.gregs[REG_EFL] |= static_cast<greg_t>(0x100);
      g_guardStepping = 1;
    }
    return;
  }

  int slot = -1;
  for (int i = 0; i < g_guardWriterCount; i++) {
    if (g_guardWriters[i].pc == pc) {
      slot = i;
      break;
    }
  }
  if (slot < 0 && g_guardWriterCount < kGuardMaxPcs) {
    slot = g_guardWriterCount;
    g_guardWriters[slot] = GuardWriter{pc, off, 0, 0};
    g_guardWriterCount = slot + 1;

    if (g_guardShots < g_guardShotLimit) {
      g_guardShots++;
      // write(2) and backtrace() are the async-signal-safe-ish pair; fmt and
      // printf are deliberately not used here.
      char buf[160];
      int n = snprintf(buf, sizeof buf,
                       "\n[WGUARD] #%u escrita em 0x8%07X (phys 0x%X) "
                       "pc=%p -- pilha do host:\n",
                       g_guardShots, off, off, reinterpret_cast<void *>(pc));
      ssize_t ignored = write(2, buf, n);
      (void)ignored;
      void *bt[32];
      int frames = backtrace(bt, 32);
      backtrace_symbols_fd(bt, frames, 2);
    }
  }
  if (slot >= 0) {
    g_guardWriters[slot].count++;
    g_guardPendingSlot = slot;
    g_guardPendingAddr = fault;
  }

  if (g_guardFaults >= g_guardFaultBudget) {
    const char msg[] = "[WGUARD] orcamento de faltas esgotado -- guarda desligada\n";
    ssize_t ignored = write(2, msg, sizeof msg - 1);
    (void)ignored;
    return; // leave the span writable
  }

  // Re-arm: let the store retry, then trap on the next instruction.
  uc->uc_mcontext.gregs[REG_EFL] |= static_cast<greg_t>(0x100);
  g_guardStepping = 1;
}

// Presented frames, guest VBlanks and wall time, side by side.
//
// The two clocks are separate: the render loop is paced by the compositor and
// has been measured between 32 and 54 frames per second during play, while the
// guest advances on its own 60 Hz pump.  Printing both is what turns "the game
// feels slow" into a number, and tells which of the two is behind.
std::chrono::steady_clock::time_point g_runStart;
// Hardware ticks actually issued: CDROM state machine plus the root counters.
std::atomic<uint64_t> g_hwTicks{0};

void reportClocks(uint64_t renderFrames) {
  using namespace std::chrono;
  const double secs =
      duration_cast<milliseconds>(steady_clock::now() - g_runStart).count() /
      1000.0;
  if (secs <= 0.0)
    return;
  const uint32_t vsyncs =
      ps1::psyq::psyq_state().vsyncCounter.load(std::memory_order_acquire);
  const uint64_t hw = g_hwTicks.load(std::memory_order_relaxed);
  fmt::print("[clocks] {:.1f}s  render {} ({:.1f}/s)  guest vblank {} ({:.1f}/s)"
             "  hw tick {} ({:.1f}/s)\n",
             secs, renderFrames, renderFrames / secs, vsyncs, vsyncs / secs, hw,
             hw / secs);
}

// Called at shutdown so a run that caught several writers still reports the
// full census, not only the ones whose stack fit in the shot budget.
void dumpWriteGuard() {
  if (g_guardBase == nullptr)
    return;
  mprotect(g_guardBase, g_guardLen, PROT_READ | PROT_WRITE);
  if (g_guardWriterCount == 0) {
    fmt::print(stderr,
               "[WGUARD] nenhuma escrita na janela ({} falta(s) na pagina)\n",
               g_guardFaults);
    return;
  }
  guardCensusToStderr();
}

// Arming is deferred to `PS1_WRITE_GUARD_AT=<vsync>` so that boot-time writes
// by legitimate owners do not consume the one-shot before the game reaches the
// state under investigation.
uint32_t g_guardArmAt = 0;
bool g_guardArmed = false;

void armWriteGuard() {
  if (g_guardArmed || g_guardBase == nullptr) return;
  g_guardArmed = true;
  if (mprotect(g_guardBase, g_guardLen, PROT_READ) != 0) {
    fmt::print(stderr, "[WGUARD] mprotect falhou ao armar\n");
    return;
  }
  fmt::print(stderr, "[WGUARD] armada em phys 0x{:X}..0x{:X}\n",
             static_cast<uint32_t>(g_guardBase - g_guardRamBase),
             static_cast<uint32_t>(g_guardBase - g_guardRamBase + g_guardLen));
}

void installWriteGuard(uint8_t *ramPtr) {
  const char *spec = std::getenv("PS1_WRITE_GUARD");
  if (!spec || !*spec) return;
  if (const char *at = std::getenv("PS1_WRITE_GUARD_AT"))
    g_guardArmAt = std::strtoul(at, nullptr, 10);
  if (const char *n = std::getenv("PS1_WRITE_GUARD_SHOTS"))
    g_guardShotLimit = std::strtoul(n, nullptr, 10);
  if (const char *n = std::getenv("PS1_WRITE_GUARD_FAULTS"))
    g_guardFaultBudget = std::strtoul(n, nullptr, 10);
  unsigned long watchBytes = 0;
  if (const char *n = std::getenv("PS1_WRITE_GUARD_BYTES"))
    watchBytes = std::strtoul(n, nullptr, 0);
  char *end = nullptr;
  const uint32_t guest = std::strtoul(spec, &end, 0);
  unsigned pages = 1;
  if (end && *end == ',') pages = std::strtoul(end + 1, nullptr, 0);
  if (pages == 0) pages = 1;

  const long ps = sysconf(_SC_PAGESIZE);
  const uint32_t phys = guest & 0x1FFFFFFFu;
  uint8_t *want = ramPtr + phys;
  uint8_t *aligned = reinterpret_cast<uint8_t *>(
      reinterpret_cast<uintptr_t>(want) & ~static_cast<uintptr_t>(ps - 1));

  g_guardRamBase = ramPtr;
  g_guardBase = aligned;
  g_guardLen = static_cast<std::size_t>(ps) * pages;
  if (watchBytes > 0) {
    g_guardWatchLo = want;
    g_guardWatchHi = want + watchBytes;
    if (g_guardWatchHi > g_guardBase + g_guardLen)
      g_guardWatchHi = g_guardBase + g_guardLen;
  } else {
    g_guardWatchLo = g_guardBase;
    g_guardWatchHi = g_guardBase + g_guardLen;
  }

  struct sigaction sa {};
  sa.sa_sigaction = guardSigsegv;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, nullptr);

  struct sigaction st {};
  st.sa_sigaction = guardSigtrap;
  st.sa_flags = SA_SIGINFO;
  sigemptyset(&st.sa_mask);
  sigaction(SIGTRAP, &st, nullptr);

  // The dispatcher aborts on an unmapped target, which is exactly the failure
  // this guard is usually chasing.  Print the census on the way out.
  struct sigaction sab {};
  sab.sa_handler = [](int) {
    guardCensusToStderr();
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
  };
  sigemptyset(&sab.sa_mask);
  sigaction(SIGABRT, &sab, nullptr);

  fmt::print(stderr, "[WGUARD] alvo phys 0x{:X}..0x{:X} ({} pagina(s)), "
                     "armar em vsync {}, {} tiro(s), {} falta(s), "
                     "janela 0x{:X}..0x{:X}\n",
             static_cast<uint32_t>(g_guardBase - ramPtr),
             static_cast<uint32_t>(g_guardBase - ramPtr + g_guardLen), pages,
             g_guardArmAt, g_guardShotLimit, g_guardFaultBudget,
             static_cast<uint32_t>(g_guardWatchLo - ramPtr),
             static_cast<uint32_t>(g_guardWatchHi - ramPtr));
  if (g_guardArmAt == 0)
    armWriteGuard();
}
} // namespace

// Forward declaration -- generated by ps1Recomp in recompiled_out.cpp
extern void recomp_init_dispatch_table();

static void audioCallback(void * /*userdata*/, uint8_t *stream, int len) {
  if (!g_spu) {
    std::memset(stream, 0, len);
    return;
  }
  uint32_t numSamples = len / (2 * sizeof(int16_t)); // stereo S16
  g_spu->generateSamples(reinterpret_cast<int16_t *>(stream), numSamples);
}

// SDL2 Key Mapping
// Keyboard to PS1 pad.  One table, so the startup banner and the event handler
// cannot drift apart -- the pad layout is the first thing anyone needs to know
// to play, and "which key is Start" has already cost a play session.
struct KeyBinding {
  SDL_Keycode key;
  uint16_t button;
  const char *keyName;
  const char *padName;
};

static const KeyBinding kPadBindings[] = {
    {SDLK_UP, ps1::input::BTN_UP, "Up", "D-Pad Up"},
    {SDLK_DOWN, ps1::input::BTN_DOWN, "Down", "D-Pad Down"},
    {SDLK_LEFT, ps1::input::BTN_LEFT, "Left", "D-Pad Left"},
    {SDLK_RIGHT, ps1::input::BTN_RIGHT, "Right", "D-Pad Right"},
    {SDLK_z, ps1::input::BTN_CROSS, "Z", "Cross"},
    {SDLK_x, ps1::input::BTN_CIRCLE, "X", "Circle"},
    {SDLK_a, ps1::input::BTN_SQUARE, "A", "Square"},
    {SDLK_s, ps1::input::BTN_TRIANGLE, "S", "Triangle"},
    {SDLK_q, ps1::input::BTN_L1, "Q", "L1"},
    {SDLK_w, ps1::input::BTN_R1, "W", "R1"},
    {SDLK_e, ps1::input::BTN_L2, "E", "L2"},
    {SDLK_r, ps1::input::BTN_R2, "R", "R2"},
    {SDLK_c, ps1::input::BTN_L3, "C", "L3"},
    {SDLK_v, ps1::input::BTN_R3, "V", "R3"},
    {SDLK_RETURN, ps1::input::BTN_START, "Enter", "Start"},
    {SDLK_RSHIFT, ps1::input::BTN_SELECT, "Right Shift", "Select"},
    {SDLK_BACKSPACE, ps1::input::BTN_SELECT, "Backspace", "Select"},
};

static void printPadBindings() {
  fmt::print("[pad] keyboard layout:\n");
  for (const auto &b : kPadBindings)
    fmt::print("[pad]   {:<12} -> {}\n", b.keyName, b.padName);
  fmt::print("[pad]   {:<12} -> {}\n", "Esc", "quit");
}

static void mapKeyToButton(SDL_Keycode key, ps1::input::InputController &input,
                           bool pressed) {
  for (const auto &b : kPadBindings) {
    if (b.key != key)
      continue;
    // Printed on every press: a button that does nothing in the game is a
    // different problem from a button that never reached the pad at all.
    if (pressed)
      fmt::print(stderr, "[pad] {} -> {}\n", b.keyName, b.padName);
    if (pressed)
      input.press(b.button, 0);
    else
      input.release(b.button, 0);
    return;
  }
}

// Constants
static constexpr uint32_t CPU_CLOCK = 33868800;              // 33.8688 MHz
static constexpr uint32_t CYCLES_PER_FRAME = CPU_CLOCK / 60; // ~564480
static constexpr uint32_t CYCLES_PER_SCANLINE = 3413; // ~3413 cycles per hblank
static constexpr uint32_t SCANLINES_PER_FRAME = 263;  // NTSC

// SIGTERM/SIGINT raises this so `tools/smoke_test.py` (and `timeout`) can
// stop the runtime gracefully -- giving the main loop a chance to dump VRAM
// before the OS reaps us.
// Stall sampler (PS1_STALL_SAMPLE)
// No gdb/perf here, but every recompiled MIPS function is a real C++ function
// with an exported symbol, so a backtrace taken on the game thread names the
// func_XXXXXXXX it is stuck in.  A watchdog thread pokes SIGPROF at it and
// prints what the handler captured.
static void *g_stallFrames[32];
static int g_stallDepth = 0;
static std::atomic<bool> g_stallCaptured{false};

static void stallSampler(int) {
  g_stallDepth = backtrace(g_stallFrames, 32);
  g_stallCaptured.store(true, std::memory_order_release);
}

static volatile std::sig_atomic_t g_shutdown_requested = 0;
static void onTerminate(int) { g_shutdown_requested = 1; }

// Serialize full 1024x512 VRAM (ABGR1555) as a P6 PPM at `path`.
// Used by tools/smoke_test.py as the regression contract for game bring-up.
static bool dumpVramPpm(const ps1::gpu::GPU &gpu, const char *path) {
  std::FILE *f = std::fopen(path, "wb");
  if (!f) {
    fmt::print(stderr, "[VRAM-DUMP] open failed: {}\n", path);
    return false;
  }
  constexpr uint32_t W = ps1::gpu::GPU::VRAM_WIDTH;
  constexpr uint32_t H = ps1::gpu::GPU::VRAM_HEIGHT;
  std::fprintf(f, "P6\n%u %u\n255\n", W, H);
  const ps1::gpu::Color16 *vram = gpu.getVRAM();
  std::vector<uint8_t> row(W * 3);
  uint64_t nonZero = 0;
  for (uint32_t y = 0; y < H; ++y) {
    for (uint32_t x = 0; x < W; ++x) {
      uint16_t p = vram[y * W + x].raw;
      row[x * 3 + 0] = static_cast<uint8_t>((p & 0x1F) << 3);
      row[x * 3 + 1] = static_cast<uint8_t>(((p >> 5) & 0x1F) << 3);
      row[x * 3 + 2] = static_cast<uint8_t>(((p >> 10) & 0x1F) << 3);
      if (p != 0)
        ++nonZero;
    }
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
  ps1::metrics::count("vram_dump.written");
  ps1::metrics::setState("vram_dump.nonzero_pixels",
                         static_cast<int64_t>(nonZero));
  fmt::print("[VRAM-DUMP] wrote {} ({}x{}), {} non-zero pixels\n", path, W, H,
             nonZero);
  return true;
}


// Follow goolobj->local (an nsentry) for the fields the GOOL render gate
// reads.  Returns 0 when the pointer is not plausible RAM, so a bad entry
// shows up as zeros rather than as a crash.
static uint32_t nsw(ps1::Memory &mem, uint32_t obj, uint32_t off) {
  const uint32_t local = mem.read32(obj + 32);
  if ((local & 0x1FFFFFFFu) >= 0x200000u)
    return 0;
  return mem.read32(local + off);
}
static uint32_t nsCategory(ps1::Memory &mem, uint32_t obj) {
  const uint32_t item0 = nsw(mem, obj, 16);
  if ((item0 & 0x1FFFFFFFu) >= 0x200000u)
    return 0xFFFFFFFFu;
  return mem.read32(item0 + 4);
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: ps1Runtime <ps1_elf_or_iso> [disc_image] [--config "
                 "game.toml]\n"
              << "       ps1Runtime --config game.toml\n";
    return 1;
  }

  std::signal(SIGTERM, onTerminate);
  std::signal(SIGINT, onTerminate);

  // Parse arguments: support --config flag anywhere
  std::string input_path;
  std::string explicit_disc_path;
  std::string config_path;
  for (int i = 1; i < argc; i++) {
    if (std::string(argv[i]) == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else if (input_path.empty()) {
      input_path = argv[i];
    } else if (explicit_disc_path.empty()) {
      explicit_disc_path = argv[i];
    }
  }

  // If only a TOML config is provided, read paths from it
  if (input_path.empty() && !config_path.empty()) {
    try {
      auto cfg = toml::parse(config_path);
      if (cfg.contains("disc")) {
        auto &disc = toml::find(cfg, "disc");
        if (disc.contains("exe_path"))
          input_path = toml::find<std::string>(disc, "exe_path");
        if (disc.contains("cue_path"))
          explicit_disc_path = toml::find<std::string>(disc, "cue_path");
      }
    } catch (const std::exception &e) {
      fmt::print(stderr, "Failed to parse config: {}\n", e.what());
      return 1;
    }
  }
  if (input_path.empty()) {
    std::cerr << "No ELF/EXE path provided (via argument or --config)\n";
    return 1;
  }
  fmt::print("ps1Runtime -- PS1 Hardware Simulation (Full Integration)\n");
  printPadBindings();

  // Initialize all hardware subsystems

  // Core
  ps1::Memory memory;
  ps1::gpu::GPU gpu;
  ps1::spu::SPU spu;
  ps1::DMA dma;
  ps1::InterruptController irqCtrl;
  ps1::Timers timers;
  ps1::input::InputController input;
  ps1::mdec::MDEC mdec;
  ps1::cdrom::CdromController cdromCtrl;
  ps1::cdrom::VirtualFs fs;

  // Bind emuptr<T> to this Memory instance so hand-written HLE bodies
  // (psyq_hle.cpp etc.) resolve PS1 addresses through the live RAM buffer.
  ps1::emuptr_set_ram(memory.ramPtr());

  // Wire Memory I/O routing
  memory.setGPU(&gpu);
  memory.setSPU(&spu);
  memory.setDMA(&dma);
  memory.setCDROM(&cdromCtrl);
  memory.setInput(&input);

  // Back both memory card slots with files, so a save survives the run.
  // Default location is `memcards/` beside the config; `[paths] memcard_dir`
  // overrides it.  A missing file is created from a freshly formatted card.
  {
    std::filesystem::path cardDir = "memcards";
    if (!config_path.empty()) {
      try {
        auto cfg = toml::parse(config_path);
        if (cfg.contains("paths")) {
          auto &paths = toml::find(cfg, "paths");
          if (paths.contains("memcard_dir"))
            cardDir = toml::find<std::string>(paths, "memcard_dir");
        }
      } catch (const std::exception &) {
        // Paths are optional; the default stands.
      }
    }
    std::error_code ec;
    std::filesystem::create_directories(cardDir, ec);
    for (int slot = 0; slot < 2; ++slot) {
      const auto file = cardDir / fmt::format("card{}.mcd", slot + 1);
      auto &card = input.getMemoryCard(slot);
      if (card.attachFile(file.string()))
        fmt::print("[MemCard] slot {} -> {}\n", slot + 1, file.string());
      else
        fmt::print(stderr, "[MemCard] slot {}: could not use {}\n", slot + 1,
                   file.string());
    }
  }
  memory.setMDEC(&mdec);
  memory.setTimers(&timers);
  memory.setInterruptController(&irqCtrl);

  // Wire DMA to devices
  dma.setMemory(&memory);
  dma.setGPU(&gpu);
  dma.setSPU(&spu);
  dma.setCDROM(&cdromCtrl);
  dma.setMDEC(&mdec);

  // Wire CDROM to VirtualFS
  cdromCtrl.attachVirtualFs(&fs);

  // Wire XA-ADPCM from CDROM -> SPU
  cdromCtrl.setXaCallback([&spu](const int16_t *samples, uint32_t count) {
    spu.pushXaSamples(samples, count);
  });

  // CPU Context
  recomp_context ctx;
  ctx.reset();
  ctx.mem = &memory;
  // Initialize Stack Pointer ($sp / r29) to top of 2MB Main RAM (KSEG0)
  ctx.r[29] = 0x801FFFF0;

  // BIOS
  ps1::bios::Bios bios(ctx, fs, memory);
  ctx.bios = &bios;
  bios.setGPU(&gpu);
  bios.setInputController(&input);
  bios.setCdromController(&cdromCtrl);
  bios.setDma(&dma);

  // Enable display (GP1 0x03, val=0): mirrors what the real BIOS does during boot.
  // reset() leaves bit 23 = 1 (display off) per hardware spec.
  gpu.writeGP1(0x03000000);

  // PsyQ HLE layer wiring
  // Phase 2 (2.1 .. 2.4) moved every per-game BSS address into the
  // process-wide `psyq_state()` singleton.  All HleConfig now carries are
  // the GP0/GP1 writers and a callback drain hook -- no addresses.
  {
    ps1::psyq::HleConfig hleCfg;
    hleCfg.drainCallbacks      = [&bios]() { bios.drainPendingCallbacks(); };
    hleCfg.writeGP0            = [&gpu](uint32_t w) { gpu.writeGP0(w); };
    hleCfg.writeGP1            = [&gpu](uint32_t w) { gpu.writeGP1(w); };
    hleCfg.deliverVBlankEvent  = [&bios]() { bios.triggerVBlankEvent(); };
    ps1::psyq::configure(hleCfg);
    psyq_registry_init_defaults();
    psyq_register_rayman_boot();
  }

  // Wire CDROM interrupt callback -> BIOS cross-thread queue (Phase 3.3).
  // The callback fires from `cdromCtrl.tick()` on the SDL render thread AND
  // from `cdromCtrl.writeRegister()` on the game thread -- calling
  // `triggerCdromEvent` directly from the SDL thread races on event-system
  // state with the game thread.  Push onto a mutex-protected queue instead;
  // `drainCdromEventQueue` pops and dispatches on the game thread (from
  // `drainPendingCallbacks` and `hle_libcd_CdSync`).
  cdromCtrl.setInterruptCallback(
      [&bios](uint8_t intType) { bios.queueCdromEvent(intType); });

  // Initialize SDL2

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
    fmt::print(stderr, "SDL_Init failed: {}\n", SDL_GetError());
    return 1;
  }

  // Initialize renderer (creates window + OpenGL context)
  ps1::gpu::RendererOpenGL renderer(gpu);
  if (!renderer.init("ps1Recomp")) {
    fmt::print(stderr, "Failed to initialize OpenGL renderer!\n");
    SDL_Quit();
    return 1;
  }

  // SDL2 Audio
  g_spu = &spu;
  SDL_AudioSpec desired{}, obtained{};
  desired.freq = 44100;
  desired.format = AUDIO_S16SYS;
  desired.channels = 2;
  desired.samples = 1024;
  desired.callback = audioCallback;

  SDL_AudioDeviceID audioDevice =
      SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
  if (audioDevice > 0) {
    SDL_PauseAudioDevice(audioDevice, 0); // Start playback
    fmt::print("[Audio] SDL2 audio: {}Hz, {} ch, {} samples/buffer\n",
               obtained.freq, obtained.channels, obtained.samples);
  } else {
    fmt::print(stderr, "[Audio] Warning: could not open audio: {}\n",
               SDL_GetError());
  }

  // SDL2 GameController
  SDL_GameController *gamepad = nullptr;
  for (int i = 0; i < SDL_NumJoysticks(); i++) {
    if (SDL_IsGameController(i)) {
      gamepad = SDL_GameControllerOpen(i);
      if (gamepad) {
        fmt::print("[Input] GameController: {}\n",
                   SDL_GameControllerName(gamepad));
        break;
      }
    }
  }

  // Load ELF or ISO

  ps1recomp::ElfParser parser;
  if (!parser.load(input_path)) {
    fmt::print(stderr, "Failed to load ELF: {}\n", parser.getError());
    renderer.destroy();
    SDL_Quit();
    return 1;
  }

  // Auto-load corresponding Disc Image if running a extracted .boot.exe
  if (!explicit_disc_path.empty()) {
    // Disc image was explicitly provided as first argument
    if (fs.loadDisc(explicit_disc_path)) {
      fmt::print("[CDROM] Mounted disc image: {}\n", explicit_disc_path);
    } else {
      fmt::print(stderr, "[CDROM] WARNING: Could not mount disc image: {}\n",
                 explicit_disc_path);
    }
  } else {
    std::string disc_path = input_path;
    if (disc_path.ends_with(".boot.exe")) {
      disc_path =
          disc_path.substr(0, disc_path.length() - 9); // strip ".boot.exe"
      if (fs.loadDisc(disc_path)) {
        fmt::print("[CDROM] Automatically mounted disc image: {}\n", disc_path);
      } else {
        fmt::print(
            stderr,
            "[CDROM] WARNING: Could not mount corresponding disc image: {}\n",
            disc_path);
      }
    } else if (disc_path.ends_with(".bin") || disc_path.ends_with(".cue")) {
      if (fs.loadDisc(disc_path)) {
        fmt::print("[CDROM] Mounted disc image: {}\n", disc_path);
      }
    }
  }

  auto sections = parser.getSections();
  int loaded_sections = 0;
  for (const auto &sec : sections) {
    if ((sec.type == ps1recomp::SectionType::Text ||
         sec.type == ps1recomp::SectionType::Data) &&
        sec.data != nullptr) {
      uint32_t phys = ps1::Memory::toPhysical(sec.vaddr);
      if (phys < ps1::Memory::RAM_SIZE) {
        std::memcpy(memory.ramPtr() + phys, sec.data, sec.size);
        loaded_sections++;
      }
    }
  }
  fmt::print("Loaded {} sections into PS1 memory.\n", loaded_sections);

  // Apply [memory_init] pre-initialization
  // Some games need specific PS1 RAM locations pre-initialized before the
  // game code runs (e.g., PsyQ BSS pointer tables that CdInit would normally
  // set up but can't due to bootstrap timing in the recompiled environment).
  if (!config_path.empty()) {
    try {
      auto cfg = toml::parse(config_path);
      if (cfg.contains("memory_init")) {
        auto &mi = toml::find(cfg, "memory_init");
        if (mi.is_table()) {
          for (const auto &[key, val] : mi.as_table()) {
            uint32_t addr = std::strtoul(key.c_str(), nullptr, 0);
            uint32_t data = 0;
            if (val.is_integer()) {
              data = static_cast<uint32_t>(val.as_integer());
            } else if (val.is_string()) {
              data = std::strtoul(val.as_string().c_str(), nullptr, 0);
            }
            uint32_t phys = ps1::Memory::toPhysical(addr);
            if (phys + 4 <= ps1::Memory::RAM_SIZE) {
              memory.write32(addr, data);
              fmt::print("[memory_init] 0x{:08X} = 0x{:08X}\n", addr, data);
            } else {
              fmt::print(stderr,
                         "[memory_init] WARN: address 0x{:08X} out of RAM range\n",
                         addr);
            }
          }
        }
      }
    } catch (const std::exception &e) {
      fmt::print(stderr, "[WARN] Failed to read memory_init from config: {}\n",
                 e.what());
    }
  }

  // Main Emulation Loop
  // Game code runs in a separate thread (it never returns).
  // Main thread handles SDL events + hardware ticking + rendering.

  uint32_t entryPoint = parser.getEntryPoint();
  fmt::print("Starting emulation... Entry Point: 0x{:08X}\n", entryPoint);

  // DEBUG: verify binary data integrity at known vtable addresses
  {
    uint32_t dbgAddrs[] = {0x800549BC, 0x8005499C, 0x800549A4, 0x800549A8};
    for (auto a : dbgAddrs) {
      uint32_t phys = ps1::Memory::toPhysical(a);
      if (phys + 4 <= ps1::Memory::RAM_SIZE) {
        uint32_t val = memory.read32(a);
        fmt::print("[DBG] 0x{:08X} = 0x{:08X}\n", a, val);
      }
    }
  }
  std::atomic<bool> gameRunning{true};
  // `gameFinished` = "shutdown requested" (set by main or game thread)
  // `gameThreadDone` = "game thread lambda has returned" (set only by game
  // thread). Splitting these avoids the UAF in shutdown: previously the
  // cleanup code set `gameFinished` to request shutdown, then waited
  // `while (!gameFinished)` -- which already returned true the instant after
  // the request -- so the wait exited immediately and the still-running
  // game thread was detach()ed, leaving it racing with `Bios::~Bios()`
  // destructing `cdEventQueue_`.  See ISSUES.md #1.
  std::atomic<bool> gameFinished{false};
  std::atomic<bool> gameThreadDone{false};
  uint64_t frameCount = 0;
  // Set once PS1_VRAM_DUMP_FRAME has been honoured, so the shutdown dump does
  // not overwrite the requested frame -- both write PS1_VRAM_DUMP_PATH, and
  // the shutdown VRAM is exactly the freshly-cleared state we are avoiding.
  bool frameDumpWritten = false;

  const uint32_t shotEvery =
      std::getenv("PS1_SHOT_EVERY")
          ? std::strtoul(std::getenv("PS1_SHOT_EVERY"), nullptr, 10)
          : 0;
  const uint32_t shotFrom =
      std::getenv("PS1_SHOT_FROM")
          ? std::strtoul(std::getenv("PS1_SHOT_FROM"), nullptr, 10)
          : 0;
  const std::string shotDir =
      std::getenv("PS1_SHOT_DIR") ? std::getenv("PS1_SHOT_DIR") : "/tmp/shots";
  uint32_t lastShotBucket = 0xFFFFFFFFu;

  uint32_t censusFrom = 0, censusTo = 0;
  bool censusStarted = false, censusWritten = false;
  if (const char *c = std::getenv("PS1_CENSUS")) {
    censusFrom = std::strtoul(c, nullptr, 10);
    if (const char *colon = std::strchr(c, ':'))
      censusTo = std::strtoul(colon + 1, nullptr, 10);
  }

  // Initialize dispatch table before starting the game
  recomp_init_dispatch_table();
  fmt::print("[Dispatch] Table initialized.\n");

  // BSS mirror addresses (per-game polling slots)
  // Phase 2.2 retired the per-game VBlank counter BSS slot in favor of
  // `psyq_state().vsyncCounter` (atomic).  PsyQ HLE reads the singleton.
  // BUT: recompiled native MIPS code can poll the legacy BSS address
  // directly (`MEM_READ32(0x801CF2CC)` in Rayman) -- without a write-
  // through, those polls observe 0 forever and the game thread spins.
  //
  // `[bss_mirrors]` (TOML, optional) declares which BSS slots the VBlank
  // ticker should mirror.  When set, the thread does an atomic
  // `vsyncCounter` bump AND a `mem.write32(addr, counter)` so legacy
  // polling continues to work with the current architecture.
  uint32_t vblankCounterMirror = 0;
  uint32_t cdSyncMirror = 0;
  uint32_t cdReadyMirror = 0;
  if (!config_path.empty()) {
    try {
      auto cfg = toml::parse(config_path);
      if (cfg.contains("bss_mirrors")) {
        auto &mirrors = toml::find(cfg, "bss_mirrors");
        if (mirrors.is_table()) {
          auto readAddr = [&](const char *key) -> uint32_t {
            if (!mirrors.contains(key))
              return 0;
            auto v = toml::find(mirrors, key);
            if (v.is_string())
              return std::strtoul(v.as_string().c_str(), nullptr, 0);
            if (v.is_integer())
              return static_cast<uint32_t>(v.as_integer());
            return 0;
          };
          vblankCounterMirror = readAddr("vblank_counter");
          cdSyncMirror = readAddr("cd_sync_byte");
          cdReadyMirror = readAddr("cd_ready_byte");
          if (vblankCounterMirror != 0)
            fmt::print("[bss_mirrors] vblank_counter = 0x{:08X}\n",
                       vblankCounterMirror);
          if (cdSyncMirror != 0)
            fmt::print("[bss_mirrors] cd_sync_byte   = 0x{:08X}\n",
                       cdSyncMirror);
          if (cdReadyMirror != 0)
            fmt::print("[bss_mirrors] cd_ready_byte  = 0x{:08X}\n",
                       cdReadyMirror);
        }
      }
    } catch (const std::exception &) {
      // Optional section -- silent fallback.
    }

    // `[timing]` (TOML, optional): games that drive their frame delta off a
    // root counter register an IRQ callback that increments a word in their
    // own BSS.  This runtime models interrupts cooperatively and never fires
    // it, so the word stays frozen and every time-driven wait crawls.  These
    // two keys let the VSync HLE advance it at the right rate instead.
    try {
      auto cfg = toml::parse(config_path);
      if (cfg.contains("timing")) {
        auto &t = toml::find(cfg, "timing");
        auto readU32 = [&](const char *key) -> uint32_t {
          if (!t.contains(key))
            return 0;
          auto v = toml::find(t, key);
          if (v.is_string())
            return std::strtoul(v.as_string().c_str(), nullptr, 0);
          if (v.is_integer())
            return static_cast<uint32_t>(v.as_integer());
          return 0;
        };
        auto &st = ps1::psyq::psyq_state();
        st.rcntTickAddr = readU32("rcnt_tick_addr");
        st.rcntTicksPerVBlank = readU32("rcnt_ticks_per_vblank");
        st.gpuEnvAddr = readU32("gpu_env_addr");
        st.intrStackTop = readU32("intr_stack_top");
        if (st.intrStackTop != 0)
          fmt::print("[timing] interrupt stack top at 0x{:08X}\n", st.intrStackTop);
        if (st.gpuEnvAddr != 0)
          fmt::print("[timing] libgpu env block at 0x{:08X}\n", st.gpuEnvAddr);
        if (st.rcntTickAddr != 0 && st.rcntTicksPerVBlank != 0)
          fmt::print("[timing] rcnt tick 0x{:08X} += {} per VBlank\n",
                     st.rcntTickAddr, st.rcntTicksPerVBlank);
      }
    } catch (const std::exception &) {
      // Optional section -- silent fallback.
    }
  }
  bios.setBssMirrors(cdSyncMirror, cdReadyMirror);

  // VBlank ticker thread
  // The game spins polling psyq_state().vsyncCounter (and PsyQ HLE wraps
  // VSync to read the same singleton).  This dedicated 60 Hz thread is
  // independent of SDL rendering, so the tick advances at the right rate
  // and the game's VSync wait exits quickly.
  //
  // Phase 3.2: this thread no longer calls `bios.triggerVBlankEvent()`
  // directly.  That call writes non-atomic state -- `drawSync.status[]`,
  // event-system flags, queued swap callback -- which was racing with the
  // game thread.  Instead we just bump the atomic counter and raise the
  // `vblankPending` flag; `hle_VSync`, running on the game thread, drains
  // the flag at the end of its wait and runs `triggerVBlankEvent` there.
  // `gpu.snapshotDisplayBuffer` and `bios.updatePadBuffers` stay here --
  // they own their own thread safety (renderer double-buffer / atomic
  // pad state) and are out of scope for 3.2.
  //
  // BSS mirror (`vblankCounterMirror`): when set via `[bss_mirrors]`, also
  // write-through to a PS1 RAM address so recompiled MIPS code that polls
  // the legacy slot directly keeps working -- see comment block above.
  const bool watchGlobals = std::getenv("PS1_WATCH_GLOBALS") != nullptr;
  const uint32_t autoStartVsync =
      std::getenv("PS1_AUTO_START")
          ? std::strtoul(std::getenv("PS1_AUTO_START"), nullptr, 10)
          : 0;
  const uint32_t autoStartHold =
      std::getenv("PS1_AUTO_START_HOLD")
          ? std::strtoul(std::getenv("PS1_AUTO_START_HOLD"), nullptr, 10)
          : 8;
  // `PS1_AUTO_CROSS=<period>` taps X every <period> VBlanks. Menus advance on
  // X, so an unattended run otherwise sits on the first screen that waits for
  // it -- which reads as a hang but is the game doing exactly what it should.
  const uint32_t autoCrossEvery =
      std::getenv("PS1_AUTO_CROSS")
          ? std::strtoul(std::getenv("PS1_AUTO_CROSS"), nullptr, 10)
          : 0;
  // `PS1_METRICS=<period>` prints a per-<period>-VBlank delta panel: how many
  // yield points the recompiled code hit, how many reached the drain slow
  // path, how many were refused as re-entrant, and where dispatched callback
  // time actually went.  One run answers "is the drain the bottleneck", "is a
  // callback being dispatched in a burst" and "which callback is slow" at
  // once, instead of one hypothesis per rebuild.
  // Named apart from PS1_METRICS, which metrics.cpp already reads as the
  // output path for the shutdown JSON.
  const bool inputTrace = std::getenv("PS1_INPUT_TRACE") != nullptr;
  const bool canary = std::getenv("PS1_CANARY") != nullptr;
  // The globals the corruption hits live around 0x80060000; grab 16 KB there.
  constexpr uint32_t kCanaryBase = 0x80054000u;
  constexpr uint32_t kCanarySize = 0x10000u;
  const uint32_t metricsEvery =
      std::getenv("PS1_DRAIN_METRICS")
          ? std::strtoul(std::getenv("PS1_DRAIN_METRICS"), nullptr, 10)
          : 0;
  // `PS1_INPUT_SCRIPT="600:down,660:down,720:cross"` -- press a button at a
  // given VBlank and release it 8 VBlanks later.  Replaces hand-timed manual
  // testing: a scripted run is repeatable, so a screenshot taken after a press
  // is evidence about that press rather than about when a human hit the key.
  struct ScriptedPress {
    uint32_t atVsync;
    uint16_t button;
  };
  std::vector<ScriptedPress> inputScript;
  if (const char *scriptEnv = std::getenv("PS1_INPUT_SCRIPT")) {
    using namespace ps1::input;
    const std::pair<const char *, uint16_t> names[] = {
        {"up", BTN_UP},          {"down", BTN_DOWN},
        {"left", BTN_LEFT},      {"right", BTN_RIGHT},
        {"cross", BTN_CROSS},    {"circle", BTN_CIRCLE},
        {"square", BTN_SQUARE},  {"triangle", BTN_TRIANGLE},
        {"start", BTN_START},    {"select", BTN_SELECT},
        {"l1", BTN_L1},          {"r1", BTN_R1},
    };
    std::string spec(scriptEnv);
    std::size_t pos = 0;
    while (pos < spec.size()) {
      std::size_t comma = spec.find(',', pos);
      if (comma == std::string::npos)
        comma = spec.size();
      const std::string item = spec.substr(pos, comma - pos);
      pos = comma + 1;
      const std::size_t colon = item.find(':');
      if (colon == std::string::npos)
        continue;
      const uint32_t at = std::strtoul(item.c_str(), nullptr, 10);
      const std::string name = item.substr(colon + 1);
      for (const auto &n : names) {
        if (name == n.first) {
          inputScript.push_back({at, n.second});
          fmt::print(stderr, "[script] vsync={} press {}\n", at, name);
          break;
        }
      }
    }
  }
  // VBlank tick -- runs on the GAME thread, driven from its yield points.
  //
  // This used to be a 60 Hz thread of its own.  That put the host clock, the
  // pad-buffer refresh and the VRAM snapshot on a different thread from the
  // recompiled code, and the resulting race corrupted ~180 KB of guest globals
  // in a single frame in roughly one run out of four.  It was a Heisenbug:
  // adding any work at all to that thread made it vanish, which is why every
  // probe aimed at it came back clean.  RecompOne -- the reference runtime that
  // reaches gameplay -- is single-threaded for the same reason.
  //
  // Called from `Bios::drainPendingCallbacks` (every ~1024 yield points) and
  // from the VSync wait, so the clock still advances inside spin-waits that
  // never call VSync, such as the NSF loader.
  auto vblankTick = [&]() {
    using namespace std::chrono;
    static auto next = steady_clock::now();
    const auto now = steady_clock::now();
    if (now < next)
      return;
    next = now + microseconds(16667); // ~60 Hz

    // Hardware tick, paced by the wall clock rather than by presentation.
    g_hwTicks.fetch_add(1, std::memory_order_relaxed);
    cdromCtrl.tick(CYCLES_PER_FRAME);

    uint32_t timerIrqs = 0;
    for (uint32_t scanline = 0; scanline < SCANLINES_PER_FRAME; scanline++)
      timerIrqs |= timers.tick(CYCLES_PER_SCANLINE, true, false);
    if (timerIrqs & ps1::IRQ_TMR0)
      irqCtrl.raiseInterrupt(ps1::IRQ_TMR0);
    if (timerIrqs & ps1::IRQ_TMR1)
      irqCtrl.raiseInterrupt(ps1::IRQ_TMR1);
    if (timerIrqs & ps1::IRQ_TMR2)
      irqCtrl.raiseInterrupt(ps1::IRQ_TMR2);
    irqCtrl.raiseInterrupt(ps1::IRQ_VBLANK);
    // The CDROM event is raised by the interrupt callback from pushResponse;
    // only the hardware line is asserted here.
    if (cdromCtrl.hasInterrupt())
      irqCtrl.raiseInterrupt(ps1::IRQ_CDROM);
    if (dma.hasInterrupt())
      irqCtrl.raiseInterrupt(ps1::IRQ_DMA);
    if (input.hasInterrupt()) {
      irqCtrl.raiseInterrupt(ps1::IRQ_PAD_MC);
      input.clearInterrupt();
    }
    if (spu.hasIrq()) {
      irqCtrl.raiseInterrupt(ps1::IRQ_SPU);
      spu.clearIrq();
    }

    {
      uint32_t newCount = ps1::psyq::psyq_state().vsyncCounter.fetch_add(
          1, std::memory_order_release) + 1;
      ps1::psyq::psyq_state().vblankPending.store(
          true, std::memory_order_release);
      if (vblankCounterMirror != 0) {
        memory.write32(vblankCounterMirror, newCount);
      }
      if (autoStartVsync != 0) {
        if (newCount == autoStartVsync) {
          input.press(ps1::input::BTN_START, 0);
          fmt::print(stderr, "[auto] START press @vsync={}\n", newCount);
        } else if (newCount == autoStartVsync + autoStartHold) {
          input.release(ps1::input::BTN_START, 0);
          fmt::print(stderr, "[auto] START release @vsync={}\n", newCount);
        }
      }
      for (const auto &p : inputScript) {
        if (newCount == p.atVsync) {
          input.press(p.button, 0);
          fmt::print(stderr, "[script] vsync={} down 0x{:04X}\n", newCount,
                     p.button);
        } else if (newCount == p.atVsync + 8) {
          input.release(p.button, 0);
        }
      }
      if (autoCrossEvery != 0) {
        const uint32_t phase = newCount % autoCrossEvery;
        if (phase == 0)
          input.press(ps1::input::BTN_CROSS, 0);
        else if (phase == 8)
          input.release(ps1::input::BTN_CROSS, 0);
      }
      if (g_guardArmAt != 0 && !g_guardArmed && newCount >= g_guardArmAt)
        armWriteGuard();
      if (watchGlobals) {
        // GOOL actor state, named from the c1c decompilation's absolute
        // addresses: whether the Crash object exists at all, how many objects
        // are live, and how many the renderer accepted this frame.
        // goolobj field offsets from the c1c decompilation's struct.
        const uint32_t co = memory.read32(0x800566B4u);
        if (co) {
          fmt::print(stderr,
                     "[crash] state={} statusa=0x{:08X} statusb=0x{:08X} "
                     "statusc=0x{:08X} trans=({},{},{}) scale=({},{},{}) "
                     "animseq=0x{:08X} animframe={} displaymode=0x{:X} "
                     "entity=0x{:08X} zindex={} globanimflags=0x{:08X} src189C=0x{:08X} stateflags=0x{:08X} local=0x{:08X} "
                     "nsmagic=0x{:08X} nstype={} items={} item0=0x{:08X} category=0x{:X} "
                     "execanims=0x{:08X} animidx={} pc=0x{:08X} nsid=0x{:08X}\n",
                     memory.read32(co + 44), memory.read32(co + 200),
                     memory.read32(co + 204), memory.read32(co + 208),
                     (int32_t)memory.read32(co + 128),
                     (int32_t)memory.read32(co + 132),
                     (int32_t)memory.read32(co + 136),
                     (int32_t)memory.read32(co + 152),
                     (int32_t)memory.read32(co + 156),
                     (int32_t)memory.read32(co + 160),
                     memory.read32(co + 264), memory.read32(co + 268),
                     memory.read32(co + 296), memory.read32(co + 272),
                     memory.read32(co + 312), memory.read32(0x800618B0u),
                     memory.read32(0x8006189Cu), memory.read32(co + 288), memory.read32(co + 32),
                     // nsentry: magic, id, type, itemcount, items[]; the
                     // render gate reads category from items[0]+4.
                     nsw(memory, co, 0), nsw(memory, co, 8), nsw(memory, co, 12),
                     nsw(memory, co, 16), nsCategory(memory, co),
                     // items[5] is the exec's animation table; the render gate
                     // indexes it with the ChangeAnim instruction's anim field.
                     nsw(memory, co, 16 + 5 * 4),
                     (memory.read32(co + 264) - nsw(memory, co, 16 + 5 * 4)) / 4,
                     memory.read32(co + 224), nsw(memory, co, 4));
        }
        fmt::print(stderr,
                   "[lvl] nextlevelid={} zone=0x{:08X} pad=0x{:08X}\n",
                   (int32_t)memory.read32(0x80056714u),
                   memory.read32(0x80057914u), memory.read32(0x8005E71Cu));
        fmt::print(stderr,
                   "[watch] vsync={} ticks={} frames_elapsed={} vblank={} "
                   "title_state={} pad0=0x{:08X} raw=0x{:04X} "
                   "exit=0x{:08X} p={:08X} p32={:04X}\n",
                   newCount, memory.read32(0x80034520u),
                   memory.read32(0x80060E04u), memory.read32(0x800549F0u),
                   memory.read32(0x800618D4u), memory.read32(0x8005E71Cu),
                   input.buttonState(0), memory.read32(0x80061994u),
                   memory.read32(0x8005791Cu),
                   memory.read32(0x8005791Cu)
                       ? memory.read16(memory.read32(0x8005791Cu) + 32)
                       : 0xFFFF);
      }
      // `PS1_INPUT_TRACE=1`: log only on change, so a human play session
      // produces a short readable record of what the game actually received
      // for each key press instead of thousands of identical lines.
      if (inputTrace) {
        const uint16_t raw = input.buttonState(0);
        const uint32_t gamePad = memory.read32(0x8005E71Cu);
        const uint32_t st = memory.read32(0x800618D4u);
        const int32_t nextLevel = (int32_t)memory.read32(0x80056714u);
        static uint16_t prevRaw = 0xFFFF;
        static uint32_t prevGamePad = 0, prevSt = 0xFFFFFFFFu;
        static int32_t prevNext = 0x7FFFFFFF;
        if (raw != prevRaw || gamePad != prevGamePad || st != prevSt ||
            nextLevel != prevNext) {
          fmt::print(stderr,
                     "[input] vsync={} tecla_raw=0x{:04X} pad[0]=0x{:08X} "
                     "pad[4]=0x{:08X} pad[8]=0x{:08X} pad[12]=0x{:08X} "
                     "title_state={} nextlevelid={} dono_carga=0x{:08X} "
                     "fila=0x{:08X}/0x{:08X} audio=[{},{},0x{:08X}] slot_som=0x{:08X}\n",
                     newCount, raw, gamePad, memory.read32(0x8005E720u),
                     memory.read32(0x8005E724u), memory.read32(0x8005E728u),
                     st, nextLevel,
                     // 0x8005CFB4 is the load pipeline's owner slot: state 10
                     // waits for it to read 0, state 11 waits for it to name
                     // its own request.  A stale owner deadlocks every load.
                     memory.read32(0x8005CFB4u), memory.read32(0x8005CFA8u),
                     memory.read32(0x8005CFACu), memory.read32(0x8005594Cu),
                     memory.read32(0x80055914u), memory.read32(0x800559A0u),
                     // 0x80055918 is the sound-tick's callback slot.  While it
                     // is zero the tick delivers Event(0xF0000009,0x20), which
                     // is what the level loader waits on; once the game
                     // registers a handler there the tick calls that instead
                     // and the event stops arriving.
                     memory.read32(0x80055918u));
          prevRaw = raw;
          prevGamePad = gamePad;
          prevSt = st;
          prevNext = nextLevel;
        }
      }
      // `PS1_CANARY=1`: the boot instability corrupts ~180 KB of globals
      // between two consecutive VBlanks with no bulk transfer to blame, so
      // catching the value is not enough -- we need the *shape*.  Keep a
      // rolling copy of the globals region and, the first time the game state
      // goes obviously wrong, write the previous and current copies out.
      // Recognisable data in the "after" copy means something was written to
      // the wrong place; noise means code ran wild.
      if (canary) {
        static std::vector<uint8_t> prev(kCanarySize), cur(kCanarySize);
        static bool primed = false, fired = false;
        for (uint32_t i = 0; i < kCanarySize; ++i)
          cur[i] = memory.read8(kCanaryBase + i);
        // Trigger on the fill pattern itself: the level load overwrites the
        // globals with a repeating word, so a known-small global holding it is
        // an unambiguous "corruption happened here" signal.
        const uint32_t ts = memory.read32(0x800618D4u);
        const uint32_t gaf = memory.read32(0x800618B0u);
        const bool wrecked = ts > 1000 || (gaf & 0xFF000000u) != 0;
        if (!fired && primed && wrecked) {
          fired = true;
          FILE *fa = std::fopen("/tmp/canary_antes.bin", "wb");
          FILE *fb = std::fopen("/tmp/canary_depois.bin", "wb");
          if (fa) { std::fwrite(prev.data(), 1, prev.size(), fa); std::fclose(fa); }
          if (fb) { std::fwrite(cur.data(), 1, cur.size(), fb); std::fclose(fb); }
          uint32_t diff = 0;
          for (uint32_t i = 0; i < kCanarySize; ++i)
            if (prev[i] != cur[i]) ++diff;
          fmt::print(stderr,
                     "[CANARIO] vsync={} title_state={} globanimflags=0x{:08X} bytes_alterados={}/{} "
                     "-- despejado em /tmp/canary_{{antes,depois}}.bin\n",
                     newCount, ts, gaf, diff, kCanarySize);
        }
        prev.swap(cur);
        primed = true;
      }
      if (metricsEvery != 0 && newCount % metricsEvery == 0) {
        static ps1::bios::Bios::DrainMetrics prev{};
        static uint32_t prevTicks = 0;
        static uint64_t prevCalls[ps1::bios::EventSystem::kMaxCallbackStats]{};
        static uint64_t prevNanos[ps1::bios::EventSystem::kMaxCallbackStats]{};
        const auto m = bios.drainMetrics();
        const uint32_t ticks = memory.read32(0x80034520u);
        fmt::print(stderr,
                   "[metrics] vsync={} ticks=+{} yields=+{} slow=+{} "
                   "nested=+{} queued=+{} nostack=+{}\n",
                   newCount, ticks - prevTicks, m.calls - prev.calls,
                   m.slow - prev.slow, m.nested - prev.nested,
                   m.dispatched - prev.dispatched, m.noStack - prev.noStack);
        const auto *st = bios.eventSystem().callbackStats();
        for (std::size_t i = 0; i < ps1::bios::EventSystem::kMaxCallbackStats;
             ++i) {
          const uint32_t pc = st[i].pc.load(std::memory_order_relaxed);
          if (pc == 0)
            break;
          const uint64_t c = st[i].calls.load(std::memory_order_relaxed);
          const uint64_t n = st[i].nanos.load(std::memory_order_relaxed);
          const uint64_t dc = c - prevCalls[i];
          const uint64_t dn = n - prevNanos[i];
          prevCalls[i] = c;
          prevNanos[i] = n;
          if (dc == 0)
            continue;
          fmt::print(stderr, "[metrics]   cb 0x{:08X} calls=+{} ms=+{:.2f}\n",
                     pc, dc, dn / 1e6);
        }
        prev = m;
        prevTicks = ticks;
      }
      gpu.snapshotDisplayBuffer();
      bios.updatePadBuffers();
    }
  };
  bios.setVBlankPump(vblankTick);

  // Armed after the ELF loader's bulk copies, so only the running game trips it.
  installWriteGuard(memory.ramPtr());

  g_runStart = std::chrono::steady_clock::now();

  // Launch game thread
  std::thread gameThread([&]() {
    // Identify ourselves so Bios::queueCdromEvent can inline-drain when
    // the IRQ callback fires synchronously from MIPS-driven port writes
    // on this thread.
    bios.setGameThreadId(std::this_thread::get_id());
    try {
      ctx.pc = entryPoint;
      recomp_dispatch(memory.ramPtr(), &ctx, entryPoint);
    } catch (const ps1::CpuException &e) {
      fmt::print("[Game] CPU Exception {} at PC=0x{:08X}\n",
                 static_cast<uint32_t>(e.cause), ctx.pc);
    } catch (const std::exception &e) {
      fmt::print("[Game] Exception: {}\n", e.what());
    } catch (...) {
      fmt::print("[Game] Unknown exception\n");
    }
    gameFinished.store(true, std::memory_order_release);
    gameThreadDone.store(true, std::memory_order_release);
  });

  if (const char *everyMs = std::getenv("PS1_STALL_SAMPLE")) {
    struct sigaction sa {};
    sa.sa_handler = stallSampler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGPROF, &sa, nullptr);
    const long periodMs = std::max(200L, std::strtol(everyMs, nullptr, 10));
    pthread_t gt = gameThread.native_handle();
    std::thread([gt, periodMs, &gameFinished]() {
      while (!gameFinished.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(periodMs));
        g_stallCaptured.store(false, std::memory_order_release);
        if (pthread_kill(gt, SIGPROF) != 0)
          return;
        for (int i = 0; i < 200 && !g_stallCaptured.load(std::memory_order_acquire); ++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!g_stallCaptured.load(std::memory_order_acquire))
          continue;
        char **syms = backtrace_symbols(g_stallFrames, g_stallDepth);
        fmt::print(stderr, "[stall] depth={}\n", g_stallDepth);
        for (int i = 0; i < g_stallDepth && i < 14; ++i)
          fmt::print(stderr, "[stall]   #{} {}\n", i, syms ? syms[i] : "?");
        free(syms);
      }
    }).detach();
  }

  bool running = true;
  while (running) {
    // 1. Poll SDL2 Events
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      switch (event.type) {
      case SDL_QUIT:
        running = false;
        break;
      case SDL_KEYDOWN:
        if (event.key.keysym.sym == SDLK_ESCAPE) {
          running = false;
        } else {
          mapKeyToButton(event.key.keysym.sym, input, true);
        }
        break;
      case SDL_KEYUP:
        mapKeyToButton(event.key.keysym.sym, input, false);
        break;
      }
    }

    // The hardware tick used to run here, once per presented frame, which tied
    // the CDROM and the root counters to the compositor.  Measured during
    // play: this loop ran between 32 and 54 frames per second while the guest
    // pump held 59, so the root counters -- the clock Crash reads for
    // velocity -- lost up to a third of their rate and the game moved in slow
    // motion, worse the more there was to draw.  It now rides the game
    // thread's own 60 Hz pump; see vblankTick.

    // 2. Render
    if (!renderer.processEvents()) {
      running = false;
      continue;
    }
    renderer.renderFrame();

    frameCount++;
    ps1::metrics::count("frames");

    // Capture at a chosen frame, not only at shutdown: the game clears VRAM
    // every frame, so a shutdown-only dump always catches it freshly wiped.
    //
    // The threshold counts VBlanks, not iterations of this loop.  This loop is
    // paced by the SDL renderer's vsync and has been measured anywhere from 39
    // to 59 iterations per second depending on compositor load, while the game
    // thread advances on the steady 60 Hz vblankThread.  Anchoring on
    // frameCount therefore captured a different point of the intro on every
    // run -- sometimes the logo screen, sometimes the black gap after it --
    // which is what made the golden frame look non-deterministic.
    if (const char *atFrame = std::getenv("PS1_VRAM_DUMP_FRAME")) {
      const uint32_t vblanks =
          ps1::psyq::psyq_state().vsyncCounter.load(std::memory_order_acquire);
      if (!frameDumpWritten && vblanks >= std::strtoul(atFrame, nullptr, 10)) {
        const char *path = std::getenv("PS1_VRAM_DUMP_PATH");
        frameDumpWritten =
            dumpVramPpm(gpu, path ? path : "/tmp/vram_frame.ppm");
      }
    }

    // Primitive census over a VBlank window: `PS1_CENSUS=<from>:<to>`.
    if (censusTo != 0) {
      const uint32_t vblanks =
          ps1::psyq::psyq_state().vsyncCounter.load(std::memory_order_acquire);
      if (!censusStarted && vblanks >= censusFrom) {
        censusStarted = true;
        gpu.censusReset();
      } else if (censusStarted && !censusWritten && vblanks >= censusTo) {
        censusWritten = true;
        gpu.censusDump("/tmp/census.txt");
        fmt::print(stderr, "[census] written at vsync {}\n", vblanks);
      }
    }

    // Filmstrip: `PS1_SHOT_EVERY=<n>` writes a VRAM dump every n VBlanks into
    // `PS1_SHOT_DIR`, named by VBlank.  One run then answers "did the screen
    // change, and when" without re-running per question -- which is the only
    // way to see whether a scripted button press had any effect.
    if (shotEvery != 0) {
      const uint32_t vblanks =
          ps1::psyq::psyq_state().vsyncCounter.load(std::memory_order_acquire);
      const uint32_t bucket = vblanks / shotEvery;
      if (bucket != lastShotBucket && vblanks >= shotFrom) {
        lastShotBucket = bucket;
        dumpVramPpm(gpu,
                    fmt::format("{}/shot_{:05}.ppm", shotDir, vblanks).c_str());
      }
    }

    // Status every 5 seconds
    if (frameCount % 300 == 0) {
      fmt::print("[Frame {}] I_STAT=0x{:04X} I_MASK=0x{:04X}\n", frameCount,
                 irqCtrl.readIStat(), irqCtrl.readIMask());
    }

    // Check if game thread ended
    if (gameFinished.load(std::memory_order_acquire)) {
      fmt::print("[Main] Game thread finished at frame {}\n", frameCount);
      running = false;
    }

    if (g_shutdown_requested) {
      fmt::print("[Main] Shutdown signal received at frame {}\n", frameCount);
      running = false;
    }

    // Frame pacing: SDL_GL_SetSwapInterval(1) provides VSync-based pacing
    // via the blocking SDL_GL_SwapWindow call in renderFrame().
    // No additional SDL_Delay needed -- it would double the frame time.
  }

  if (const char *vramDump = std::getenv("PS1_VRAM_DUMP_PATH")) {
    if (!frameDumpWritten)
      dumpVramPpm(gpu, vramDump);
  }
  gpu.publishMetrics();
  ps1::metrics::dumpJson();
  reportClocks(frameCount);
  dumpWriteGuard();

  // Cleanup

  // Wait for game thread (with timeout)
  if (gameThread.joinable()) {
    gameFinished.store(true, std::memory_order_release); // shutdown signal
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!gameThreadDone.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (gameThreadDone.load(std::memory_order_acquire)) {
      gameThread.join(); // lambda returned -- safe to join
    } else {
      // Game thread is still inside `recomp_dispatch` (recompiled MIPS is a
      // tight polling loop that does not check `gameFinished`).  Detaching
      // and letting destructors run is UB -- the thread will be reading
      // `Bios::cdEventQueue_` etc. while we destruct them.  Skip C++
      // destructors entirely and exit immediately; the OS will reclaim
      // memory and SDL/audio handles.
      fmt::print("[Main] Game thread did not finish in 2s -- forcing exit "
                 "(skipping destructors to avoid UAF race)\n");
      reportClocks(frameCount);
      fmt::print("Simulation ended after {} frames.\n", frameCount);
      // Re-publish: the game thread kept issuing GP0 words during the 2s
      // deadline above.  publishMetrics() only hands over what has accrued
      // since the call before the join, so this adds those words without
      // doubling the ones already reported.
      gpu.publishMetrics();
      ps1::metrics::dumpJson();
      std::_Exit(0);
    }
  }

  // Join vblank ticker (gameFinished is already set above, so it will exit)


  if (audioDevice > 0) {
    SDL_CloseAudioDevice(audioDevice);
  }
  if (gamepad) {
    SDL_GameControllerClose(gamepad);
  }
  renderer.destroy();
  SDL_Quit();

  fmt::print("Simulation ended after {} frames.\n", frameCount);
  return 0;
}
