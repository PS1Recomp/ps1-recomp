# ps1-recomp — Open Issues (game-specific)

Game-specific blockers. Framework-side improvements live in
`tools/production_line.py status`; finished work is in `git log`.

Format (per [TombaRecomp `ISSUES.md`](https://github.com/mstan/TombaRecomp/blob/master/ISSUES.md)):

```
## Issue #N — <one-line title>

**Status:** open | in-progress | fixed (commit SHA) | wont-fix
**Game:** <game name>
**Branch:** <branch if non-main>

### Symptom

What the user / runtime observes. Include reproducible metrics
(brightness, vsync_calls, dma2_count, exit code).

### Root Cause

What is actually broken. Cite files:lines. If unknown, "diagnosing"
and the active hypothesis.

### Fix

How it was resolved. Commit SHAs. If still open, the planned fix.

### Verification

How we know the fix works — before/after metrics, screenshot diff,
test that catches regressions.
```

---

## Issue #1 — Crash Bandicoot: hash table walk hangs without GOOL interpreter

**Status:** fixed (`d04dcd7`; workaround removed in `ca2c735`)
**Game:** Crash Bandicoot (USA, SCUS-94900)

### Symptom

Boot stalled in `func_80015B58` walking the hash table at `0x8005C530`;
`PS1_SKIP_31BF8=1` bypassed it at the cost of a racy render path.

### Root Cause

The game's own GOOL bytecode interpreter (`func_800201DC`) did not run
correctly: its jump table was sized wrong, so dispatch fell off the table.
GOOL never needed to be reimplemented.

### Fix

Jump table size is derived from the `sltiu` bounds check (`d04dcd7`). The
native interpreter is the default; `PS1_SKIP_31BF8` was dropped (`ca2c735`).

### Verification

Crash boots through menus and map into the levels; levels 1–3 were finished
by a human player and replay frame-exactly in lockstep mode.

---

## Issue #2 — Rayman regen-fresh: brightness=0 / dma2=5

**Status:** open
**Game:** Rayman (USA, SLUS-00005)
**Branch:** main

### Symptom

`mcp__ps1-recomp__run_game config=rayman.toml duration=15`:

| Metric | Value |
|---|---|
| `vsync_calls` | 0 |
| `dma2_count` | 5 |
| `gpu_commands` | 5 FillRect + 5 LinkedList (each with empty GP0) |
| `I_MASK` | `0x0040` (CDROM only, no VBlank) |
| brightness | 0 |

The legacy `tools/patch_rayman.py` path used to render fine; that script
was retired during open-source preparation (commit `ab6558b`). Since
then the regen-fresh path has produced this output.

### Root Cause

Diagnosing. Discovery in session 2026-05-15: 11 libetc interrupt /
callback HLEs (`ResetCallback`, `VSyncCallback`, `SetIntrMask`, …) are
implemented and registered in `psyq_register_rayman_boot()`, but
zero `psyq_dispatch("libetc_…")` calls appear in Rayman's
`recompiled_out.cpp`. The analyzer is not detecting them in the
Rayman binary — hash mismatch, inlining, or matcher dedupe drop are
candidates.

### Fix

Pending. Either (a) re-run analyzer with permissive matching that
surfaces the missed addresses, or (b) `--add-func` manual entries
for known libetc addresses in Rayman.

### Verification

When fixed:

- `psyq_dispatch("libetc_VSyncCallback"` present in regen-fresh
  `recompiled_out.cpp`
- `vsync_calls > 0`, brightness > 50 within 15 s

---

## Issue #3 — Crash Bandicoot: level 1 fork camera does not turn

**Status:** open
**Game:** Crash Bandicoot (USA, SCUS-94900)

### Symptom

At the first level's fork, taking the left path first leaves the camera
facing the old direction. Right-then-left works.

### Root Cause

Diagnosing. Not retested since the root-counter (`40d7695`) and GTE
square root (`d831d38`) fixes, which both changed gameplay.

### Fix

Pending.

### Verification

Left path first at the fork turns the camera; a recorded run of it replays
frame-exactly.

---

## Issue #4 — Crash Bandicoot: warp pads fire early, enemies missing, sliding floors

**Status:** fixed (`d831d38`)
**Game:** Crash Bandicoot (USA, SCUS-94900)

### Symptom

In The Great Gate the exit warp fired with Crash ~14 m away; some enemies
never appeared; floors kept Crash sliding as if on ice.

### Root Cause

The emitter translated `MFC2`/`MTC2`/`LWC2`/`SWC2` as raw `cop2d[]` array
accesses, bypassing the GTE. LZCR (leading-zero count of LZCS) was never
computed, so libgte `SquareRoot0` (`0x80042B9C`) returned 0 and every GOOL
distance was 0. The warp pad's `distance < 1.6m` test passed as soon as it
spawned.

### Fix

GTE data moves go through `gte_read_data`/`gte_write_data`, which call
`GTE::readData`/`writeData` (LZCR, SXYP push, IRGB).

### Verification

Emitter and macro tests; The Great Gate and Jungle Rollers finished by a
human player with warp pads and enemies where expected.

---

## Issue #5 — Crash Bandicoot: slow motion in busy levels

**Status:** fixed (`40d7695`)
**Game:** Crash Bandicoot (USA, SCUS-94900)

### Symptom

Busy levels ran at 75–79% speed with a 20/30 fps cadence.

### Root Cause

Root-counter ticks (Crash's game clock) were credited only while the game
waited in `VSync`; VBlanks that passed during a frame's work were lost.

### Fix

Ticks are credited once per VBlank on the game thread.

### Verification

`PS1_PACING_LOG`: ~16.8 ticks per VBlank (target 17) and a steady 30 fps
in every level; CPU time 4–9 ms per frame against a 33 ms budget.

