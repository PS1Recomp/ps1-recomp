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

## Issue #1 — Crash Bandicoot: silent hang before first GPU command

**Status:** bypassed (workaround in place; permanent fix in roadmap PLANNING.md Fase 2)
**Game:** Crash Bandicoot (USA, SLUS-00005)
**Branch:** main

### Current state (2026-05-17)

Bypass shipped: env var `PS1_SKIP_31BF8=1` registers a runtime override that
NOPs `func_80031BF8` (the display-mode state machine call that triggers the
unpopulated hash table walk at `0x8005C530`). With the bypass plus the 4 new
function entries added to `crash_recomp.toml` (jumptable targets that
`ps1Analyzer` over-merged into surrounding functions), Crash now boots to the
main loop and runs the full PsyQ render pipeline:

| Metric | Value |
|---|---|
| `libetc_VSync` calls/run | 881 (~58 fps over ~15s) |
| `libgpu_DrawOTag` calls/run | 435 |
| `libgte_MulMatrix` calls/run | 872 |
| `libgpu_PutDispEnv` calls/run | 436 |
| GP1 display swaps | 20 (double-buffer working) |
| Suite | 557/557 green |
| Unknown dispatch targets | 0 |

The root cause (unpopulated hash table — see "Diagnostic history" below) is
unchanged; the bypass is a workaround. The permanent fix is to port the NS
(Naughty Sequence) chunk subsystem from c1c reference as `recomp_register_override`
implementations for `func_80013B94` (`NS_FixupPage`) and `func_80013B30`
(`NS_PageTransition`). See PLANNING.md Fase 2 for the roadmap.

### Symptom

`mcp__ps1-recomp__run_game config=crash.toml duration=15` produces:

| Metric | Value |
|---|---|
| `vsync_calls` | 0 |
| `dma2_count` | 0 |
| `dma3_count` | 0 |
| `gpu_commands` | `[]` (empty) |
| `frame_status` | `I_STAT=0x0041 I_MASK=0x0000` |
| VRAM dump | not produced (no GPU activity) |

Game initialises far enough to issue `SetIntrMask(0)` (mask = 0), then
stalls. No GPU primitive ever emitted.

### Diagnostic history

`PS1_HLE_TRACE=1` + `PS1_INDIRECT_TRACE=1 PS1_INDIRECT_TRACE_RA=0x800163E8`
(see `progress/2026-05-15_crash_*.log`) identified a GTE setup loop at
`recompiled_out.cpp:6793-6807` that dispatches through a function-pointer
table:

| Iter | Target ptr | Behaviour |
|---|---|---|
| 0 | `0x800253A0` | dispatches GTE setup batch 1 → returns |
| 1 | `0x000000A0` | BIOS A0 vector → returns |
| 2 | `0x8001AC60` | dispatches GTE setup batch 2 → returns |
| 3 | `0x80031BF8` | display-mode setup → returns |
| 4+ | – | game never advances past here |

Iterations 0–3 succeed and dispatch 8 GTE calls total. After iter 3
returns, the loop continues but no further PsyQ dispatch ever happens.
The game thread is stuck in a recompiled MIPS function that does not
call PsyQ.

An ASan rebuild (`-DPS1RECOMP_ASAN=ON`, in `build-asan/`) initially
reported `heap-use-after-free` / `double-free` in worker thread T19.
That was a *symptom of shutdown*, not the game-runtime hang — see
fixes #1 and #2 below.

### Fixes landed 2026-05-16

#### Fix #1 — Shutdown destructor UAF (`main_host.cpp:609-620`)

Full ASan trace (`progress/2026-05-16_crash_asan_uaf_full.log`) showed
the read fault came from `Bios::~Bios()` destructing `cdEventQueue_`
while the still-detached game thread was inside
`drainCdromEventQueue()`.

Root cause was a logic error in the cleanup loop:

```cpp
gameFinished.store(true);              // request shutdown
while (!gameFinished.load() && ...) {  // immediately exits — we just set it!
  ...
}
if (gameThread.joinable()) gameThread.detach();  // detaches a running thread
```

`gameFinished` was used both as "shutdown requested" (set by main) and
"thread really exited" (set by game thread). Conflating the roles meant
the wait loop exited the instant after the shutdown request, before the
game thread had a chance to return.

**Fix:** added a second atomic `gameThreadDone` written only by the
game thread, used by cleanup to know when joining is safe. On timeout,
cleanup calls `std::_Exit(0)` instead of `detach()` — skips C++
destructors entirely, avoids the UAF window (the OS reclaims memory
and SDL/audio handles).

#### Fix #2 — Emitter signed-overflow UB (`instruction_emitter.cpp:127, 131, 150`)

UBSan reported `signed integer overflow` at ~13 sites inside
`func_80045E50` and `func_8004636C`, all with the same operands
(`1482998122 + 741499061`). Root cause was the emitter producing
`(int32_t)((int32_t)A + (int32_t)B)` for `ADD`/`ADDU`/`SUB`/`SUBU`/`ADDI`/`ADDIU`
— signed addition is UB in C++ on overflow, but MIPS ADDU/SUBU are
explicitly modular and PSY-Q code relies on modular wrap throughout.

**Fix:** emit through `uint32_t` so the wraparound is well-defined,
then cast back to `int32_t` (a no-op bit reinterpret on 2's-complement
targets). Suite 557/557 still green; UBSan diagnostics for `func_80045E50`
disappear on next regen.

#### Fix #3 — Diagnostic hook (`ps1_runtime_macros.h`)

`CALL_INDIRECT` / `JUMP_INDIRECT` macros gained an env-var-gated trace
hook (`PS1_INDIRECT_TRACE=1` + optional `PS1_INDIRECT_TRACE_RA=<addr>`
filter). Lets us see exact target addresses of indirect dispatch from a
specific call site without rebuilding. The trace that pinpointed iters
0–3 was generated by this hook.

### Still open

The runtime now exits cleanly on `crash.toml` (no UAF, no SIGABRT), but
the **game itself still hangs** at the same point: iter 4+ of the
GTE-setup loop produces no further PsyQ dispatch and no GPU activity.
Latest metrics from `mcp__ps1-recomp__run_game`:

| Metric | Value |
|---|---|
| `vsync_calls` | 0 |
| `dma2_count` | 0 |
| `gpu_commands` | `[]` |
| Exit | clean timeout (code 124, no core dump) |

### Residual hang -- root cause CONFIRMED 2026-05-16

Through a sequence of source-level probes inserted into
`ps1Runtime/src/recompiled_out.cpp` (gated on `PS1_PROBE_15B58=1`),
the hang was traced through 7 nested function calls. The leaf is
`func_80015978` -- a **hash-table linked-list walk** at base address
`0x8005C530`. This matches the prior memory note
`project-crash-hash-table-0x8005c530`.

Diagnostic call chain (each step verified by entry+exit probes):

```
__start -> func_80011D88 -> func_80011FC4 -> func_80015B58
   |
   +-- L_800163C8 loop iter 17 (r16=17) calls 0x80031BF8
       +-- func_80031BF8
           +-- func_80031EB4 (state machine, state=10 at gp+740+13776)
               +-- L_80032048 -> func_80012660 (returns cleanly)
                              -> func_80031D50
                                  +-- func_8001BA18 (returns cleanly)
                                  +-- func_80015118
                                      +-- func_80015978  <-- HANG
```

Bias-corrected metric from probe runs:
- `Bios::drainPendingCallbacks` is called **1M+ times/sec, delta=0**
  (every entry returns). So the runtime is not deadlocked; it is
  spinning in a recompiled MIPS busy loop.

The c1c reference (`PS1Recomp-workspace/c1c/srczz/zz_15978.h`) shows
`func_80015978` is a hash-bucket lookup:

```c
V0 = A0 >> 13;                          // hash key A0 into bucket
V1 = mem[0xFFFFC530 + 0x80060000];      // = mem[0x8005C530] (hash-table base)
V0 &= 0x3FC;
V0 += V1;
V1 = mem[V0];                            // bucket head pointer
V0 = mem[V1 + 4];                        // first entry's key
if (V0 == A0) goto FOUND;
LOOP:
    V1 += 8;
    V0 = mem[V1 + 4];
    if (V0 != A0) goto LOOP;             // walk chain looking for A0
```

The loop terminates only when an entry's key matches `A0`. If the
chain is missing the terminator (i.e., the slot pointer for some key
was never written), the walk runs forever, reading past the chain.

The "slot pointer NUNCA escrito no binario Crash" hypothesis from
the prior memory is now mechanically confirmed: some HLE stub the
game calls during boot is supposed to populate a hash slot for key
`A0` (whatever that argument was when `func_80015978` is called from
`func_80015118` line 5306), and our implementation omits that side
effect.

### Hypothesis updated 2026-05-16 (afternoon)

Further probes refute the "PsyQ HLE missing side effect" theory:

1. `A0 = 0x0057CCFB` at lookup (5-char GOOL token, encoded by
   `func_80012660` from a static string at `0x80056550`).
2. Hash table base at `mem[0x8005C530]` is **never written by any
   instruction in our recompiled output** -- verified with grep for
   `MEM_WRITE32.*-15056` (0 hits) and any direct write to that VA.
3. `mem[0x8005C530]` is in BSS (after `tSize` end at `0x80056800`),
   so it is zero-init by `__start`'s BSS-clear loop
   (`recompiled_out.cpp:55934-55939`, clears `0x80056598..0x800617B8`).
4. PS-EXE header has `dAddr=dSize=bAddr=bSize=0`, so there is no
   secondary data segment that could populate this address from disk.

What's actually missing is not a PsyQ HLE side effect. The hash table
base pointer is supposed to be written by **game code that our
recompiler did not emit, or that lives in a code path we never run**.
Two plausible explanations:

A. **ps1Analyzer/ps1Recomp lost a function** that contains the write.
   The MIPS instruction would be `sw rA, -15056(rB)` where `rB` is
   pre-loaded with `0x80060000`. Verify by disassembling the
   PS-EXE around the boot path with Ghidra and comparing against
   our analyzer's `[[hle_functions]]` + `[[functions]]` lists.

B. **The write happens via DMA**: Crash CD-loads a game data file
   (one of the early sectors landed at `0x80061A80`) whose content
   includes the hash table base pointer for `0x8005C530`. The data
   file is parsed by some boot code that writes the parsed pointer
   to BSS. If our CdRead delivers the sector but the parser is
   stuck on a missing PsyQ HLE earlier, the write never happens.

### Pragmatic patches tried -- all failed to unblock

- **256-iter cap on `func_80015978`** (HASH-PATCH env var): function
  returns r2=0 after walk exceeds 256 iterations. Caller
  (`func_80015118`) writes 0 to the struct field, sets r16=0, jumps
  to `L_80015174` which dereferences r4 = mem[r16+0] = mem[0] -- low
  memory garbage -- and downstream code hangs in a different busy
  loop (no further PsyQ dispatches captured).
- **`func_80015118` early-return -10** (BYPASS env var): short-circuits
  before the lookup. Caller `func_80031D50` then does
  `mem[r2 + 16] = mem[-10 + 16] = mem[6]` -- low-RAM garbage --
  same downstream consequence.

Conclusion: patching the hash lookup is insufficient. Downstream code
treats the returned pointer as a valid struct and dereferences several
fields; substituting a sentinel propagates the failure deeper.

### Real path forward (multi-session)

To get Crash past iter 17, one of the following is required:

1. **Ghidra-assisted analysis** of the Crash boot path (PC =
   `0x8003E018` through to `func_80015B58`) to find the missing
   initialization function that writes `0x8005C530`, then add it
   via `ps1Analyzer --add-func <addr>`.
2. **Implement Crash's data-file parser as an HLE override** that
   reads the CD-loaded data at `0x80061A80` and pre-populates BSS
   addresses including `0x8005C530`. Requires understanding the
   data file format (one of the early sectors of Crash's binary).
3. **Override `func_80031BF8` (display-mode setup) as a no-op**: this
   is the iter-17 target. If we skip it entirely, the GTE loop
   continues to iter 18..20 (mostly null pointers, skip) and exits.
   The game then proceeds past this initialization phase -- though
   subsequent code likely hangs on similar missing inits.

Option 1 is the only one that produces a long-term-correct recompiler.
Options 2 and 3 are TCC-specific workarounds.

### Independent open item

**UBSan: misaligned 4-byte load** at `iso9660.cpp:29` and `:31`.

#### Independent open item

**UBSan: misaligned 4-byte load** at `iso9660.cpp:29` and `:31`. Reading
`uint32_t` directly from a byte buffer that is not 4-aligned. Replace
with `memcpy` into a stack `uint32_t`. Not related to the hang; surface
in any disc-mount path.

### Verification

When the residual hang is fixed:

- 5 consecutive ASan runs of `crash.toml` for 30s each: 0 errors.
- `vsync_calls > 100`, `dma2_count > 500`, `brightness > 100` over a
  15-s headless run.
- VRAM diff against `audit_notes/crash/title_ref.ppm` within tolerance
  via `tools/smoke_test.py`.

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
