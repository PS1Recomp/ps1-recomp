#!/usr/bin/env python3
"""Correct function sizes the analyzer gets wrong for Crash Bandicoot.

`ps1Analyzer` discovers functions from JAL targets.  When a routine has two
entry points that share one body -- a common PsyQ libgte shape -- the second
entry is recorded as a new function and the first is truncated to end where it
begins.  Everything after that first instruction is silently dropped from the
recompiled output.

This is not cosmetic.  0x80042FEC is the libgte vector transform that
func_800180CC calls to fill a stack MATRIX's translation (`a1 = SP+44`).
Truncated to one instruction, the body stopped after `lwc2 $0,0(a0)`, so the
translation was never written; SetTransMatrix then loaded stale stack contents
(RAM pointers) into the GTE's TRX/TRY/TRZ, every RTPT saturated with SZ3 = 0,
and the game's own `if (FLAG < 0) skip polygon` discarded 100% of every actor's
geometry -- no Crash, no Naughty Dog doghouse or banner, no level trail.

Extents verified against the c1c reference decompilation (srczz/zz_42fec.h).

The real fix belongs in ps1Analyzer: an entry point discovered inside an
existing function must not shorten it.  Until then, `tools/regen_crash.sh`
runs this pass so a regeneration does not silently undo the correction.

Usage:
    tools/fix_crash_func_sizes.py configs/crash_recomp.toml
"""

import re
import sys

# address -> (wrong size the analyzer emits, real size in bytes)
CORRECTIONS = {
    "0x80042FEC": (4, 40),
    # 0x800334A0 is the NSF chunk decompressor.  0x80033878 is a second entry
    # point into its body, so the analyzer ends the first function there --
    # but the body branches forward to 0x80033C94, past the cut.  The emitter
    # cannot place code at a label outside the function's extent, so it emits
    #     L_80033C94: recomp_dispatch(rdram, ctx, 0x80033C94); return;
    # and the dispatch aborts at run time: no function *starts* at that
    # address.  Reaching the level-select map is enough to hit it.
    # Real extent runs to 0x80033EF8, where func_80033EF8 begins: 2648 bytes.
    "0x800334A0": (984, 2648),
    # 0x80036BF4 is a hand-written polygon renderer that picks its per-polygon
    # block with `jr $ra`, $ra = 0x80036D9C + index*128.  The analyzer starts a
    # function at the table base (linear sweep; nothing calls it), so the
    # blocks fall outside 0x80036BF4 and its `jr $ra` is emitted as a return --
    # leaving mid-body with $sp still a data register, the defect that froze
    # the Papu Papu fight in the twin routine 0x80036340.  Real extent runs to
    # 0x800375EC, where func_800375EC begins: 2552 bytes.
    "0x80036BF4": (424, 2552),
}


def main() -> int:
    path = sys.argv[1]
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()

    failures = []
    for addr, (wrong, right) in CORRECTIONS.items():
        # Match the size line of the [[functions]] block for this address.
        block = re.compile(
            r"(\[\[functions\]\]\n(?:[^\[]*?\n)?size = )(\d+)((?:[^\[]*?\n)?address = \"%s\"\n)"
            % re.escape(addr))
        m = block.search(text)
        if m is None:
            failures.append("%s: no [[functions]] block found" % addr)
            continue
        current = int(m.group(2))
        if current == right:
            print("  %s already %d" % (addr, right))
            continue
        if current != wrong:
            failures.append(
                "%s: size is %d, expected %d before correction -- the analyzer's "
                "output changed shape, re-verify before overriding"
                % (addr, current, wrong))
            continue
        text = text[:m.start(2)] + str(right) + text[m.end(2):]
        print("  %s size %d -> %d" % (addr, current, right))

    if failures:
        for f in failures:
            print("FAILED: " + f, file=sys.stderr)
        return 1

    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
