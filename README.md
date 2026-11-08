# PS1Recomp

[![CI](https://github.com/PS1Recomp/ps1-recomp/actions/workflows/ci.yml/badge.svg)](https://github.com/PS1Recomp/ps1-recomp/actions/workflows/ci.yml) [![codecov](https://codecov.io/gh/PS1Recomp/ps1-recomp/branch/main/graph/badge.svg)](https://codecov.io/gh/PS1Recomp/ps1-recomp)

**Static recompiler for PlayStation 1 games -- translates MIPS R3000A machine code to native C++ for PC.**

## Overview

PS1Recomp converts PS1 game binaries into native PC executables through static recompilation. Instead of interpreting each MIPS instruction at runtime like a traditional emulator, it translates the entire game binary to C++ at compile time and links it against a hardware simulation runtime. The result is a standalone native executable that runs at full CPU speed.

```
PS1 ISO/BIN
    |
    v
ps1Analyzer  ->  game_config.toml   (function boundaries, PsyQ signatures)
    |
    v
ps1Recomp    ->  recompiled_out.cpp  (MIPS -> C++, ~150k lines per game)
    |
    v
ps1Runtime   ->  Native PC executable (SDL2 + OpenGL)
```

## Architecture

| Component | Directory | Description |
|-----------|-----------|-------------|
| **ps1Analyzer** | `ps1Analyzer/` | Parses PS1 ELF/BIN, detects function boundaries, identifies PsyQ SDK signatures, generates TOML config |
| **ps1Recomp** | `ps1Recomp/` | Decodes MIPS R3000A instructions and emits literal C++ translation (1:1 per instruction), handles GTE coprocessor and jump tables |
| **ps1Runtime** | `ps1Runtime/` | Simulates PS1 hardware: BIOS HLE, GPU (software rasterizer, OpenGL output), SPU (SDL2 audio), CD-ROM, DMA, GTE, MDEC, Timers, Input |
| **ps1Interface** | `ps1Interface/` | GUI studio for loading ELFs, exploring functions, editing configs, and previewing recompiled C++ (ImGui + SDL2) |
| **ps1Test** | `ps1Test/` | GoogleTest suite: instruction semantics, every runtime subsystem, HLE contracts |

## Project Structure

```
ps1-recomp/
+-- ps1Analyzer/       ELF parser, function finder, PsyQ signature DB, disc reader
+-- ps1Recomp/         MIPS decoder, C++ emitter, GTE emitter, overlay handler
+-- ps1Runtime/        Full PS1 hardware simulation + SDL2/OpenGL host
|   +-- src/
|       +-- bios/      BIOS HLE (syscall tables A/B/C, heap, events, file I/O)
|       +-- cdrom/     CD-ROM emulation (ISO 9660, CUE/BIN, virtual FS)
|       +-- gpu/       GPU state machine + OpenGL renderer
|       +-- spu/       Sound Processing Unit (24-voice ADPCM)
|       +-- mdec/      Motion Picture Decoder (FMV video)
|       +-- dma/       DMA controllers (Ch0-Ch6)
|       +-- timers/    Hardware timers
|       +-- psyq/      PsyQ middleware HLE (VSync, DrawSync, DrawOTag, etc.)
|       +-- main_host.cpp  SDL2 host: game loop, input, VBlank thread
+-- ps1Interface/      GUI studio (ImGui docking): ELF explorer, function inspector,
|   |                  C++ preview, config editor, Ghidra CSV import
|   +-- include/
|   |   +-- StudioState.hpp    Async state management (analysis, config, exports)
|   |   +-- GUI.hpp            4 dockable panels + menu bar
|   |   +-- ui/StyleManager.hpp  Dark/Light/Custom themes and font scaling
|   +-- src/
|       +-- main.cpp           SDL2 + OpenGL3 entry point with ImGui docking
|       +-- StudioState.cpp    ps1Analyzer backend integration
|       +-- GUI.cpp            Panel rendering and keyboard shortcuts
|       +-- ui/StyleManager.cpp
+-- ps1Test/           Unit and integration tests (GoogleTest)
+-- third_party/       External dependencies: ELFIO, toml11, fmt, googletest
+-- tools/
|   +-- extract_psyq_signatures.py   Build the PsyQ signature DB from PsyQ SDK .LIB files
|   +-- psyq_lib_extract.py          Split SN Systems .LIB archives into per-function .OBJ
|   +-- ps1_mcp_server.py            MCP server for AI-assisted debugging
|   +-- run_and_report.py            Automated game diagnostics (VRAM, frame rate, logs)
|   +-- validate_game.py             Pixel-content validation against a frame budget
|   +-- demo_build.sh                Quick-start demo script
|   +-- ghidra/                      Ghidra integration scripts
|   |   +-- ExportPS1Functions.py    Export function list from Ghidra to CSV
|   |   +-- ExportPS1Functions.java  Same, Java version (headless mode)
|   |   +-- start_ghidra_mcp.sh      Launch Ghidra with GhidraMCP on port 8080
|   +-- pcsx_redux_mcp/              PCSX-Redux emulator integration (MCP server)
+-- .github/workflows/ci.yml   CI/CD: build + test on every push
+-- CMakeLists.txt             C++20 / CMake 3.20+ build system
+-- LICENSE                    GPLv3
```

## Building

### Prerequisites

```bash
# Ubuntu / Debian
sudo apt install cmake build-essential libsdl2-dev libgl-dev

# macOS
brew install cmake sdl2
```

### Clone and Build

```bash
git clone --recurse-submodules https://github.com/PS1Recomp/ps1-recomp.git
cd ps1-recomp
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Build with GUI Studio (optional)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DPS1RECOMP_BUILD_INTERFACE=ON
cmake --build build -j$(nproc)
./build/ps1Interface/ps1Interface
```

### Run Tests

```bash
ctest --test-dir build --output-on-failure -j$(nproc)
```

## Running Crash Bandicoot

> **Note:** Game disc images are not included in this repository. Use an image extracted
> from a disc you own.

Crash Bandicoot (USA, SCUS-94900) is the validated title.

1. Put the disc image under `test_roms/Crash Bandicoot /` (the paths in
   `configs/examples/crash.toml`), or edit those paths.
2. Copy the runtime config:
   ```bash
   cp configs/examples/crash.toml configs/crash.toml
   ```
3. Analyze the binary, recompile it to C++ and rebuild:
   ```bash
   tools/regen_crash.sh --recomp
   cmake --build build -j$(nproc)
   ```
4. Run (from the repository root, since config paths are relative):
   ```bash
   ./build/ps1Runtime/ps1Runtime --config configs/crash.toml
   ```

### Other games

The same pipeline applies to any PS1 executable, with per-game facts kept in TOML:

```bash
./build/ps1Analyzer/ps1Analyzer game.exe configs/game_recomp.toml
./build/ps1Recomp/ps1Recomp configs/game_recomp.toml ps1Runtime/src/recompiled_out.cpp
cmake --build build -j$(nproc)
./build/ps1Runtime/ps1Runtime --config configs/game.toml
```

A new game usually needs analyzer fixes (missed functions, wrong sizes) and runtime
settings before it runs; see `configs/examples/crash.toml` for what Crash needed.

## Controls

The keyboard stands in for a PS1 digital pad. The runtime prints this table on
startup, so it is always visible in the log of a run.

| Keyboard | PS1 pad | Keyboard | PS1 pad |
|---|---|---|---|
| Arrow keys | D-Pad | <kbd>Q</kbd> | L1 |
| <kbd>Z</kbd> | Cross | <kbd>W</kbd> | R1 |
| <kbd>X</kbd> | Circle | <kbd>E</kbd> | L2 |
| <kbd>A</kbd> | Square | <kbd>R</kbd> | R2 |
| <kbd>S</kbd> | Triangle | <kbd>C</kbd> | L3 |
| <kbd>Enter</kbd> | Start | <kbd>V</kbd> | R3 |
| <kbd>Right Shift</kbd> or <kbd>Backspace</kbd> | Select | <kbd>Esc</kbd> | quit |

Each press is echoed to stderr as `[pad] <key> -> <button>`, which separates a
button that reached the pad from one that never got there.

<kbd>F2</kbd> turns the PS1 dithering pattern off and on.

### Gamepads (XInput)

Any controller SDL2 recognises works: Xbox 360/One/Series pads, XInput-mode
third-party pads (e.g. GuliKit KingKong), and most DualShock/DualSense pads.
Plug it in before or after starting the game. The window title then shows its
name. Buttons map by position:

| Gamepad (Xbox names) | PS1 pad |
|---|---|
| A / B / X / Y | Cross / Circle / Square / Triangle |
| LB / RB, LT / RT | L1 / R1, L2 / R2 |
| Stick clicks | L3 / R3 |
| Back / Start | Select / Start |
| D-pad, left stick | D-Pad |

Input is read only while the game window has focus.

### Memory cards

Two cards are created under `memcards/` on first run, 128 KB each and formatted
as a real card is. Set `[paths] memcard_dir` in the game config to put them
somewhere else. A written sector is flushed to the file immediately, because
shutdown skips destructors and a deferred write would be lost.

## How It Works

### Phase 1 -- Analysis (`ps1Analyzer`)
- Reads the disc image (ISO 9660 / CUE+BIN) and extracts the PS1 executable
- Parses ELF sections: `.text` (MIPS code), `.data`, `.bss`
- Detects function boundaries via prologue patterns and call graph analysis
- Identifies PsyQ middleware calls via signature matching
- Outputs a TOML config with all function addresses and BSS layout

### Phase 2 -- Recompilation (`ps1Recomp`)
- Decodes every MIPS R3000A instruction (32-bit fixed-width)
- Emits equivalent C++ for each instruction using a `recomp_context` struct as register file
- Detects jump tables (switch statements) via LUI+ADDIU+ADDU+LW+JR pattern
- Handles GTE coprocessor instructions separately via `gte_emitter`
- Produces a single `recompiled_out.cpp` with one C++ function per MIPS function

### Phase 3 -- Runtime (`ps1Runtime`)
- **BIOS HLE**: implements PS1 syscall tables A/B/C without a BIOS ROM
- **GPU**: interprets GP0/GP1 commands, rasterizes polygons/lines/rectangles into VRAM (1024x512), renders via OpenGL 3.3 shader
- **CD-ROM**: emulates drive state machine, reads sectors from CUE/BIN files via ISO 9660
- **SPU**: 24-voice ADPCM audio, mixed via SDL2 audio callback
- **DMA**: transfers between RAM, GPU, SPU, MDEC, and CD-ROM
- **GTE**: geometry transform engine for 3D vertex processing
- **PsyQ HLE**: replaces PsyQ middleware functions (VSync, DrawSync, DrawOTag, SetDefDispEnv, PutDispEnv, PutDrawEnv) with native implementations

### GUI Studio (`ps1Interface`)
- **Explorer panel**: lists all functions with color-coded strategy (PsyQ=yellow, Stub=red, Skip=gray, ForceRecompile=green), filterable by name
- **Inspector panel**: shows function details (address, size, disassembly summary) and lets you change the recompilation strategy per function
- **Workspace panel**: tabbed view with C++ preview, hex dump, config.toml editor, and Ghidra CSV importer
- **Logs panel**: real-time analysis progress bar and error markers
- Keyboard shortcuts: `F5` = Analyze, `Ctrl+O` = Open ELF, `Ctrl+S` = Save config

## Key Design Decisions

**Static vs. dynamic recompilation**: All translation happens at compile time -- no JIT, no interpreter loop. Game code runs as regular C++ function calls at native CPU speed.

**HLE over LLE**: BIOS and PsyQ middleware are implemented as High-Level Emulation (HLE) stubs rather than running the original ROM code. This avoids legal issues and simplifies the runtime.

**Hash-based PsyQ detection**: `ps1Analyzer` matches each function body against a SHA-256 database of PsyQ SDK functions extracted from 14 SDK versions (3463 signatures, 1943 unique after collision filtering). Detected functions are routed to native HLE implementations at runtime instead of being recompiled. This avoids per-game imperative patches and is the project's main divergence from prior PS1 recompilers.

**Double-buffered framebuffer**: PS1 games typically use double-buffering in VRAM (e.g. Buffer A at (0,0) rendered while Buffer B at (0,256) is displayed, swapping each VBlank at 60Hz). The runtime infers the active display window from the GPU command stream.

## Tooling

### GUI Studio
```bash
./build/ps1Interface/ps1Interface
```
Opens the visual studio for loading ELF files, exploring functions, editing game configs, and previewing the generated C++ output before running the full pipeline.

### Ghidra Integration
The project includes Ghidra scripts for static analysis validation:
- Install the `ghidra_psx_ldr` plugin (PSX-specific loader with GTE + PsyQ support)
- Run `tools/ghidra/ExportPS1Functions.py` inside Ghidra to export function boundaries to CSV
- Run `tools/ghidra/start_ghidra_mcp.sh` to start Ghidra with the MCP bridge on port 8080

### Automated Diagnostics
```bash
python3 tools/run_and_report.py --duration 15 --output /tmp/report.json
```
Runs the game for N seconds and outputs a JSON report with frame rate, VRAM pixel analysis, GPU command log, and CD-ROM callback counts.

## Dependencies

| Library | Purpose | How included |
|---------|---------|--------------|
| [ELFIO](https://github.com/serge1/ELFIO) | ELF binary parser | Git submodule |
| [toml11](https://github.com/ToruNiina/toml11) | TOML config parser | Git submodule |
| [fmt](https://github.com/fmtlib/fmt) | String formatting | Git submodule |
| [GoogleTest](https://github.com/google/googletest) | Unit testing framework | Git submodule |
| [Dear ImGui](https://github.com/ocornut/imgui) | GUI (docking branch) | FetchContent (ps1Interface only) |
| [ImGuiColorTextEdit](https://github.com/BalazsJako/ImGuiColorTextEdit) | Syntax-highlighted code editor | FetchContent (ps1Interface only) |
| [ImGuiFileDialog](https://github.com/aiekick/ImGuiFileDialog) | Native file open/save dialog | FetchContent (ps1Interface only) |
| SDL2 | Audio (SPU) + window + input | System package |
| OpenGL 3.3 | GPU rendering backend | System package |

## Inspired By

- [N64Recomp](https://github.com/N64Recomp/N64Recomp) -- pioneered the static recompilation approach for retro consoles
- [PS2Recomp](https://github.com/ran-j/PS2Recomp) -- direct architectural reference (ps2Analyzer/ps2Recomp/ps2Runtime structure)
- [psxrecomp](https://1379.tech/i-built-a-ps1-static-recompiler-with-no-prior-experience-and-claude-code/) -- PS1 static recompiler reference implementation
- [Nocash PSX Specs](https://problemkaputt.de/psx-spx.htm) -- definitive PS1 hardware reference
- [ps1-bare-metal](https://github.com/spicyjpeg/ps1-bare-metal) -- PS1 hardware programming examples

## Status

The project is the subject of an undergraduate thesis. It is a recompiler with a runtime
that covers enough of the PS1 to run its case study, not a general emulator.

**Crash Bandicoot** runs from boot through the menus, map and levels at the game's
30 fps, with music, sound effects, memory card saves and gamepad input. Levels are
finished by a human player and replayed frame-exactly by the lockstep mode
(`PS1_STEP`), which turns a recorded run into a regression test.

Known limits:

- **One validated game.** Rayman was the first target and ran with hand-written
  patches; without them its regenerated build boots but draws nothing.
- **FMV:** MDEC decoding is incomplete (Crash does not use MDEC).
- **Hardware accuracy:** GPU commands have no cycle timing; SPU reverb and pitch
  modulation are approximations; some GTE, GPU and BIOS edge cases differ from
  psx-spx.

## License

GPLv3
