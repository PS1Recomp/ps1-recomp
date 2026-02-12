# PS1Recomp

**Static recompilation tool for PS1 (MIPS R3000) to native C++ on PC.**

## Overview

PS1Recomp converts PS1 game binaries into native PC executables through static recompilation. Instead of emulating each instruction at runtime, it translates MIPS I instructions into equivalent C++ code at compile time, then links them with a hardware simulation runtime.

```
PS1 ISO → ELF Extraction → ps1xAnalyzer → config.toml → ps1xRecomp → C++ Code → ps1xRuntime → Native PC Game
```

## Architecture

| Component | Description |
|-----------|-------------|
| **ps1xAnalyzer** | Parses PS1 ELF binaries, detects functions, identifies PsyQ SDK calls, generates `config.toml` |
| **ps1xRecomp** | Decodes MIPS I instructions and emits literal C++ translation (1:1 per instruction) |
| **ps1xRuntime** | Simulates PS1 hardware: GTE, GPU (OpenGL), SPU (SDL Audio), CD-ROM, Memory, Input |

## Project Structure

```
ps1-recomp/
├── analyzer/          → ps1xAnalyzer (ELF parser + function finder + PsyQ signatures)
├── recompiler/        → ps1xRecomp (MIPS I decoder + C++ emitter + GTE/overlay support)
├── runtime/           → ps1xRuntime (GPU/GTE/SPU/CD-ROM/Memory/Input simulation)
├── lib/               → Third-party: ELFIO, toml11, fmt (git submodules)
├── configs/           → Per-game TOML configurations
├── tests/             → Unit tests
├── tools/             → Helper scripts
├── docs/              → Research & TCC documentation
├── CMakeLists.txt     → C++20 / CMake build system
└── LICENSE            → MIT
```

## Building

### Prerequisites

```bash
sudo apt install cmake build-essential libsdl2-dev libglfw3-dev libgl-dev
```

### Build

```bash
git clone --recurse-submodules https://github.com/your-org/ps1-recomp.git
cd ps1-recomp
mkdir build && cd build
cmake ..
make -j$(nproc)
```

## Dependencies

| Library | Purpose |
|---------|---------|
| [ELFIO](https://github.com/serge1/ELFIO) | ELF binary parser |
| [toml11](https://github.com/ToruNiina/toml11) | TOML config parser |
| [fmt](https://github.com/fmtlib/fmt) | String formatting |
| [SDL2](https://www.libsdl.org/) | Audio (SPU) + Input |
| [OpenGL](https://www.opengl.org/) | GPU rendering |

## Inspired By

- [N64Recomp](https://github.com/N64Recomp/N64Recomp) — Original static recompilation concept
- [PS2Recomp](https://github.com/ran-j/PS2Recomp) — Direct architectural reference
- [ps1-bare-metal](https://github.com/spicyjpeg/ps1-bare-metal) — PS1 hardware examples
- [Nocash PSX Specs](https://problemkaputt.de/psx-spx.htm) — Definitive PS1 hardware reference

## License

MIT