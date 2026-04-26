#!/usr/bin/env python3
"""
Patch build/rayman_recompiled.cpp for use as runtime/src/recompiled_out.cpp.
Applies minimal HLE fixes:
  1. Add #include <thread>, <chrono>, <runtime/psyq/psyq_hle.h>
  2. HLE DrawSync: func_801AA484 returns 0 immediately (GPU is synchronous)
  3. Add drainPendingCallbacks() in 4 polling loops of func_80132A44
  4. Add func_8019F848: missing 2-instr prefix trampoline for func_8019F850
  5. Add func_801BE0C0: SetTexWindow builder (dead code in func_801BE0B4)
  6. Register func_8019F848 and func_801BE0C0 in dispatch table
  7. Add drainPendingCallbacks() in L_8019FA1C spin loop of func_8019F8D0
  8. Add func_801AA3F4: dead-code block in func_801AA3E8 at 0x801AA3F4
     (DrawSync array clear + index update, called from func_801AA37C)
  9. Patch func_80134C14 JUMP_INDIRECT: inline handler for embedded MIPS at 0x80134C48
     (jump table case that reads controller bytes → [0x801CEEFC])
  10. Register func_801AA3F4 in dispatch table
  11. Skip FMV intro: func_80132898 returns immediately (game stuck in VLC decode loop)
  12. HLE VSync wait: func_801B954C sleeps 1ms/iter instead of busy-loop 32768×
      (original loop completes in ~30µs, far too fast for 16.67ms VBlank period)
  13. Add func_801B1AE4: dead-code mid-function entry point in func_801B18C4
     (entity dispatch handler — calls func_801B1F7C with sign_extend(a0), sign_extend(a1), a2)
  14. Add func_8019F848: animation counter increment + frame tick
     (dead-code entry at 0x8019F848 inside func_8019F3A0's data block)
  15. Add func_801BEAD0 + func_801BEB08: GPU indexed-buffer ops (defined only,
     NOT dispatched — func_801B8DFC init is missing so ptr at 0x801CF31C is 0)
  16. HLE PutDispEnv: func_801BD69C calls HLE directly, bypassing broken
     PsyQ GPU function-pointer chain (func_801B8DFC initializer is missing,
     so ptr at 0x801CF300+16 is garbage → GP1 commands never sent → display
     area never updated from initial (0,0) → wrong framebuffer shown)
  17. Synthesize func_801B8DFC: GPU HAL struct initializer (was dead code).
     Creates GP0/GP1 write stubs (func_801B8E00/08), builds 64-byte HAL
     struct at 0x801D3000, writes pointer to [0x801CF300]. Called from
     func_801C104C (display pipeline) with guard at [0x801CF2D4].
"""

import sys
import re

SRC = "build/rayman_recompiled.cpp"
DST = "ps1Runtime/src/recompiled_out.cpp"

print(f"Reading {SRC}...")
with open(SRC, "r") as f:
    text = f.read()

# ─── 1. Add required includes ───────────────────────────────────────────────
OLD_INCLUDES = '#include <cstdint>\n'
NEW_INCLUDES = '#include <cstdint>\n#include <atomic>\n#include <chrono>\n#include <thread>\n'
if '#include <thread>' not in text:
    text = text.replace(OLD_INCLUDES, NEW_INCLUDES, 1)
    print("  [1a] Added <atomic>, <chrono> and <thread> includes")
elif '#include <atomic>' not in text:
    text = text.replace('#include <chrono>\n', '#include <atomic>\n#include <chrono>\n', 1)
    print("  [1a] Added <atomic> include")
else:
    print("  [1a] <thread>/<atomic> already included")

OLD_INCLUDES2 = '#include <runtime/bios/bios.h>\n'
NEW_INCLUDES2 = '#include <runtime/bios/bios.h>\n#include <runtime/psyq/psyq_hle.h>\n'
if '#include <runtime/psyq/psyq_hle.h>' not in text:
    text = text.replace(OLD_INCLUDES2, NEW_INCLUDES2, 1)
    print("  [1b] Added psyq_hle.h include")
else:
    print("  [1b] psyq_hle.h already included")

# ─── 2. HLE DrawSync: replace func_801AA484 body ────────────────────────────
# Find the function body and replace it entirely
OLD_DRAWSYNC_START = 'void func_801AA484(uint8_t* rdram, recomp_context* ctx) {\n    ctx->r2 = 0x801D0000;\n    ctx->r2 = MEM_READ32(ctx, (int32_t)(ctx->r2) + -2792);\n'
NEW_DRAWSYNC_BODY = (
    'void func_801AA484(uint8_t* rdram, recomp_context* ctx) {\n'
    '    // HLE DrawSync: GPU is synchronous — drawing is always complete\n'
    '    ps1::psyq::hle_DrawSync(ctx);\n'
    '    return;\n'
    '    // Original code unreachable — kept for reference\n'
    '    ctx->r2 = 0x801D0000;\n'
    '    ctx->r2 = MEM_READ32(ctx, (int32_t)(ctx->r2) + -2792);\n'
)
if OLD_DRAWSYNC_START in text:
    text = text.replace(OLD_DRAWSYNC_START, NEW_DRAWSYNC_BODY, 1)
    print("  [2] HLE DrawSync: func_801AA484 patched")
else:
    print("  [2] WARNING: func_801AA484 pattern not found — DrawSync not patched!")

# ─── 3. Add drainPendingCallbacks in polling loops of func_80132A44 ─────────
# Loop 1: L_80132BF0 — polls byte until non-zero
OLD_LOOP1 = ('L_80132BF0:\n'
             '    ctx->r2 = (uint8_t)MEM_READ8(ctx, (int32_t)(ctx->r16) + 0);\n'
             '    // NOP\n'
             '    // NOP // delay slot\n'
             '    if ((int32_t)(ctx->r2) == (int32_t)((int32_t)0)) goto L_80132BF0;\n')
NEW_LOOP1 = ('L_80132BF0:\n'
             '    if (ctx->bios) ctx->bios->drainPendingCallbacks();\n'
             '    ctx->r2 = (uint8_t)MEM_READ8(ctx, (int32_t)(ctx->r16) + 0);\n'
             '    // NOP\n'
             '    // NOP // delay slot\n'
             '    if ((int32_t)(ctx->r2) == (int32_t)((int32_t)0)) goto L_80132BF0;\n')
if OLD_LOOP1 in text:
    text = text.replace(OLD_LOOP1, NEW_LOOP1, 1)
    print("  [3a] drainPendingCallbacks in L_80132BF0")
else:
    print("  [3a] WARNING: L_80132BF0 pattern not found")

# Loop 2: L_80132C08 — polls byte until zero
OLD_LOOP2 = ('L_80132C08:\n'
             '    ctx->r2 = (uint8_t)MEM_READ8(ctx, (int32_t)(ctx->r3) + 0);\n'
             '    // NOP\n'
             '    // NOP // delay slot\n'
             '    if ((int32_t)(ctx->r2) != (int32_t)((int32_t)0)) goto L_80132C08;\n')
NEW_LOOP2 = ('L_80132C08:\n'
             '    if (ctx->bios) ctx->bios->drainPendingCallbacks();\n'
             '    ctx->r2 = (uint8_t)MEM_READ8(ctx, (int32_t)(ctx->r3) + 0);\n'
             '    // NOP\n'
             '    // NOP // delay slot\n'
             '    if ((int32_t)(ctx->r2) != (int32_t)((int32_t)0)) goto L_80132C08;\n')
if OLD_LOOP2 in text:
    text = text.replace(OLD_LOOP2, NEW_LOOP2, 1)
    print("  [3b] drainPendingCallbacks in L_80132C08")
else:
    print("  [3b] WARNING: L_80132C08 pattern not found")

# Loop 3: L_80132CB0 — polls byte until non-zero
OLD_LOOP3 = ('L_80132CB0:\n'
             '    ctx->r2 = (uint8_t)MEM_READ8(ctx, (int32_t)(ctx->r16) + 0);\n'
             '    // NOP\n'
             '    // NOP // delay slot\n'
             '    if ((int32_t)(ctx->r2) == (int32_t)((int32_t)0)) goto L_80132CB0;\n')
NEW_LOOP3 = ('L_80132CB0:\n'
             '    if (ctx->bios) ctx->bios->drainPendingCallbacks();\n'
             '    ctx->r2 = (uint8_t)MEM_READ8(ctx, (int32_t)(ctx->r16) + 0);\n'
             '    // NOP\n'
             '    // NOP // delay slot\n'
             '    if ((int32_t)(ctx->r2) == (int32_t)((int32_t)0)) goto L_80132CB0;\n')
if OLD_LOOP3 in text:
    text = text.replace(OLD_LOOP3, NEW_LOOP3, 1)
    print("  [3c] drainPendingCallbacks in L_80132CB0")
else:
    print("  [3c] WARNING: L_80132CB0 pattern not found")

# Loop 4: L_80132CC8 — polls byte until zero
OLD_LOOP4 = ('L_80132CC8:\n'
             '    ctx->r2 = (uint8_t)MEM_READ8(ctx, (int32_t)(ctx->r3) + 0);\n'
             '    // NOP\n'
             '    // NOP // delay slot\n'
             '    if ((int32_t)(ctx->r2) != (int32_t)((int32_t)0)) goto L_80132CC8;\n')
NEW_LOOP4 = ('L_80132CC8:\n'
             '    if (ctx->bios) ctx->bios->drainPendingCallbacks();\n'
             '    ctx->r2 = (uint8_t)MEM_READ8(ctx, (int32_t)(ctx->r3) + 0);\n'
             '    // NOP\n'
             '    // NOP // delay slot\n'
             '    if ((int32_t)(ctx->r2) != (int32_t)((int32_t)0)) goto L_80132CC8;\n')
if OLD_LOOP4 in text:
    text = text.replace(OLD_LOOP4, NEW_LOOP4, 1)
    print("  [3d] drainPendingCallbacks in L_80132CC8")
else:
    print("  [3d] WARNING: L_80132CC8 pattern not found")

# ─── 4. Add func_8019F848 forward declaration ───────────────────────────────
# Insert after func_8019F3A8 forward decl; this is a 2-instr prefix trampoline
# that loads byte counter into r2 then falls through to func_8019F850.
OLD_FWD_8019F850 = 'void func_8019F850(uint8_t* rdram, recomp_context* ctx);\n'
NEW_FWD_8019F850 = (
    'void func_8019F848(uint8_t* rdram, recomp_context* ctx);\n'
    'void func_8019F850(uint8_t* rdram, recomp_context* ctx);\n'
)
if 'void func_8019F848' not in text:
    if OLD_FWD_8019F850 in text:
        text = text.replace(OLD_FWD_8019F850, NEW_FWD_8019F850, 1)
        print("  [4a] Forward decl func_8019F848 added")
    else:
        print("  [4a] WARNING: func_8019F850 forward decl not found")
else:
    print("  [4a] func_8019F848 already declared")

# Insert func_8019F848 implementation before func_8019F850 definition.
# MIPS at 0x8019F848: LUI r2, 0x801D; LBU r2, -3016(r2)
# Then execution falls through to func_8019F850 which uses r2 += 1.
OLD_FUNC_8019F850_DEF = 'void func_8019F850(uint8_t* rdram, recomp_context* ctx) {\n'
NEW_FUNC_8019F848_IMPL = (
    '// func_8019F848: 2-instruction trampoline — loads animation counter into\n'
    '// r2 then falls through to func_8019F850 (increment + call func_801809FC).\n'
    '// MIPS: LUI r2,0x801D; LBU r2,-3016(r2)  → r2 = MEM_READ8(0x801CF438)\n'
    'void func_8019F848(uint8_t* rdram, recomp_context* ctx) {\n'
    '    ctx->r2 = (uint8_t)MEM_READ8(ctx, 0x801CF438);\n'
    '    func_8019F850(rdram, ctx);\n'
    '}\n'
    '\n'
)
if '    func_8019F850(rdram, ctx);\n}\n' not in text:
    if OLD_FUNC_8019F850_DEF in text:
        text = text.replace(OLD_FUNC_8019F850_DEF,
                            NEW_FUNC_8019F848_IMPL + OLD_FUNC_8019F850_DEF, 1)
        print("  [4b] func_8019F848 implementation added before func_8019F850")
    else:
        print("  [4b] WARNING: func_8019F850 definition not found")
else:
    print("  [4b] func_8019F848 already defined")

# ─── 5. Add func_801BE0C0 forward declaration and implementation ─────────────
# func_801BE0C0 is the SetTexWindow GP0 command builder.  It is the branch
# target of func_801BE0B0 (BNE R4,R0,0x801BE0C0 + delay-slot SP-=16).
# The actual code lives in func_801BE0B4 as dead-code after an unconditional
# branch — the recompiler never created a labelled entry point for it.
OLD_FWD_801BE0B4 = 'void func_801BE0B4(uint8_t* rdram, recomp_context* ctx);\n'
NEW_FWD_801BE0B4 = (
    'void func_801BE0B4(uint8_t* rdram, recomp_context* ctx);\n'
    'void func_801BE0C0(uint8_t* rdram, recomp_context* ctx);\n'
)
if 'void func_801BE0C0' not in text:
    if OLD_FWD_801BE0B4 in text:
        text = text.replace(OLD_FWD_801BE0B4, NEW_FWD_801BE0B4, 1)
        print("  [5a] Forward decl func_801BE0C0 added")
    else:
        print("  [5a] WARNING: func_801BE0B4 forward decl not found")
else:
    print("  [5a] func_801BE0C0 already declared")

# Insert func_801BE0C0 implementation after func_801BE0B4 definition.
# The code is extracted from the dead-code block in func_801BE0B4 (after the
# goto L_801BE12C).  The delay-slot ADDIU SP,-16 from func_801BE0B0 must be
# included as the prolog here.
OLD_AFTER_801BE0B4 = (
    'void func_801BE0B4(uint8_t* rdram, recomp_context* ctx) {\n'
    '    ctx->r29 = (int32_t)((int32_t)(ctx->r29) + -16);\n'
    '    ctx->r2 = (int32_t)((int32_t)((int32_t)0) + (int32_t)((int32_t)0)); // delay slot\n'
    '    goto L_801BE12C;\n'
    '    ctx->r5 = (uint8_t)MEM_READ8(ctx, (int32_t)(ctx->r4) + 0);\n'
    '    // NOP\n'
    '    ctx->r5 = (int32_t)((uint32_t)(ctx->r5) >> 3);\n'
    '    MEM_WRITE32(ctx, (int32_t)(ctx->r29) + 0, ctx->r5);\n'
    '    ctx->r6 = MEM_READ16(ctx, (int32_t)(ctx->r4) + 4);\n'
    '    // NOP\n'
    '    ctx->r6 = (int32_t)((int32_t)((int32_t)0) - (int32_t)(ctx->r6));\n'
    '    ctx->r6 = ctx->r6 & 0x00FF;\n'
    '    ctx->r6 = (int32_t)(ctx->r6) >> 3;\n'
    '    MEM_WRITE32(ctx, (int32_t)(ctx->r29) + 8, ctx->r6);\n'
    '    ctx->r2 = (uint8_t)MEM_READ8(ctx, (int32_t)(ctx->r4) + 2);\n'
    '    // NOP\n'
    '    ctx->r2 = (int32_t)((uint32_t)(ctx->r2) >> 3);\n'
    '    MEM_WRITE32(ctx, (int32_t)(ctx->r29) + 4, ctx->r2);\n'
    '    ctx->r3 = MEM_READ16(ctx, (int32_t)(ctx->r4) + 6);\n'
    '    ctx->r5 = (int32_t)((uint32_t)(ctx->r5) << 10);\n'
    '    ctx->r2 = (int32_t)((uint32_t)(ctx->r2) << 15);\n'
    '    ctx->r4 = 0xE2000000;\n'
    '    ctx->r5 = ctx->r5 | ctx->r4;\n'
    '    ctx->r2 = ctx->r2 | ctx->r5;\n'
    '    ctx->r3 = (int32_t)((int32_t)((int32_t)0) - (int32_t)(ctx->r3));\n'
    '    ctx->r3 = ctx->r3 & 0x00FF;\n'
    '    ctx->r3 = (int32_t)(ctx->r3) >> 3;\n'
    '    MEM_WRITE32(ctx, (int32_t)(ctx->r29) + 12, ctx->r3);\n'
    '    ctx->r3 = (int32_t)((uint32_t)(ctx->r3) << 5);\n'
    '    ctx->r2 = ctx->r2 | ctx->r3;\n'
    '    ctx->r2 = ctx->r2 | ctx->r6;\n'
    'L_801BE12C:\n'
    '    ctx->r29 = (int32_t)((int32_t)(ctx->r29) + 16); // delay slot\n'
    '    return;\n'
    '}\n'
)
NEW_AFTER_801BE0B4 = OLD_AFTER_801BE0B4 + (
    '\n'
    '// func_801BE0C0: SetTexWindow GP0 (0xE2) command builder.\n'
    '// Entry point reached via func_801BE0B0 BNE branch.\n'
    '// Delay slot (ADDIU SP,-16) from func_801BE0B0 is emitted here as the prolog.\n'
    '// r4 = pointer to PsyQ TWINDOW struct { uint8_t w,h, x,y; int16_t ox,oy }.\n'
    '// Returns GP0 0xE2 command word in r2.\n'
    'void func_801BE0C0(uint8_t* rdram, recomp_context* ctx) {\n'
    '    ctx->r29 = (int32_t)((int32_t)(ctx->r29) + -16); // delay slot from func_801BE0B0\n'
    '    ctx->r5 = (uint8_t)MEM_READ8(ctx, (int32_t)(ctx->r4) + 0);\n'
    '    // NOP\n'
    '    ctx->r5 = (int32_t)((uint32_t)(ctx->r5) >> 3);\n'
    '    MEM_WRITE32(ctx, (int32_t)(ctx->r29) + 0, ctx->r5);\n'
    '    ctx->r6 = MEM_READ16(ctx, (int32_t)(ctx->r4) + 4);\n'
    '    // NOP\n'
    '    ctx->r6 = (int32_t)((int32_t)((int32_t)0) - (int32_t)(ctx->r6));\n'
    '    ctx->r6 = ctx->r6 & 0x00FF;\n'
    '    ctx->r6 = (int32_t)(ctx->r6) >> 3;\n'
    '    MEM_WRITE32(ctx, (int32_t)(ctx->r29) + 8, ctx->r6);\n'
    '    ctx->r2 = (uint8_t)MEM_READ8(ctx, (int32_t)(ctx->r4) + 2);\n'
    '    // NOP\n'
    '    ctx->r2 = (int32_t)((uint32_t)(ctx->r2) >> 3);\n'
    '    MEM_WRITE32(ctx, (int32_t)(ctx->r29) + 4, ctx->r2);\n'
    '    ctx->r3 = MEM_READ16(ctx, (int32_t)(ctx->r4) + 6);\n'
    '    ctx->r5 = (int32_t)((uint32_t)(ctx->r5) << 10);\n'
    '    ctx->r2 = (int32_t)((uint32_t)(ctx->r2) << 15);\n'
    '    ctx->r4 = 0xE2000000;\n'
    '    ctx->r5 = ctx->r5 | ctx->r4;\n'
    '    ctx->r2 = ctx->r2 | ctx->r5;\n'
    '    ctx->r3 = (int32_t)((int32_t)((int32_t)0) - (int32_t)(ctx->r3));\n'
    '    ctx->r3 = ctx->r3 & 0x00FF;\n'
    '    ctx->r3 = (int32_t)(ctx->r3) >> 3;\n'
    '    MEM_WRITE32(ctx, (int32_t)(ctx->r29) + 12, ctx->r3);\n'
    '    ctx->r3 = (int32_t)((uint32_t)(ctx->r3) << 5);\n'
    '    ctx->r2 = ctx->r2 | ctx->r3;\n'
    '    ctx->r2 = ctx->r2 | ctx->r6;\n'
    '    ctx->r29 = (int32_t)((int32_t)(ctx->r29) + 16); // epilog\n'
    '    return;\n'
    '}\n'
)
if '// func_801BE0C0: SetTexWindow GP0' not in text:
    if OLD_AFTER_801BE0B4 in text:
        text = text.replace(OLD_AFTER_801BE0B4, NEW_AFTER_801BE0B4, 1)
        print("  [5b] func_801BE0C0 implementation added after func_801BE0B4")
    else:
        print("  [5b] WARNING: func_801BE0B4 body pattern not found")
else:
    print("  [5b] func_801BE0C0 already defined")

# ─── 8. Add func_801AA3F4 forward declaration and implementation ─────────────
# func_801AA3F4 is the dead-code block inside func_801AA3E8 starting at 0x3F4.
# It clears up to r6 entries in the sprite array starting at index r4,
# then updates the DrawSync index at [0x801CF518] (0x801D0000-2792).
# Called from func_801AA37C via recomp_dispatch when status word == 0x0004.
OLD_FWD_801AA3E8 = 'void func_801AA37C(uint8_t* rdram, recomp_context* ctx);\nvoid func_801AA3E8(uint8_t* rdram, recomp_context* ctx);\n'
NEW_FWD_801AA3E8 = (
    'void func_801AA37C(uint8_t* rdram, recomp_context* ctx);\n'
    'void func_801AA3F4(uint8_t* rdram, recomp_context* ctx);\n'
    'void func_801AA3E8(uint8_t* rdram, recomp_context* ctx);\n'
)
if 'void func_801AA3F4' not in text:
    if OLD_FWD_801AA3E8 in text:
        text = text.replace(OLD_FWD_801AA3E8, NEW_FWD_801AA3E8, 1)
        print("  [8a] Forward decl func_801AA3F4 added")
    else:
        print("  [8a] WARNING: func_801AA3E8 forward decl context not found")
else:
    print("  [8a] func_801AA3F4 already declared")

# Insert func_801AA3F4 implementation before func_801AA3E8 definition.
# The MIPS at 0x801AA3F4 is the dead code in func_801AA3E8 after the initial B+delay.
# r4=start_index, r5=0 (MIPS delay slot forces r5=0), r6=signed 16-bit count.
OLD_FUNC_801AA3E8_DEF = 'void func_801AA3E8(uint8_t* rdram, recomp_context* ctx) {\n'
NEW_FUNC_801AA3F4_IMPL = (
    '// func_801AA3F4: sprite-array clear + DrawSync index update.\n'
    '// Extracted from dead code in func_801AA3E8 (0x801AA3F4–0x801AA43F).\n'
    '// r4=start_index, r5=0 (caller delay slot), r6=count (signed 16-bit).\n'
    '// Clears r6 word[0] entries in the sprite table, then writes (r5+r4)\n'
    '// to the DrawSync index at [0x801D0000-2792] == [0x801CF518]. r2 -> 0.\n'
    'void func_801AA3F4(uint8_t* rdram, recomp_context* ctx) {\n'
    '    ctx->r2 = (int32_t)((uint32_t)(ctx->r6) << 16);\n'
    '    ctx->r2 = (int32_t)(ctx->r2) >> 16;\n'
    '    ctx->r5 = (int32_t)0; // delay slot: r5 = 0\n'
    '    if ((int32_t)(ctx->r2) <= 0) goto L_801AA3F4_done;\n'
    '    ctx->r6 = (int32_t)(ctx->r2);\n'
    '    ctx->r2 = (int32_t)((int32_t)(ctx->r5) + (int32_t)(ctx->r4));\n'
    'L_801AA3F4_loop:\n'
    '    ctx->r3 = 0x801F0000;\n'
    '    ctx->r3 = MEM_READ32(ctx, (int32_t)(ctx->r3) + 21552);\n'
    '    ctx->r5 = (int32_t)((int32_t)(ctx->r5) + 1);\n'
    '    ctx->r2 = (int32_t)((uint32_t)(ctx->r2) << 5);\n'
    '    ctx->r2 = (int32_t)((int32_t)(ctx->r2) + (int32_t)(ctx->r3));\n'
    '    MEM_WRITE16(ctx, (int32_t)(ctx->r2) + 0, (int32_t)0);\n'
    '    ctx->r2 = ((int32_t)(ctx->r5) < (int32_t)(ctx->r6)) ? 1 : 0;\n'
    '    {\n'
    '      uint32_t _ds_r2 = ctx->r2;\n'
    '      ctx->r2 = (int32_t)((int32_t)(ctx->r5) + (int32_t)(ctx->r4)); // delay slot\n'
    '      if ((int32_t)(_ds_r2) != (int32_t)((int32_t)0)) goto L_801AA3F4_loop;\n'
    '    }\n'
    'L_801AA3F4_done:\n'
    '    ctx->r2 = (int32_t)((int32_t)(ctx->r5) + (int32_t)(ctx->r4));\n'
    '    ctx->r1 = 0x801D0000;\n'
    '    MEM_WRITE32(ctx, (int32_t)(ctx->r1) + -2792, ctx->r2);\n'
    '    ctx->r2 = (int32_t)0;\n'
    '    return;\n'
    '}\n'
    '\n'
)
if '// func_801AA3F4: sprite-array clear' not in text:
    if OLD_FUNC_801AA3E8_DEF in text:
        text = text.replace(OLD_FUNC_801AA3E8_DEF,
                            NEW_FUNC_801AA3F4_IMPL + OLD_FUNC_801AA3E8_DEF, 1)
        print("  [8b] func_801AA3F4 implementation added before func_801AA3E8")
    else:
        print("  [8b] WARNING: func_801AA3E8 definition not found")
else:
    print("  [8b] func_801AA3F4 already defined")

# ─── 9. Patch func_80134C14 JUMP_INDIRECT for embedded 0x80134C48 code ──────
# In func_80134C14, JUMP_INDIRECT dispatches to jump-table entries.
# Entry 0x80134C48 is 18 MIPS words embedded as data (not compiled as a fn).
# We intercept the dispatch BEFORE JUMP_INDIRECT runs and execute the 18 words
# inline, then fall through to L_80134C90 (the shared continuation).
#
# Decoded MIPS at 0x80134C48–0x80134C8C:
#   r2 = [0x801CEEFC]; r4 = MEM_READ8(0x801F7EDB); r3 = MEM_READ8(0x801F7EDA)
#   [0x801CEEFC] = 0; [0x801CEF00] = old_r2; r3 <<= 8; r2 = r4 + r3
#   [0x801CEEFC] = r2
#   if r2 == 0: r2 = 0^0xFFFF = 0xFFFF (delay slot), goto L_80134C90
#   else: r2 ^= 0xFFFF (delay slot); [0x801CEEFC] = r2; fallthrough L_80134C90
OLD_JUMP_INDIRECT_C14 = (
    '    JUMP_INDIRECT(ctx, ctx->r2);\n'
    '    // .word 0x3C02801D (data)\n'
    '    // .word 0x8C42EEFC (data)\n'
    '    // .word 0x3C04801F (data)\n'
    '    // .word 0x90847EDB (data)\n'
    '    // .word 0x3C03801F (data)\n'
    '    // .word 0x90637EDA (data)\n'
    '    // .word 0x3C01801D (data)\n'
    '    // .word 0xAC20EEFC (data)\n'
    '    // .word 0x3C01801D (data)\n'
    '    // .word 0xAC22EF00 (data)\n'
    '    // .word 0x00031A00 (data)\n'
    '    // .word 0x00831021 (data)\n'
    '    // .word 0x3C01801D (data)\n'
    '    // .word 0xAC22EEFC (data)\n'
    '    // .word 0x10400003 (data)\n'
    '    // .word 0x3842FFFF (data)\n'
    '    // .word 0x3C01801D (data)\n'
    '    // .word 0xAC22EEFC (data)\n'
    'L_80134C90:\n'
)
NEW_JUMP_INDIRECT_C14 = (
    '    // Intercept jump-table entry 0x80134C48: embedded MIPS not compiled.\n'
    '    // Execute the 18 words inline then fall through to L_80134C90.\n'
    '    if ((uint32_t)(ctx->r2) == 0x80134C48u) {\n'
    '        // 0x80134C48: LUI/LW r2,[0x801CEEFC]\n'
    '        uint32_t _c48_r2 = (uint32_t)MEM_READ32(ctx, 0x801CEEFC);\n'
    '        // 0x80134C50/54: LUI/LBU r4,[0x801F7EDB]\n'
    '        uint32_t _c48_r4 = (uint8_t)MEM_READ8(ctx, 0x801F7EDB);\n'
    '        // 0x80134C58/5C: LUI/LBU r3,[0x801F7EDA]\n'
    '        uint32_t _c48_r3 = (uint8_t)MEM_READ8(ctx, 0x801F7EDA);\n'
    '        // 0x80134C60/64: [0x801CEEFC] = 0\n'
    '        MEM_WRITE32(ctx, 0x801CEEFC, (int32_t)0);\n'
    '        // 0x80134C68/6C: [0x801CEF00] = old r2\n'
    '        MEM_WRITE32(ctx, 0x801CEF00, (int32_t)_c48_r2);\n'
    '        // 0x80134C70: r3 <<= 8\n'
    '        _c48_r3 = (uint32_t)(_c48_r3) << 8;\n'
    '        // 0x80134C74: r2 = r4 + r3\n'
    '        _c48_r2 = _c48_r4 + _c48_r3;\n'
    '        // 0x80134C78/7C: [0x801CEEFC] = r2\n'
    '        MEM_WRITE32(ctx, 0x801CEEFC, (int32_t)_c48_r2);\n'
    '        // 0x80134C80: BEQ r2,r0,L_80134C90; delay slot: r2 ^= 0xFFFF\n'
    '        uint32_t _c48_cond = _c48_r2;\n'
    '        _c48_r2 = _c48_r2 ^ 0xFFFFu; // delay slot always executes\n'
    '        if ((uint32_t)(_c48_cond) != 0u) {\n'
    '            // 0x80134C88/8C: [0x801CEEFC] = r2 (xor\'d)\n'
    '            MEM_WRITE32(ctx, 0x801CEEFC, (int32_t)_c48_r2);\n'
    '        }\n'
    '        ctx->r2 = (int32_t)_c48_r2;\n'
    '        ctx->r3 = (int32_t)_c48_r3;\n'
    '        ctx->r4 = (int32_t)_c48_r4;\n'
    '        goto L_80134C90;\n'
    '    }\n'
    '    JUMP_INDIRECT(ctx, ctx->r2);\n'
    '    // .word 0x3C02801D (data)\n'
    '    // .word 0x8C42EEFC (data)\n'
    '    // .word 0x3C04801F (data)\n'
    '    // .word 0x90847EDB (data)\n'
    '    // .word 0x3C03801F (data)\n'
    '    // .word 0x90637EDA (data)\n'
    '    // .word 0x3C01801D (data)\n'
    '    // .word 0xAC20EEFC (data)\n'
    '    // .word 0x3C01801D (data)\n'
    '    // .word 0xAC22EF00 (data)\n'
    '    // .word 0x00031A00 (data)\n'
    '    // .word 0x00831021 (data)\n'
    '    // .word 0x3C01801D (data)\n'
    '    // .word 0xAC22EEFC (data)\n'
    '    // .word 0x10400003 (data)\n'
    '    // .word 0x3842FFFF (data)\n'
    '    // .word 0x3C01801D (data)\n'
    '    // .word 0xAC22EEFC (data)\n'
    'L_80134C90:\n'
)
if '0x80134C48u' not in text:
    if OLD_JUMP_INDIRECT_C14 in text:
        text = text.replace(OLD_JUMP_INDIRECT_C14, NEW_JUMP_INDIRECT_C14, 1)
        print("  [9] func_80134C14 JUMP_INDIRECT patched for 0x80134C48")
    else:
        print("  [9] WARNING: func_80134C14 JUMP_INDIRECT pattern not found")
else:
    print("  [9] func_80134C14 already patched for 0x80134C48")

# ─── 6. Register new functions in dispatch table ─────────────────────────────
OLD_TABLE_8019F8D0 = '    recomp_func_table[0x8019F8D0] = func_8019F8D0;\n'
# Only add a function to the dispatch table if it was actually created by patches 4/8.
_new_entries = ''
if 'func_8019F848' in text and '0x8019F848' not in text:
    _new_entries += '    recomp_func_table[0x8019F848] = func_8019F848;\n'
_new_entries += '    recomp_func_table[0x8019F8D0] = func_8019F8D0;\n'
if 'func_801AA3F4' in text and '0x801AA3F4' not in text:
    _new_entries += '    recomp_func_table[0x801AA3F4] = func_801AA3F4;\n'
NEW_TABLE_8019F8D0 = _new_entries
if OLD_TABLE_8019F8D0 in text and NEW_TABLE_8019F8D0 != OLD_TABLE_8019F8D0:
    text = text.replace(OLD_TABLE_8019F8D0, NEW_TABLE_8019F8D0, 1)
    print("  [6a] func_8019F848 + func_801AA3F4 registered in dispatch table")
else:
    print("  [6a] 0x8019F848 + 0x801AA3F4 already in dispatch table (or functions missing)")

OLD_TABLE_801BE0B4 = '    recomp_func_table[0x801BE0B4] = func_801BE0B4;\n'
NEW_TABLE_801BE0B4 = (
    '    recomp_func_table[0x801BE0B4] = func_801BE0B4;\n'
    '    recomp_func_table[0x801BE0C0] = func_801BE0C0;\n'
)
if '0x801BE0C0] = func_801BE0C0' not in text:
    if OLD_TABLE_801BE0B4 in text:
        text = text.replace(OLD_TABLE_801BE0B4, NEW_TABLE_801BE0B4, 1)
        print("  [6b] func_801BE0C0 registered in dispatch table")
    else:
        print("  [6b] WARNING: 0x801BE0B4 table entry not found")
else:
    print("  [6b] 0x801BE0C0 already in dispatch table")

# ─── 7. Add drainPendingCallbacks in L_8019FA1C spin loop of func_8019F8D0 ───
# This loop waits for MEM_READ16(0x801CEEBC) != 0.
# 0x801CEEBC is set by func_801300AC (GPU swap callback) called with arg=4.
# The VBlank handler queues this callback but the game thread must call
# drainPendingCallbacks() to execute it.
OLD_LOOP_FA1C = ('L_8019FA1C:\n'
                 '    // NOP // delay slot\n'
                 '    func_80131DB8(rdram, ctx);\n'
                 '    // NOP // delay slot\n'
                 '    if ((int32_t)(ctx->r2) == (int32_t)((int32_t)0)) goto L_8019FA1C;\n')
NEW_LOOP_FA1C = ('L_8019FA1C:\n'
                 '    if (ctx->bios) ctx->bios->drainPendingCallbacks();\n'
                 '    // NOP // delay slot\n'
                 '    func_80131DB8(rdram, ctx);\n'
                 '    // NOP // delay slot\n'
                 '    if ((int32_t)(ctx->r2) == (int32_t)((int32_t)0)) goto L_8019FA1C;\n')
if OLD_LOOP_FA1C in text:
    text = text.replace(OLD_LOOP_FA1C, NEW_LOOP_FA1C, 1)
    print("  [7] drainPendingCallbacks in L_8019FA1C (func_8019F8D0 GPU-sync loop)")
else:
    print("  [7] WARNING: L_8019FA1C pattern not found")

# ─── 11. Skip FMV intro: func_80132898 ──────────────────────────────────────
# INVESTIGATION MODE: patch #11 is now disabled so MDEC diagnostics in
# decodeSlice() can identify exactly what fails during FMV playback.
#
# To re-enable the skip (safe fallback), set SKIP_FMV = True below.
# To investigate MDEC failure, leave SKIP_FMV = False and check logs for:
#   [MDEC] decodeSlice: N words of input
#   [MDEC] macroblock K ok
#   [MDEC] decodeSlice: K macroblocks decoded  ← K=0 means first block fails
#
# Known failure modes:
#   - K=0: rleDecode() fails on first word — likely bad DC word or end-marker
#   - K>0 but < expected: AC RLE misaligned — zigzag/quant mismatch
#   - Hang/no output: infinite loop in rleDecode() — end marker 0xFE00 not hit
SKIP_FMV = True

if SKIP_FMV:
    OLD_FMV_SKIP = (
        'void func_80132898(uint8_t* rdram, recomp_context* ctx) {\n'
        '    ctx->r29 = (int32_t)((int32_t)(ctx->r29) + -24);\n'
        '    MEM_WRITE32(ctx, (int32_t)(ctx->r29) + 20, ctx->r31);\n'
        '    MEM_WRITE32(ctx, (int32_t)(ctx->r29) + 16, ctx->r16);\n'
        '    ctx->r16 = (int32_t)((int32_t)(ctx->r4) + (int32_t)((int32_t)0));\n'
        '    ctx->r4 = (int32_t)((int32_t)(ctx->r4) + -1);\n'
    )
    NEW_FMV_SKIP = (
        'void func_80132898(uint8_t* rdram, recomp_context* ctx) {\n'
        '    // [PATCH 11] Skip FMV intro — MDEC_vlec hits invalid VLC IDs from CD data.\n'
        '    // Return r2=0 immediately so the intro sequence proceeds past the FMV call.\n'
        '    ctx->r2 = 0;\n'
        '    return;\n'
        '    // Original code (unreachable):\n'
        '    ctx->r29 = (int32_t)((int32_t)(ctx->r29) + -24);\n'
        '    MEM_WRITE32(ctx, (int32_t)(ctx->r29) + 20, ctx->r31);\n'
        '    MEM_WRITE32(ctx, (int32_t)(ctx->r29) + 16, ctx->r16);\n'
        '    ctx->r16 = (int32_t)((int32_t)(ctx->r4) + (int32_t)((int32_t)0));\n'
        '    ctx->r4 = (int32_t)((int32_t)(ctx->r4) + -1);\n'
    )
    if '// [PATCH 11] Skip FMV intro' not in text:
        if OLD_FMV_SKIP in text:
            text = text.replace(OLD_FMV_SKIP, NEW_FMV_SKIP, 1)
            print("  [11] FMV skip: func_80132898 patched to return immediately")
        else:
            print("  [11] WARNING: func_80132898 pattern not found — FMV not skipped!")
    else:
        print("  [11] FMV skip already applied")
else:
    print("  [11] FMV skip DISABLED — MDEC investigation mode (set SKIP_FMV=True to re-enable)")

# ─── 12. HLE VSync wait: func_801B954C ───────────────────────────────────────
# The original PsyQ VSync waits by counting down 0x8000 iterations.
# On x86 at 3GHz this loop completes in ~30µs — too fast for the 16.67ms
# VBlank thread tick — causing "VSync: timeout" on every call.
# Replace the body with a proper sleep-based wait.
OLD_VSYNC_WAIT = (
    'void func_801B954C(uint8_t* rdram, recomp_context* ctx) {\n'
    '    ctx->r29 = (int32_t)((int32_t)(ctx->r29) + -32);\n'
    '    ctx->r2 = (int32_t)0 | 0x8000;\n'
    '    MEM_WRITE32(ctx, (int32_t)(ctx->r29) + 16, ctx->r2);\n'
    '    ctx->r2 = 0x801D0000;\n'
    '    ctx->r2 = MEM_READ32(ctx, (int32_t)(ctx->r2) + -3380);\n'
)
NEW_VSYNC_WAIT = (
    'void func_801B954C(uint8_t* rdram, recomp_context* ctx) {\n'
    '    // [HLE 12] VSync wait: block until vblankCounter (0x801CF2CC) >= r4.\n'
    '    // The original PsyQ code loops 0x8000 times which completes in ~30µs on\n'
    '    // x86 — far too fast for the 16.67ms VBlank period — causing "VSync: timeout".\n'
    '    // We sleep 1ms per iteration so the VBlank thread (60Hz) has time to fire.\n'
    '    static std::atomic<uint32_t> vsync_call_count{0};\n'
    '    uint32_t n = ++vsync_call_count;\n'
    '    if (n % 60 == 0) {\n'
    '        fprintf(stderr, "[VSYNC-DBG] VSync call #%u (game loop active)\\n", n);\n'
    '        fflush(stderr);\n'
    '    }\n'
    '    uint32_t target = (uint32_t)(ctx->r4);\n'
    '    for (int i = 0; i < 100; i++) {\n'
    '        uint32_t cnt = (uint32_t)MEM_READ32(ctx, (int32_t)(0x801D0000) + -3380);\n'
    '        if (cnt >= target) break;\n'
    '        if (ctx->bios) ctx->bios->drainPendingCallbacks();\n'
    '        std::this_thread::sleep_for(std::chrono::milliseconds(1));\n'
    '    }\n'
    '    ctx->r2 = (int32_t)(uint32_t)MEM_READ32(ctx, (int32_t)(0x801D0000) + -3380);\n'
    '    return;\n'
    '    // Original code (unreachable):\n'
    '    ctx->r29 = (int32_t)((int32_t)(ctx->r29) + -32);\n'
    '    ctx->r2 = (int32_t)0 | 0x8000;\n'
    '    MEM_WRITE32(ctx, (int32_t)(ctx->r29) + 16, ctx->r2);\n'
    '    ctx->r2 = 0x801D0000;\n'
    '    ctx->r2 = MEM_READ32(ctx, (int32_t)(ctx->r2) + -3380);\n'
)
if '// [HLE 12] VSync wait' not in text:
    if OLD_VSYNC_WAIT in text:
        text = text.replace(OLD_VSYNC_WAIT, NEW_VSYNC_WAIT, 1)
        print("  [12] HLE VSync wait: func_801B954C patched")
    else:
        print("  [12] WARNING: func_801B954C pattern not found — VSync not patched!")
else:
    print("  [12] func_801B954C already HLE'd")

# ─── 13. Add func_801B1AE4 — mid-function entry point in func_801B18C4 ───────
# 0x801B1AE4 is dead code inside func_801B18C4 (unreachable via static flow)
# but IS a valid entry point called via function pointer from the entity dispatch
# table at 0x801D20E8.  The body is identical to the L_801B196C case: it calls
# func_801B1F7C(sign_extend_16(a0), sign_extend_16(a1), a2).
OLD_FWD_801B18C4 = (
    'void func_801B18C4(uint8_t* rdram, recomp_context* ctx);\n'
    'void func_801B1B9C(uint8_t* rdram, recomp_context* ctx);\n'
)
NEW_FWD_801B18C4 = (
    'void func_801B18C4(uint8_t* rdram, recomp_context* ctx);\n'
    'void func_801B1AE4(uint8_t* rdram, recomp_context* ctx);\n'
    'void func_801B1B9C(uint8_t* rdram, recomp_context* ctx);\n'
)
if 'void func_801B1AE4' not in text:
    if OLD_FWD_801B18C4 in text:
        text = text.replace(OLD_FWD_801B18C4, NEW_FWD_801B18C4, 1)
        print("  [13a] Forward decl func_801B1AE4 added")
    else:
        print("  [13a] WARNING: func_801B18C4/func_801B1B9C forward decl context not found")
else:
    print("  [13a] func_801B1AE4 already declared")

# Insert implementation before func_801B1B9C definition.
# The body mirrors the other switch-table cases in func_801B18C4 (e.g. L_801B196C):
#   r4 = sign_extend_16(r17 = a0), r5 = sign_extend_16(r19 = a1), r6 = r18 = a2
# Stack frame matches func_801B18C4 (sp-=40, saves r31/r17/r18/r19).
OLD_FUNC_801B1B9C_DEF = 'void func_801B1B9C(uint8_t* rdram, recomp_context* ctx) {\n'
NEW_FUNC_801B1AE4_IMPL = (
    '// func_801B1AE4: standalone wrapper for dead-code entry point inside func_801B18C4.\n'
    '// Called via function pointer from entity dispatch table at 0x801D20E8.\n'
    '// Equivalent to the L_801B196C case: calls func_801B1F7C(sign_extend(a0), sign_extend(a1), a2).\n'
    'void func_801B1AE4(uint8_t* rdram, recomp_context* ctx) {\n'
    '    ctx->r29 = (int32_t)((int32_t)(ctx->r29) + -40);\n'
    '    MEM_WRITE32(ctx, (int32_t)(ctx->r29) + 36, ctx->r31);\n'
    '    MEM_WRITE32(ctx, (int32_t)(ctx->r29) + 28, ctx->r19);\n'
    '    MEM_WRITE32(ctx, (int32_t)(ctx->r29) + 24, ctx->r18);\n'
    '    MEM_WRITE32(ctx, (int32_t)(ctx->r29) + 20, ctx->r17);\n'
    '    ctx->r17 = ctx->r4; // a0\n'
    '    ctx->r19 = ctx->r5; // a1\n'
    '    ctx->r18 = ctx->r6; // a2\n'
    '    ctx->r4 = (int32_t)((uint32_t)(ctx->r17) << 16);\n'
    '    ctx->r4 = (int32_t)(ctx->r4) >> 16;\n'
    '    ctx->r5 = (int32_t)((uint32_t)(ctx->r19) << 16);\n'
    '    ctx->r5 = (int32_t)(ctx->r5) >> 16;\n'
    '    ctx->r6 = (int32_t)((int32_t)(ctx->r18) + (int32_t)((int32_t)0));\n'
    '    func_801B1F7C(rdram, ctx);\n'
    '    ctx->r31 = MEM_READ32(ctx, (int32_t)(ctx->r29) + 36);\n'
    '    ctx->r19 = MEM_READ32(ctx, (int32_t)(ctx->r29) + 28);\n'
    '    ctx->r18 = MEM_READ32(ctx, (int32_t)(ctx->r29) + 24);\n'
    '    ctx->r17 = MEM_READ32(ctx, (int32_t)(ctx->r29) + 20);\n'
    '    ctx->r29 = (int32_t)((int32_t)(ctx->r29) + 40);\n'
    '}\n'
    '\n'
)
if '// func_801B1AE4: standalone wrapper' not in text:
    if OLD_FUNC_801B1B9C_DEF in text:
        text = text.replace(OLD_FUNC_801B1B9C_DEF,
                            NEW_FUNC_801B1AE4_IMPL + OLD_FUNC_801B1B9C_DEF, 1)
        print("  [13b] func_801B1AE4 implementation added before func_801B1B9C")
    else:
        print("  [13b] WARNING: func_801B1B9C definition not found")
else:
    print("  [13b] func_801B1AE4 already defined")

# Register func_801B1AE4 in dispatch table after func_801B18C4.
OLD_TABLE_801B18C4 = '    recomp_func_table[0x801B18C4] = func_801B18C4;\n'
NEW_TABLE_801B18C4 = (
    '    recomp_func_table[0x801B18C4] = func_801B18C4;\n'
    '    recomp_func_table[0x801B1AE4] = func_801B1AE4;\n'
)
if '0x801B1AE4] = func_801B1AE4' not in text:
    if OLD_TABLE_801B18C4 in text:
        text = text.replace(OLD_TABLE_801B18C4, NEW_TABLE_801B18C4, 1)
        print("  [13c] func_801B1AE4 registered in dispatch table")
    else:
        print("  [13c] WARNING: 0x801B18C4 table entry not found")
else:
    print("  [13c] 0x801B1AE4 already in dispatch table")

# ─── 14. Add func_8019F848 — animation counter increment ─────────────────────
# 0x8019F848 is the start of a dead-code block inside func_8019F3A0.
# Decoded from .word data at 0x8019F848–0x8019F880:
#   LUI/LBU r2 ← MEM_READ8(0x801CF438);  ADDIU sp,-24;  SW r31,16(sp)
#   r2++;  SB r2,(0x801CF438);  JAL 801809FC;  NOP
#   LUI/LBU r2 ← reload;  NOP;  SLTIU r2,r2,61;  LW r31,16(sp)
#   XORI r2,r2,1;  JR r31;  ADDIU sp,24 [delay slot]
# Returns 1 if counter reached 61 (animation sequence done), 0 otherwise.
OLD_FWD_8019F8D0 = 'void func_8019F8D0(uint8_t* rdram, recomp_context* ctx);\n'
NEW_FWD_8019F8D0 = (
    'void func_8019F848(uint8_t* rdram, recomp_context* ctx);\n'
    'void func_8019F8D0(uint8_t* rdram, recomp_context* ctx);\n'
)
if 'void func_8019F848' not in text:
    if OLD_FWD_8019F8D0 in text:
        text = text.replace(OLD_FWD_8019F8D0, NEW_FWD_8019F8D0, 1)
        print("  [14a] Forward decl func_8019F848 added")
    else:
        print("  [14a] WARNING: func_8019F8D0 forward decl not found")
else:
    print("  [14a] func_8019F848 already declared")

OLD_DEF_8019F8D0 = 'void func_8019F8D0(uint8_t* rdram, recomp_context* ctx) {\n'
NEW_IMPL_8019F848 = (
    '// func_8019F848: animation counter increment + frame tick.\n'
    '// Dead-code entry point (0x8019F848) inside func_8019F3A0\'s data block.\n'
    '// Increments counter at 0x801CF438, calls func_801809FC (frame tick),\n'
    '// then returns 1 when counter reaches/exceeds 61 (animation complete).\n'
    'void func_8019F848(uint8_t* rdram, recomp_context* ctx) {\n'
    '    ctx->r2 = (uint8_t)MEM_READ8(ctx, 0x801D0000 + -3016);\n'
    '    ctx->r29 = (int32_t)((int32_t)(ctx->r29) + -24);\n'
    '    MEM_WRITE32(ctx, (int32_t)(ctx->r29) + 16, ctx->r31);\n'
    '    ctx->r2 = (int32_t)((int32_t)(ctx->r2) + 1);\n'
    '    MEM_WRITE8(ctx, 0x801D0000 + -3016, ctx->r2);\n'
    '    func_801809FC(rdram, ctx);\n'
    '    ctx->r2 = (uint8_t)MEM_READ8(ctx, 0x801D0000 + -3016);\n'
    '    ctx->r2 = ((uint32_t)(ctx->r2) < (uint32_t)(0x3D)) ? 1 : 0;\n'
    '    ctx->r31 = MEM_READ32(ctx, (int32_t)(ctx->r29) + 16);\n'
    '    ctx->r2 = ctx->r2 ^ 0x1;\n'
    '    ctx->r29 = (int32_t)((int32_t)(ctx->r29) + 24);\n'
    '}\n'
    '\n'
)
if '// func_8019F848: animation counter' not in text:
    if OLD_DEF_8019F8D0 in text:
        text = text.replace(OLD_DEF_8019F8D0, NEW_IMPL_8019F848 + OLD_DEF_8019F8D0, 1)
        print("  [14b] func_8019F848 implementation added")
    else:
        print("  [14b] WARNING: func_8019F8D0 definition not found")
else:
    print("  [14b] func_8019F848 already defined")

# Register func_8019F848 in dispatch table
OLD_TABLE_8019F8D0_NEW = '    recomp_func_table[0x8019F8D0] = func_8019F8D0;\n'
NEW_TABLE_8019F848 = (
    '    recomp_func_table[0x8019F848] = func_8019F848;\n'
    '    recomp_func_table[0x8019F8D0] = func_8019F8D0;\n'
)
if '0x8019F848] = func_8019F848' not in text:
    if OLD_TABLE_8019F8D0_NEW in text:
        text = text.replace(OLD_TABLE_8019F8D0_NEW, NEW_TABLE_8019F848, 1)
        print("  [14c] func_8019F848 registered in dispatch table")
    else:
        print("  [14c] WARNING: 0x8019F8D0 table entry not found")
else:
    print("  [14c] func_8019F848 already in dispatch table")

# ─── 15. Add func_801BEAD0 + func_801BEB08 — GPU indexed buffer ops ──────────
# Decoded from .word data block at 0x801BEAD0–0x801BEB21 inside func_801BE804.
#
# func_801BEAD0 (14 words, 0x801BEAD0–0x801BEB07):
#   ptr = MEM_READ32(0x801CF31C);  SW r4, 0(ptr);
#   r2 = (r4>>24)<<4;  r4 &= 0x00FFFFFF;
#   MEM_WRITE32(0x801D2060 + r2, r4);  return
#
# func_801BEB08 (8 words, 0x801BEB08–0x801BEB23):
#   r2 = r4<<4;  r2 = MEM_READ32(0x801D2060 + r2);
#   r4 = r4<<24;  return r2 | r4 [delay slot]
OLD_FWD_801BEB74 = 'void func_801BEB74(uint8_t* rdram, recomp_context* ctx);\n'
NEW_FWD_801BEB74 = (
    'void func_801BEAD0(uint8_t* rdram, recomp_context* ctx);\n'
    'void func_801BEB08(uint8_t* rdram, recomp_context* ctx);\n'
    'void func_801BEB74(uint8_t* rdram, recomp_context* ctx);\n'
)
if 'void func_801BEAD0' not in text:
    if OLD_FWD_801BEB74 in text:
        text = text.replace(OLD_FWD_801BEB74, NEW_FWD_801BEB74, 1)
        print("  [15a] Forward decls func_801BEAD0 + func_801BEB08 added")
    else:
        print("  [15a] WARNING: func_801BEB74 forward decl not found")
else:
    print("  [15a] func_801BEAD0/BEB08 already declared")

OLD_DEF_801BEB74 = 'void func_801BEB74(uint8_t* rdram, recomp_context* ctx) {\n'
NEW_IMPL_801BEAD0 = (
    '// func_801BEAD0: GPU indexed-buffer write.\n'
    '// Writes GP0 command word r4 to *MEM_READ32(0x801CF31C) and also\n'
    '// stores lower 24 bits to the opcode-indexed table at 0x801D2060.\n'
    'void func_801BEAD0(uint8_t* rdram, recomp_context* ctx) {\n'
    '    uint32_t ptr = (uint32_t)MEM_READ32(ctx, 0x801D0000 + -3300);\n'
    '    if (ptr == 0) return;  // guard: buffer not initialized yet\n'
    '    MEM_WRITE32(ctx, (int32_t)ptr, ctx->r4);\n'
    '    ctx->r2 = (uint32_t)(ctx->r4) >> 24;\n'
    '    ctx->r2 = (int32_t)((uint32_t)(ctx->r2) << 4);\n'
    '    ctx->r4 = ctx->r4 & 0x00FFFFFF;\n'
    '    MEM_WRITE32(ctx, (int32_t)(0x801D0000 + 0x2060) + ctx->r2, ctx->r4);\n'
    '}\n'
    '\n'
    '// func_801BEB08: GPU indexed-buffer read.\n'
    '// Returns (r4<<24) | MEM_READ32(0x801D2060 + (r4<<4)).\n'
    'void func_801BEB08(uint8_t* rdram, recomp_context* ctx) {\n'
    '    ctx->r2 = (int32_t)((uint32_t)(ctx->r4) << 4);\n'
    '    ctx->r2 = MEM_READ32(ctx, (int32_t)(0x801D0000 + 0x2060) + ctx->r2);\n'
    '    ctx->r4 = (int32_t)((uint32_t)(ctx->r4) << 24);\n'
    '    ctx->r2 = ctx->r4 | ctx->r2;\n'
    '}\n'
    '\n'
)
if '// func_801BEAD0: GPU indexed-buffer write' not in text:
    if OLD_DEF_801BEB74 in text:
        text = text.replace(OLD_DEF_801BEB74, NEW_IMPL_801BEAD0 + OLD_DEF_801BEB74, 1)
        print("  [15b] func_801BEAD0 + func_801BEB08 implementations added")
    else:
        print("  [15b] WARNING: func_801BEB74 definition not found")
else:
    print("  [15b] func_801BEAD0/BEB08 already defined")

# NOTE: func_801BEAD0 and func_801BEB08 are intentionally NOT registered in the
# dispatch table. When dispatched, they corrupt memory (ptr at 0x801CF31C is
# uninitialized because func_801B8DFC never runs) causing SIGSEGV. The functions
# are defined above for documentation purposes only.
print("  [15c] func_801BEAD0/BEB08 NOT registered (known to cause SIGSEGV — func_801B8DFC init missing)")

SKIP_P16_PUTDISPENV = False  # patch #16 enabled (safe when patch #17 is disabled)
SKIP_P17_GPU_HAL = True   # Game initializes [0x801CF300]=0x801CE174 itself — our init overwrites it

# ─── 16. HLE PutDispEnv: replace func_801BD69C body ─────────────────────────
# func_801BD69C is PutDispEnv (PsyQ SDK).  It builds GP1(0x05/06/07/08) commands
# and dispatches via a function pointer stored at *(0x801CF300 + 16).  That
# pointer is only written by func_801B8DFC, which is missing from the recompiled
# binary (dead-code block, not decoded as a function).  As a result, the pointer
# chain is broken, no GP1 commands are ever sent, and the GPU display area stays
# locked at (0, 0) — showing the wrong framebuffer — for the entire session.
#
# Fix: replace the function body with a direct call to hle_PutDispEnv(), which
# reads the DISPENV struct from a0 and sends GP1(0x05/06/07/08) immediately.
OLD_PUT_DISP_ENV = (
    'void func_801BD69C(uint8_t* rdram, recomp_context* ctx) {\n'
    '    ctx->r29 = (int32_t)((int32_t)(ctx->r29) + -40);\n'
    '    MEM_WRITE32(ctx, (int32_t)(ctx->r29) + 32, ctx->r31);\n'
    '    MEM_WRITE32(ctx, (int32_t)(ctx->r29) + 28, ctx->r19);\n'
)
NEW_PUT_DISP_ENV = (
    'void func_801BD69C(uint8_t* rdram, recomp_context* ctx) {\n'
    '    // HLE PutDispEnv: bypasses broken PsyQ GPU pointer chain\n'
    '    // (func_801B8DFC initializer missing → 0x801CF300+16 is garbage)\n'
    '    ps1::psyq::hle_PutDispEnv(ctx);\n'
    '    return;\n'
    '    // Original code unreachable — kept for reference\n'
    '    ctx->r29 = (int32_t)((int32_t)(ctx->r29) + -40);\n'
    '    MEM_WRITE32(ctx, (int32_t)(ctx->r29) + 32, ctx->r31);\n'
    '    MEM_WRITE32(ctx, (int32_t)(ctx->r29) + 28, ctx->r19);\n'
)
if SKIP_P16_PUTDISPENV:
    print("  [16] SKIPPED (SKIP_P16_PUTDISPENV=True) — testing without HLE PutDispEnv")
elif OLD_PUT_DISP_ENV in text:
    text = text.replace(OLD_PUT_DISP_ENV, NEW_PUT_DISP_ENV, 1)
    print("  [16] HLE PutDispEnv: func_801BD69C patched")
else:
    print("  [16] WARNING: func_801BD69C pattern not found — PutDispEnv not patched!")

# ─── 17. Synthesize func_801B8DFC: GPU HAL struct initializer ─────────────────
# func_801B8DFC was dead code (inside func_801B8DEC's data block, never decoded).
# It initializes the GPU HAL struct at 0x801D3000 (confirmed free area) with
# low-level GP0/GP1 write stubs, and writes the struct pointer to 0x801CF300.
# Without this, all CALL_INDIRECT through the GPU function pointer chain
# dispatches to address 0 (silently dropped), and GP1 commands are never sent.
#
# We also inject a call to func_801B8DFC at the top of func_801C104C (the
# display pipeline, called every frame). The guard at [0x801CF2D4] ensures
# it only runs once.

# 17a0. Add forward declarations for the new functions
OLD_FWDDECL_801B8DEC = 'void func_801B8DEC(uint8_t* rdram, recomp_context* ctx);\n'
NEW_FWDDECL_GPU_HAL = (
    'void func_801B8DEC(uint8_t* rdram, recomp_context* ctx);\n'
    'void func_801B8DFC(uint8_t* rdram, recomp_context* ctx);\n'
    'void func_801B8E00(uint8_t* rdram, recomp_context* ctx);\n'
    'void func_801B8E08(uint8_t* rdram, recomp_context* ctx);\n'
)
if SKIP_P17_GPU_HAL:
    print("  [17a0] SKIPPED (SKIP_P17_GPU_HAL=True)")
elif OLD_FWDDECL_801B8DEC in text and 'void func_801B8DFC(uint8_t* rdram, recomp_context* ctx);' not in text:
    text = text.replace(OLD_FWDDECL_801B8DEC, NEW_FWDDECL_GPU_HAL, 1)
    print("  [17a0] Forward declarations added for GPU HAL functions")
elif 'void func_801B8DFC(uint8_t* rdram, recomp_context* ctx);' in text:
    print("  [17a0] Forward declarations already present")
else:
    print("  [17a0] WARNING: func_801B8DEC forward decl not found!")

# 17a. Add the three new functions right before func_801B8DEC's definition
OLD_801B8DEC_DEF = 'void func_801B8DEC(uint8_t* rdram, recomp_context* ctx) {\n'

NEW_GPU_HAL_FUNCS = (
    '// ── Patch #17: GPU HAL initializer (func_801B8DFC was dead code) ─────────────\n'
    '// stub_gp1_write (PS1 addr 0x801B8E00): writes r4 directly to GP1 port\n'
    'void func_801B8E00(uint8_t* rdram, recomp_context* ctx) {\n'
    '    MEM_WRITE32(ctx, 0x1F801814, ctx->r[4]);\n'
    '}\n'
    '// stub_gp0_write (PS1 addr 0x801B8E08): writes r4 directly to GP0 port\n'
    'void func_801B8E08(uint8_t* rdram, recomp_context* ctx) {\n'
    '    MEM_WRITE32(ctx, 0x1F801810, ctx->r[4]);\n'
    '}\n'
    '// func_801B8DFC: GPU HAL struct initializer (synthesized — was dead code)\n'
    'void func_801B8DFC(uint8_t* rdram, recomp_context* ctx) {\n'
    '    // Guard: if already initialized, return immediately\n'
    '    uint32_t guard = MEM_READ32(ctx, (int32_t)(0x801D0000) + (-3372)); // [0x801CF2D4]\n'
    '    if (guard != 0) return;\n'
    '    // Zero-fill the 64-byte GPU HAL struct at 0x801D3000 (confirmed free area)\n'
    '    for (int i = 0; i < 64; i += 4)\n'
    '        MEM_WRITE32(ctx, 0x801D3000 + i, 0);\n'
    '    // struct+16 = GP1 write stub (receives ready GP1 word in r4)\n'
    '    MEM_WRITE32(ctx, 0x801D3000 + 16, 0x801B8E00);\n'
    '    // struct+4  = GP0 write stub (receives ready GP0 word in r4)\n'
    '    MEM_WRITE32(ctx, 0x801D3000 + 4,  0x801B8E08);\n'
    '    // Write struct pointer ONLY to [0x801CF300] (-3328).\n'
    '    // DO NOT write to [0x801CF2D0] (-3376): func_801B8DEC\'s compiled body\n'
    '    // (label L_801B8FA4) reads [0x801CF2D0] and writes struct+4 = 1,\n'
    '    // which would corrupt our GP0 stub pointer (0x801B8E08).\n'
    '    // 0x801CF300 is read by PutDispEnv and func_801BCF38 (display pipeline).\n'
    '    MEM_WRITE32(ctx, (int32_t)(0x801D0000) + (-3328), 0x801D3000); // [0x801CF300]\n'
    '    // Set guard\n'
    '    MEM_WRITE32(ctx, (int32_t)(0x801D0000) + (-3372), 1);          // [0x801CF2D4]\n'
    '}\n\n'
    'void func_801B8DEC(uint8_t* rdram, recomp_context* ctx) {\n'
)

if SKIP_P17_GPU_HAL:
    print("  [17a] SKIPPED (SKIP_P17_GPU_HAL=True)")
elif OLD_801B8DEC_DEF in text and 'void func_801B8DFC(uint8_t* rdram, recomp_context* ctx) {' not in text:
    text = text.replace(OLD_801B8DEC_DEF, NEW_GPU_HAL_FUNCS, 1)
    print("  [17a] GPU HAL functions added (func_801B8DFC, func_801B8E00, func_801B8E08)")
elif 'void func_801B8DFC(uint8_t* rdram, recomp_context* ctx) {' in text:
    print("  [17a] func_801B8DFC already exists — skipping")
else:
    print("  [17a] WARNING: func_801B8DEC definition not found — GPU HAL not added!")

# 17b. Register the three new functions in dispatch table
OLD_TABLE_801B8DEC = '    recomp_func_table[0x801B8DEC] = func_801B8DEC;\n'
NEW_TABLE_801B8DEC = (
    '    recomp_func_table[0x801B8DEC] = func_801B8DEC;\n'
    '    recomp_func_table[0x801B8DFC] = func_801B8DFC;\n'
    '    recomp_func_table[0x801B8E00] = func_801B8E00;\n'
    '    recomp_func_table[0x801B8E08] = func_801B8E08;\n'
)
if SKIP_P17_GPU_HAL:
    print("  [17b] SKIPPED (SKIP_P17_GPU_HAL=True)")
elif OLD_TABLE_801B8DEC in text and 'recomp_func_table[0x801B8DFC]' not in text:
    text = text.replace(OLD_TABLE_801B8DEC, NEW_TABLE_801B8DEC, 1)
    print("  [17b] GPU HAL functions registered in dispatch table")
elif 'recomp_func_table[0x801B8DFC]' in text:
    print("  [17b] GPU HAL functions already in dispatch table")
else:
    print("  [17b] WARNING: dispatch table entry for 0x801B8DEC not found!")

# 17c. Inject call to func_801B8DFC at the top of func_801C104C (display pipeline)
# TEST FLAG: set to True to skip init injection and see if original VRAM path works
SKIP_P17C_INIT = True  # skipped because SKIP_P17_GPU_HAL=True

OLD_801C104C = (
    'void func_801C104C(uint8_t* rdram, recomp_context* ctx) {\n'
    '    ctx->r2 = 0x801F0000;\n'
    '    ctx->r2 = MEM_READ32(ctx, (int32_t)(ctx->r2) + 19992);\n'
)
NEW_801C104C = (
    'void func_801C104C(uint8_t* rdram, recomp_context* ctx) {\n'
    '    // Patch #17c: ensure GPU HAL struct is initialized (guard inside prevents re-init)\n'
    '    func_801B8DFC(rdram, ctx);\n'
    '    ctx->r2 = 0x801F0000;\n'
    '    ctx->r2 = MEM_READ32(ctx, (int32_t)(ctx->r2) + 19992);\n'
)
if SKIP_P17_GPU_HAL or SKIP_P17C_INIT:
    print("  [17c] SKIPPED")
elif OLD_801C104C in text and 'func_801B8DFC(rdram, ctx)' not in text:
    text = text.replace(OLD_801C104C, NEW_801C104C, 1)
    print("  [17c] func_801B8DFC call injected into func_801C104C")
elif 'func_801B8DFC(rdram, ctx)' in text:
    print("  [17c] func_801B8DFC call already in func_801C104C")
else:
    print("  [17c] WARNING: func_801C104C pattern not found — init call not injected!")

# ─── Write output ────────────────────────────────────────────────────────────
print(f"\nWriting {DST}...")
with open(DST, "w") as f:
    f.write(text)

lines = text.count('\n')
print(f"Done! Output: {lines} lines")
