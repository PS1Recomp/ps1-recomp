# Contributing to ps1-recomp

Thank you for your interest in contributing! This guide covers everything you need to get the
project building and running, write tests, and submit changes.

---

## Table of Contents

- [Requirements](#requirements)
- [Getting Started](#getting-started)
- [Building Without a Game ROM](#building-without-a-game-rom)
- [Building With a Game](#building-with-a-game)
- [Running Tests](#running-tests)
- [Project Structure](#project-structure)
- [Adding Tests](#adding-tests)
- [Code Style](#code-style)
- [What Not to Commit](#what-not-to-commit)
- [Submitting Changes](#submitting-changes)

---

## Requirements

| Tool | Minimum version |
|------|----------------|
| CMake | 3.20 |
| C++ compiler | GCC 11 / Clang 13 (C++20 required) |
| SDL2 | any recent |
| OpenGL | 3.3 |
| Python | 3.8 (for `tools/` scripts) |
| Doxygen | 1.9 (optional, for documentation) |

**Ubuntu/Debian:**
```bash
sudo apt install cmake build-essential libsdl2-dev libglfw3-dev libgl-dev
```

**Fedora/RHEL:**
```bash
sudo dnf install cmake gcc-c++ SDL2-devel mesa-libGL-devel glfw-devel
```

**macOS (Homebrew):**
```bash
brew install cmake sdl2 glfw
```

---

## Getting Started

```bash
git clone --recurse-submodules <repo-url>
cd ps1-recomp
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

> **Note:** `--recurse-submodules` is required. The project uses ELFIO, toml11, fmt, and
> googletest as git submodules.

---

## Building Without a Game ROM

All tests and the full build work without any game files. Use `PS1RECOMP_USE_STUB=ON`:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DPS1RECOMP_USE_STUB=ON -DPS1RECOMP_BUILD_TESTS=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

This is the same configuration used by the CI pipeline.

---

## Building With a Game

### Step 1 — Provide a disc image

Place your game files under `test_roms/<GameName>/`:
```
test_roms/
└── Rayman/
    ├── Rayman (USA).cue
    └── Rayman (USA) (Track 01).bin
```

### Step 2 — Generate a config (if one doesn't exist)

```bash
./build/ps1Analyzer/ps1Analyzer \
    --disc "test_roms/Rayman/Rayman (USA).cue" \
    --out configs/rayman.toml
```

This detects function boundaries, PsyQ SDK addresses, and overlays, and writes them to the TOML
config.

### Step 3 — Recompile the game

```bash
./build/ps1Recomp/ps1Recomp configs/rayman.toml ps1Runtime/src/recompiled_out.cpp
```

### Step 4 — Apply game-specific patches (Rayman only)

```bash
python3 tools/patch_rayman.py ps1Runtime/src/recompiled_out.cpp
```

### Step 5 — Rebuild the runtime and run

```bash
cmake --build build -j$(nproc)
./build/ps1Runtime/ps1Runtime --config configs/rayman.toml
```

---

## Running Tests

```bash
# All tests
ctest --test-dir build --output-on-failure

# Verbose output for a specific test binary
./build/ps1Test/ps1Test --gtest_filter="GpuTest.*"

# Run only runtime tests
ctest --test-dir build -R runtime --output-on-failure
```

Tests cover all hardware subsystems (GPU, SPU, DMA, BIOS, CD-ROM, GTE, MDEC, timers, input)
and do not require a game ROM.

---

## Project Structure

```
ps1Analyzer/    ELF parsing, function detection, PsyQ signature matching
ps1Recomp/      MIPS R3000A → C++ translation, jump table detection, GTE emission
ps1Runtime/     PS1 hardware simulation (GPU, BIOS HLE, SPU, DMA, CD-ROM, GTE, MDEC…)
ps1Interface/   Optional GUI studio (ImGui) — build with -DPS1RECOMP_BUILD_INTERFACE=ON
ps1Test/        Google Test suite (56 files, 386+ tests)
tools/          Python automation: game-specific patching, diagnostics, Ghidra integration
configs/        Per-game TOML configs (gitignored except examples)
third_party/    Git submodules (ELFIO, toml11, fmt, googletest)
```

The pipeline flows: `ps1Analyzer` → `ps1Recomp` → `patch_*.py` → `ps1Runtime`.

---

## Adding Tests

Test files live in `ps1Test/` and mirror the source structure:

```
ps1Test/
├── analyzer/       tests for ps1Analyzer
├── pipeline/       end-to-end pipeline tests
├── recompiler/     tests for ps1Recomp
└── runtime/        tests for ps1Runtime subsystems (44 files)
```

Every new hardware behavior or HLE function must have a test. A minimal example:

```cpp
// ps1Test/runtime/test_my_feature.cpp
#include <gtest/gtest.h>
#include <runtime/my_feature.h>

TEST(MyFeatureTest, DoesTheThing) {
    ps1::MyFeature f;
    f.reset();
    EXPECT_EQ(f.readStatus(), 0x14802000u);
}
```

**Important:** when your test uses temporary files, use a unique path per test to avoid
conflicts when tests run in parallel:

```cpp
TEST(SaveStateTest, RoundTrip) {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string path = std::string("/tmp/test_") + info->name() + ".state";
    // ... use path
}
```

Register your file in `ps1Test/CMakeLists.txt` under the appropriate target.

---

## Code Style

- **C++20** throughout — use `[[likely]]`/`[[unlikely]]`, `std::span`, structured bindings where
  they improve clarity.
- **Namespaces:** all runtime code lives in `ps1::` sub-namespaces (e.g. `ps1::gpu`,
  `ps1::bios`). Do not add code to the global namespace except for `recomp_dispatch` and
  generated recompiled functions.
- **Error handling:** hardware errors set status registers or trigger interrupts — do not throw
  exceptions for expected hardware conditions. Throw only for unrecoverable programmer errors
  (missing config, allocation failure).
- **Memory access:** always use the macros in `ps1Runtime/include/runtime/memory.h`. Never cast
  `rdram` pointers directly. Apply the KSEG mask (`addr & 0x1FFFFFFF`) before indexing.
- **Logging:** use `fmt::print` for runtime diagnostics. Prefix with `[SUBSYSTEM]` so logs
  are easy to filter (e.g. `[GPU]`, `[BIOS]`, `[MDEC]`).

---

## What Not to Commit

| Path | Reason |
|------|--------|
| `ps1Runtime/src/recompiled_out.cpp` | Generated per-game; gitignored |
| `test_roms/` | Copyrighted game files |
| `configs/*.toml` (except `example_game.toml`) | Contain local ROM paths |
| `docs/` | Generated documentation; gitignored |
| `.claude/` | AI session files; gitignored |
| Any file with absolute paths to your machine | Not portable |

---

## Submitting Changes

1. Fork the repository and create a branch from `main`.
2. Make your changes. If touching hardware emulation, add or update tests in `ps1Test/`.
3. Confirm `ctest` passes: `ctest --test-dir build --output-on-failure`
4. Open a pull request against `main` with a clear description of what changed and why.

For large changes (new subsystems, API changes), open an issue first to discuss the approach
before writing code.
