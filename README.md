# PS1Recomp

**Static recompiler for PlayStation 1 games — translates MIPS R3000A machine code to native C++ for PC.**

## Overview

PS1Recomp converts PS1 game binaries into native PC executables through static recompilation. Instead of interpreting each MIPS instruction at runtime like a traditional emulator, it translates the entire game binary to C++ at compile time and links it against a hardware simulation runtime. The result is a standalone native executable that runs at full CPU speed.

```
PS1 ISO/BIN
    │
    ▼
ps1Analyzer  →  game_config.toml   (function boundaries, PsyQ signatures)
    │
    ▼
ps1Recomp    →  recompiled_out.cpp  (MIPS → C++, ~150k lines per game)
    │
    ▼
ps1Runtime   →  Native PC executable (SDL2 + OpenGL)
```

## Architecture

| Component | Directory | Description |
|-----------|-----------|-------------|
| **ps1Analyzer** | `ps1Analyzer/` | Parses PS1 ELF/BIN, detects function boundaries, identifies PsyQ SDK signatures, generates TOML config |
| **ps1Recomp** | `ps1Recomp/` | Decodes MIPS R3000A instructions and emits literal C++ translation (1:1 per instruction), handles GTE coprocessor and jump tables |
| **ps1Runtime** | `ps1Runtime/` | Simulates PS1 hardware: BIOS HLE, GPU (OpenGL 3.3), SPU (SDL2 audio), CD-ROM, DMA, GTE, MDEC, Timers, Input |
| **ps1Interface** | `ps1Interface/` | GUI studio for loading ELFs, exploring functions, editing configs, and previewing recompiled C++ (ImGui + SDL2) |
| **ps1Test** | `ps1Test/` | 557 unit + integration tests covering all runtime subsystems |

## Project Structure

```
ps1-recomp/
├── ps1Analyzer/       ELF parser, function finder, PsyQ signature DB, disc reader
├── ps1Recomp/         MIPS decoder, C++ emitter, GTE emitter, overlay handler
├── ps1Runtime/        Full PS1 hardware simulation + SDL2/OpenGL host
│   └── src/
│       ├── bios/      BIOS HLE (syscall tables A/B/C, heap, events, file I/O)
│       ├── cdrom/     CD-ROM emulation (ISO 9660, CUE/BIN, virtual FS)
│       ├── gpu/       GPU state machine + OpenGL renderer
│       ├── spu/       Sound Processing Unit (24-voice ADPCM)
│       ├── mdec/      Motion Picture Decoder (FMV video)
│       ├── dma/       DMA controllers (Ch0–Ch6)
│       ├── timers/    Hardware timers
│       ├── psyq/      PsyQ middleware HLE (VSync, DrawSync, DrawOTag, etc.)
│       └── main_host.cpp  SDL2 host: game loop, input, VBlank thread
├── ps1Interface/      GUI studio (ImGui docking): ELF explorer, function inspector,
│   │                  C++ preview, config editor, Ghidra CSV import
│   ├── include/
│   │   ├── StudioState.hpp    Async state management (analysis, config, exports)
│   │   ├── GUI.hpp            4 dockable panels + menu bar
│   │   └── ui/StyleManager.hpp  Dark/Light/Custom themes and font scaling
│   └── src/
│       ├── main.cpp           SDL2 + OpenGL3 entry point with ImGui docking
│       ├── StudioState.cpp    ps1Analyzer backend integration
│       ├── GUI.cpp            Panel rendering and keyboard shortcuts
│       └── ui/StyleManager.cpp
├── ps1Test/           Unit tests (GTest): 557 tests across all subsystems
├── third_party/       External dependencies: ELFIO, toml11, fmt, googletest
├── tools/
│   ├── extract_psyq_signatures.py   Build the PsyQ signature DB from PsyQ SDK .LIB files
│   ├── psyq_lib_extract.py          Split SN Systems .LIB archives into per-function .OBJ
│   ├── ps1_mcp_server.py            MCP server for AI-assisted debugging
│   ├── run_and_report.py            Automated game diagnostics (VRAM, frame rate, logs)
│   ├── validate_game.py             Pixel-content validation against a frame budget
│   ├── demo_build.sh                Quick-start demo script
│   ├── ghidra/                      Ghidra integration scripts
│   │   ├── ExportPS1Functions.py    Export function list from Ghidra to CSV
│   │   ├── ExportPS1Functions.java  Same, Java version (headless mode)
│   │   └── start_ghidra_mcp.sh      Launch Ghidra with GhidraMCP on port 8080
│   └── pcsx_redux_mcp/              PCSX-Redux emulator integration (MCP server)
├── .github/workflows/ci.yml   CI/CD: build + test on every push
├── CMakeLists.txt             C++20 / CMake 3.20+ build system
└── LICENSE                    GPLv3
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
git clone --recurse-submodules https://github.com/Dellareti/ps1-recomp.git
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

## Running a Game (Rayman example)

> **Note:** Game disc images (ROMs/ISOs) are not included in this repository for legal
> reasons. You need your own legal copy of the game. The disc path is configured per-game
> and can point to **any folder on your system** — there is no required directory name.

1. Place your disc image anywhere and note the path:
   ```
   # Example paths — use whatever works for you:
   /home/user/games/Rayman (USA).cue
   ~/roms/Rayman (USA).cue
   ./my_games/Rayman (USA).cue
   ```

2. Analyze the binary and generate config:
   ```bash
   ./build/ps1Analyzer/ps1Analyzer "/path/to/Rayman (USA) (Track 01).bin.boot.exe" rayman_config.toml
   ```

3. Edit `rayman_config.toml` to set your disc path:
   ```toml
   [disc]
   cue_path = "/path/to/Rayman (USA).cue"
   ```

4. Recompile MIPS → C++:
   ```bash
   ./build/ps1Recomp/ps1Recomp rayman_config.toml ps1Runtime/src/recompiled_out.cpp
   ```

5. Build and run:
   ```bash
   cmake --build build -j$(nproc)
   ./build/ps1Runtime/ps1Runtime --config rayman_config.toml
   ```

## How It Works

### Phase 1 — Analysis (`ps1Analyzer`)
- Reads the disc image (ISO 9660 / CUE+BIN) and extracts the PS1 executable
- Parses ELF sections: `.text` (MIPS code), `.data`, `.bss`
- Detects function boundaries via prologue patterns and call graph analysis
- Identifies PsyQ middleware calls via signature matching
- Outputs a TOML config with all function addresses and BSS layout

### Phase 2 — Recompilation (`ps1Recomp`)
- Decodes every MIPS R3000A instruction (32-bit fixed-width)
- Emits equivalent C++ for each instruction using a `recomp_context` struct as register file
- Detects jump tables (switch statements) via LUI+ADDIU+ADDU+LW+JR pattern
- Handles GTE coprocessor instructions separately via `gte_emitter`
- Produces a single `recompiled_out.cpp` with one C++ function per MIPS function

### Phase 3 — Runtime (`ps1Runtime`)
- **BIOS HLE**: implements PS1 syscall tables A/B/C without a BIOS ROM
- **GPU**: interprets GP0/GP1 commands, rasterizes polygons/lines/rectangles into VRAM (1024×512), renders via OpenGL 3.3 shader
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

**Static vs. dynamic recompilation**: All translation happens at compile time — no JIT, no interpreter loop. Game code runs as regular C++ function calls at native CPU speed.

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

- [N64Recomp](https://github.com/N64Recomp/N64Recomp) — pioneered the static recompilation approach for retro consoles
- [PS2Recomp](https://github.com/ran-j/PS2Recomp) — direct architectural reference (ps2Analyzer/ps2Recomp/ps2Runtime structure)
- [psxrecomp](https://1379.tech/i-built-a-ps1-static-recompiler-with-no-prior-experience-and-claude-code/) — PS1 static recompiler reference implementation
- [Nocash PSX Specs](https://problemkaputt.de/psx-spx.htm) — definitive PS1 hardware reference
- [ps1-bare-metal](https://github.com/spicyjpeg/ps1-bare-metal) — PS1 hardware programming examples

## Status

The project is the subject of an ongoing undergraduate thesis. **It is not a finished
emulator** — it is a recompiler whose architecture works end-to-end, plus a runtime that
covers enough of the PS1 hardware to boot real games. The current state, honestly:

- **Pipeline**: stable. Analyzer → recompiler → runtime → native binary works for any PS1
  ELF/EXE. Stub-mode build (no ROM) is the CI baseline (557/557 tests green).
- **Rayman (USA)**: was the primary validation target through Phases 0–3. With prior
  imperative HLE patches it ran at ~59fps with correct VRAM content. Those patches were
  retired during open-source preparation; the regen-fresh path now relies entirely on the
  PsyQ hash-based HLE coverage and has not been re-validated end-to-end since the patch
  removal.
- **Crash Bandicoot 1 (USA)**: experimental. Boots through PsyQ init, then stalls in a
  game-side hash table walk; a signature-matcher bug for short libcd wrappers is the next
  known blocker (see `PLANNING.md` Phase 4 notes).
- **Other games**: not exercised yet. The architecture is meant to be game-agnostic; adding
  a new title is mostly a matter of providing a TOML config and running the analyzer.

For an honest, gap-by-gap account of what works and what does not, see `ARCHITECTURE.md`.

## License

GPLv3
