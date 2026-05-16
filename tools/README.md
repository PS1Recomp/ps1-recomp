# tools/

Scripts and helper binaries used during development and PsyQ signature
generation. Nothing here is required to build or run the project — the main
pipeline lives in `ps1Analyzer/`, `ps1Recomp/` and `ps1Runtime/`.

## Pipeline tooling (user-facing)

| File | Purpose |
|------|---------|
| `extract_psyq_signatures.py` | Hash every function in a PsyQ `.LIB` archive and emit/update `ps1Analyzer/data/psyq_signatures.toml`. Run once per SDK release added. |
| `psyq_lib_extract.py` | Split a SN Systems `.LIB` archive into per-member `.OBJ` files (helper for the script above). |
| `cross_reference_psyz_symbols.py` | Cross-reference our signature DB against the [psyz](https://github.com/Xeeynamo/psyz) PSY-Q 4.0/4.7 symbol maps. Useful when checking gap coverage. |
| `validate_game.py` | End-to-end driver: build, run a game config for N seconds, snapshot VRAM, return a structured report. |
| `run_and_report.py` | Lightweight runtime probe used by `validate_game.py`. |

## Dev-only helpers (not needed for normal contributions)

| Path | Purpose |
|------|---------|
| `ghidra/` | Ghidra scripts (`ExportPS1Functions.{java,py}`) to export discovered functions from a Ghidra project as a CSV that `ps1Analyzer` can ingest via `--ghidra-csv`. Plus `start_ghidra_mcp.sh` for the Ghidra MCP integration. |
| `pcsx_redux_mcp/` | Standalone MCP server + Lua scripts that drive PCSX-Redux for cross-checking emulator behaviour against the recompiled runtime. Optional. |
| `ps1_mcp_server.py` | MCP server exposing build/run/log/source tools to AI agents during development. Not part of the runtime build. |
| `demo_build.sh` | Quick smoke build script used while iterating locally. |
| `tests/` | `pytest` tests for `psyq_lib_extract.py` and `extract_psyq_signatures.py`. Run with `pytest tools/tests/`. |

## Generated / external binaries (gitignored)

| Path | Reason |
|------|--------|
| `psyq-obj-parser-bin/psyq-obj-parser` | Built from the [pcsx-redux](https://github.com/grumpycoders/pcsx-redux) `psyq-obj-parser` source. Required by `psyq_lib_extract.py`. Build instructions in the pcsx-redux repo. |

## PsyQ SDK location

The signature generator expects PsyQ SDK `.LIB` archives **outside the repository**
(licensing — we do not redistribute Sony binaries). Default search root is
`$HOME/psyq_sdks/CONSOLIDATED/<version>/lib/`. Override with `--sdk-root`.
