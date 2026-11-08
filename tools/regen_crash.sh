#!/bin/bash
# Reproduces configs/crash_recomp.toml from the Crash Bandicoot 1 binary.
# Persists the --add-func entries that ps1Analyzer's heuristics miss
# (jumptable targets and a VBlank-counter symbol the prologue scan over-merges).
#
# GOOL interpreter: as of commit d04dcd7 (jump-table sizing fix), the game's
# own interpreter (func_800201DC) recompiles and runs correctly, so it is the
# default -- there is no [hle_overrides] entry for it unless requested. The
# hand-ported VM in ps1Runtime/src/gool/ remains in the build for comparison
# and is reachable with --gool-hle.
#
# Usage:
#   tools/regen_crash.sh             # regen toml only (native GOOL)
#   tools/regen_crash.sh --recomp    # regen toml + run ps1Recomp
#   tools/regen_crash.sh --extra 0x80013B94 0x80013B30   # add transient adds
#   tools/regen_crash.sh --gool-hle --recomp   # replace the native GOOL
#       interpreter (0x800201DC) with the hand-ported VM via [hle_overrides]

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
ANALYZER="$BUILD_DIR/ps1Analyzer/ps1Analyzer"
RECOMP="$BUILD_DIR/ps1Recomp/ps1Recomp"

DISC="$PROJECT_DIR/test_roms/Crash Bandicoot /Crash Bandicoot (USA).bin"
OUT_TOML="$PROJECT_DIR/configs/crash_recomp.toml"
OUT_CPP="$PROJECT_DIR/ps1Runtime/src/recompiled_out.cpp"

# Persistent --add-func entries the analyzer's heuristics miss for Crash.
# 0x80034504 — VBlank counter increment helper (prologue scan over-merges it
#              into the surrounding function)
# 0x80016C18 — jumptable target inside the GTE setup loop
# 0x8002D638 — jumptable target inside the DMA init path
# 0x8002E8A4 — jumptable target inside the BSS clear loop
# 0x8001AC60 — jumptable target inside the GTE pointer setup
# 0x80025628 — jumptable target inside the post-NSD init dispatch
#              (revealed after SWL/SWR emitter fix populated chunk[25])
# 0x800466A0 — SPU transfer completion poller. Registered via
#              DMACallback(4, fn) (DMA channel 4 = SPU); only reached through
#              the callback table so the prologue scan over-merges it into
#              func_8004636C.
#              Delivers Event(0xF0000009, 0x20) that NS_waitForAllLoads
#              slots 10/11 poll via TestEvent(9).
# 0x800256DC — sibling of 0x80025628 in the post-NSD dispatch family
#              (dropped dispatch observed once main loop runs, RA=0x80013068)
ADD_FUNCS=(
  0x80034504
  0x80016C18
  0x8002D638
  0x8002E8A4
  0x8001AC60
  0x80025628
  0x800256DC
  0x800466A0
)

EXTRA_FUNCS=()
RUN_RECOMP=0
GOOL_HLE=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --recomp) RUN_RECOMP=1; shift ;;
    --gool-hle) GOOL_HLE=1; shift ;;
    --extra)
      shift
      while [[ $# -gt 0 && "$1" != --* ]]; do
        EXTRA_FUNCS+=("$1"); shift
      done
      ;;
    -h|--help)
      sed -n '2,12p' "$0"; exit 0 ;;
    *)
      echo "unknown flag: $1" >&2; exit 1 ;;
  esac
done

if [[ ! -x "$ANALYZER" ]]; then
  echo "ps1Analyzer not built at $ANALYZER" >&2
  echo "  cmake --build $BUILD_DIR --target ps1Analyzer -j$(nproc)" >&2
  exit 1
fi
if [[ ! -f "$DISC" ]]; then
  echo "Crash disc not found at $DISC" >&2
  exit 1
fi

ARGS=("$DISC" "$OUT_TOML")
for addr in "${ADD_FUNCS[@]}" "${EXTRA_FUNCS[@]:-}"; do
  [[ -z "$addr" ]] && continue
  ARGS+=(--add-func "$addr")
done

echo "[regen_crash] running ps1Analyzer with ${#ADD_FUNCS[@]} persistent + ${#EXTRA_FUNCS[@]} extra --add-func entries"
"$ANALYZER" "${ARGS[@]}"

# Function-size corrections (2026-08-26) -- this is what made actors render.
# The analyzer truncates a function when a second entry point into its body
# is discovered as a JAL target; see tools/fix_crash_func_sizes.py for the
# full story on 0x80042FEC and the GTE translation it silently dropped.
echo "[regen_crash] correcting analyzer function sizes"
"$PROJECT_DIR/tools/fix_crash_func_sizes.py" "$OUT_TOML"

# Wrappers the hash matcher CANNOT detect (T2, 2026-07-07):
# 0x80043498 / 0x80043984 are 8-instr wrappers in the dropped collision
# group (Task #25); 0x8003E754 is a trampoline through the libetc vtable
# (statically 0 in the binary). Route them to the HLE bodies explicitly.
echo "[regen_crash] appending [[hle_functions]]: CdSync/CdReadSync/InterruptCallback/DMACallback"
cat >> "$OUT_TOML" <<'EOF'

[[hle_functions]]
subsystem = "CD-ROM"
name = "libcd_CdSync"
address = "0x80043498"
hle = true
stub_type = "recompile"

[[hle_functions]]
subsystem = "CD-ROM"
name = "libcd_CdReadSync"
address = "0x80043984"
hle = true
stub_type = "recompile"

# The three libetc wrappers at 0x8003E6F4/24/54 differ only in the vtable
# offset they call through (start/set/cb), so the masked signature cannot
# tell them apart.  0x8003E724 is InterruptCallback (vtable +8) and 0x8003E754
# is DMACallback (vtable +4) -- the SPU library's transfer poller arrives
# through the latter, libsnd's VSync sequencer tick through the former.
[[hle_functions]]
subsystem = "Other"
name = "libetc_InterruptCallback"
address = "0x8003E724"
hle = true
stub_type = "recompile"

[[hle_functions]]
subsystem = "Other"
name = "libetc_DMACallback"
address = "0x8003E754"
hle = true
stub_type = "recompile"
EOF

if [[ "$GOOL_HLE" -eq 1 ]]; then
  echo "[regen_crash] appending [hle_overrides]: 0x800201DC -> hle_gool_InterpretObject"
  cat >> "$OUT_TOML" <<'EOF'

# GOOL bytecode VM replaced by the HLE interpreter (gool_interp.cpp).
# Body-level override so direct JAL callers are intercepted too.
[hle_overrides]
"0x800201DC" = "hle_gool_InterpretObject"
EOF
fi

if [[ "$RUN_RECOMP" -eq 1 ]]; then
  if [[ ! -x "$RECOMP" ]]; then
    echo "ps1Recomp not built at $RECOMP" >&2
    echo "  cmake --build $BUILD_DIR --target ps1Recomp -j$(nproc)" >&2
    exit 1
  fi
  echo "[regen_crash] running ps1Recomp -> $OUT_CPP"
  "$RECOMP" "$OUT_TOML" "$OUT_CPP"

  # The decompressor's callback yield points used to have to be suppressed
  # here (tools/suppress_decomp_drain.py, since deleted).  What actually
  # damaged the decompression was the CDROM sector hand-off racing the render
  # thread, not the callback: with that serialised, 18 consecutive runs load
  # the level with the drains left in place.
fi

echo "[regen_crash] done."
