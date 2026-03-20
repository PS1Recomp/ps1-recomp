# PS1Recomp

**Static recompiler for PlayStation 1 games — translates MIPS R3000A machine code to native C++ for PC.**

## Overview

PS1Recomp converts PS1 game binaries into native PC executables through static recompilation. Instead of interpreting each MIPS instruction at runtime like a traditional emulator, it translates the entire game binary to C++ at compile time and links it against a hardware simulation runtime. The result is a standalone native executable that runs at full CPU speed.

```
PS1 ISO/BIN
    │
    ▼
ps1xAnalyzer  →  game_config.toml   (function boundaries, PsyQ addresses)
    │
    ▼
ps1xRecomp    →  recompiled_out.cpp  (MIPS → C++, ~150k lines per game)
    │
    ▼
patch_rayman.py → recompiled_out.cpp (HLE patches for middleware functions)
    │
    ▼
ps1xRuntime   →  Native PC executable (SDL2 + OpenGL)
```

## Architecture

| Component | Directory | Description |
|-----------|-----------|-------------|
| **ps1xAnalyzer** | `ps1xAnalyzer/` | Parses PS1 ELF/BIN, detects function boundaries, identifies PsyQ SDK signatures, generates TOML config |
| **ps1xRecomp** | `ps1xRecomp/` | Decodes MIPS R3000A instructions and emits literal C++ translation (1:1 per instruction), handles GTE coprocessor and jump tables |
| **ps1xRuntime** | `ps1xRuntime/` | Simulates PS1 hardware: BIOS HLE, GPU (OpenGL 3.3), SPU (SDL2 audio), CD-ROM, DMA, GTE, MDEC, Timers, Input |
| **ps1xTest** | `ps1xTest/` | 365 unit + integration tests covering all runtime subsystems |

## Project Structure

```
ps1-recomp/
├── ps1xAnalyzer/      ELF parser, function finder, PsyQ signature DB, disc reader
├── ps1xRecomp/        MIPS decoder, C++ emitter, GTE emitter, overlay handler
├── ps1xRuntime/       Full PS1 hardware simulation + SDL2/OpenGL host
│   └── src/
│       ├── bios/      BIOS HLE (syscall tables A/B/C, heap, events, file I/O)
│       ├── cdrom/     CD-ROM emulation (ISO 9660, CUE/BIN, virtual FS)
│       ├── gpu/       GPU state machine + OpenGL renderer
│       ├── spu/       Sound Processing Unit (24-voice ADPCM)
│       ├── mdec/      Motion Picture Decoder (FMV video)
│       ├── dma/       DMA controllers (Ch0–Ch3)
│       ├── timers/    Hardware timers
│       ├── psyq/      PsyQ middleware HLE (VSync, DrawSync, DrawOTag, etc.)
│       └── main_host.cpp  SDL2 host: game loop, input, VBlank thread
├── ps1xTest/          Unit tests (GTest): 365 tests across all subsystems
├── third_party/       External dependencies: ELFIO, toml11, fmt, googletest (git submodules)
├── tools/
│   ├── patch_rayman.py        Game-specific HLE patches (applied after recompilation)
│   ├── ps1_mcp_server.py      MCP server for AI-assisted debugging (13 tools)
│   ├── run_and_report.py      Automated game diagnostics (VRAM, frame rate, logs)
│   ├── demo_build.sh          Quick-start demo script
│   ├── ghidra/                Ghidra integration scripts
│   │   ├── ExportPS1Functions.py    Export function list from Ghidra to CSV
│   │   ├── ExportPS1Functions.java  Same, Java version (headless mode)
│   │   └── start_ghidra_mcp.sh      Launch Ghidra with GhidraMCP on port 8080
│   └── pcsx_redux_mcp/        PCSX-Redux emulator integration (MCP server)
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

### Run Tests

```bash
ctest --test-dir build --output-on-failure -j$(nproc)
```

## Running a Game (Rayman example)

1. Place the game disc image in `test_roms/`:
   ```
   test_roms/Rayman/Rayman (USA).cue
   test_roms/Rayman/Rayman (USA) (Track 01).bin
   ```

2. Analyze the binary and generate config:
   ```bash
   ./build/ps1xAnalyzer/ps1xAnalyzer test_roms/Rayman/Rayman\ \(USA\)\ \(Track\ 01\).bin.boot.exe rayman_config.toml
   ```

3. Recompile MIPS → C++:
   ```bash
   ./build/ps1xRecomp/ps1xRecomp rayman_config.toml ps1xRuntime/src/recompiled_out.cpp
   ```

4. Apply HLE patches:
   ```bash
   python3 tools/patch_rayman.py
   ```

5. Build and run:
   ```bash
   cmake --build build -j$(nproc)
   ./build/ps1xRuntime/ps1xRuntime --config configs/rayman.toml
   ```

## How It Works

### Phase 1 — Analysis (`ps1xAnalyzer`)
- Reads the disc image (ISO 9660 / CUE+BIN) and extracts the PS1 executable
- Parses ELF sections: `.text` (MIPS code), `.data`, `.bss`
- Detects function boundaries via prologue patterns and call graph analysis
- Identifies PsyQ middleware calls via signature matching
- Outputs a TOML config with all function addresses and BSS layout

### Phase 2 — Recompilation (`ps1xRecomp`)
- Decodes every MIPS R3000A instruction (32-bit fixed-width)
- Emits equivalent C++ for each instruction using a `recomp_context` struct as register file
- Detects jump tables (switch statements) via LUI+ADDIU+ADDU+LW+JR pattern
- Handles GTE coprocessor instructions separately via `gte_emitter`
- Produces a single `recompiled_out.cpp` with one C++ function per MIPS function

### Phase 3 — Runtime (`ps1xRuntime`)
- **BIOS HLE**: implements PS1 syscall tables A/B/C without a BIOS ROM
- **GPU**: interprets GP0/GP1 commands, rasterizes polygons/lines/rectangles into VRAM (1024×512), renders via OpenGL 3.3 shader
- **CD-ROM**: emulates drive state machine, reads sectors from CUE/BIN files via ISO 9660
- **SPU**: 24-voice ADPCM audio, mixed via SDL2 audio callback
- **DMA**: transfers between RAM, GPU, SPU, MDEC, and CD-ROM
- **GTE**: geometry transform engine for 3D vertex processing
- **PsyQ HLE**: replaces PsyQ middleware functions (VSync, DrawSync, DrawOTag) with native implementations

## Key Design Decisions

**Static vs. dynamic recompilation**: All translation happens at compile time — no JIT, no interpreter loop. Game code runs as regular C++ function calls at native CPU speed.

**HLE over LLE**: BIOS and PsyQ middleware are implemented as High-Level Emulation (HLE) stubs rather than running the original ROM code. This avoids legal issues and simplifies the runtime.

**Game-specific patches**: `tools/patch_rayman.py` applies targeted patches to the generated C++ for Rayman-specific quirks (timing-sensitive VSync loops, FMV skipping, dispatch table overrides).

**Double-buffered framebuffer**: Rayman uses vertical double-buffering in PS1 VRAM — Buffer A at (0,0) is rendered while Buffer B at (0,256) is displayed, swapping each VBlank at 60Hz.

## Tooling

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
| SDL2 | Audio (SPU) + window + input | System package |
| OpenGL 3.3 | GPU rendering backend | System package |

## Inspired By

- [N64Recomp](https://github.com/N64Recomp/N64Recomp) — pioneered the static recompilation approach for retro consoles
- [PS2Recomp](https://github.com/ran-j/PS2Recomp) — direct architectural reference (ps2xAnalyzer/ps2xRecomp/ps2xRuntime structure)
- [psxrecomp](https://1379.tech/i-built-a-ps1-static-recompiler-with-no-prior-experience-and-claude-code/) — PS1 static recompiler reference implementation
- [Nocash PSX Specs](https://problemkaputt.de/psx-spx.htm) — definitive PS1 hardware reference
- [ps1-bare-metal](https://github.com/spicyjpeg/ps1-bare-metal) — PS1 hardware programming examples

## License

GPLv3
