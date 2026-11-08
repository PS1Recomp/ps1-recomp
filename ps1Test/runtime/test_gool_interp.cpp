// Tests: GOOL bytecode interpreter (ps1Runtime/src/gool/gool_interp.cpp) and
// the EMU_* shim primitives it is built on (emu_compat.cpp).
//
// Each test assembles a few GOOL instructions in guest RAM, runs them through
// the public entry point `hle_gool_InterpretObject`, and asserts the object's
// stack / register file / return code. Oracles, in order of authority:
//   - the native interpreter in SCUS-94900 (func_800201DC, as recompiled),
//   - the c1c hand-decompilation (c1c/src/gool.cc),
//   - CrashEdit's GOOL disassembler ("GOOL Instructions/*.cs") and gooc's
//     expression table (gooc/thecl/expr.c). CrashEdit formats list the LSB
//     field first, so its "L" operand is field B (bits 11-0) and "R" is A.
//
// Callouts to native code (rand, sin, ChangeObjectState, CreateObject, event
// senders, NS paging) are observed through the recomp_dispatch test hook.

#include "runtime/cpu_context.h"
#include "runtime/emuptr.h"
#include "runtime/gool/emu_compat.h"
#include "runtime/gool/gool_types.h"
#include "runtime/memory.h"
#include "runtime/psyq/psyq_hle.h"

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <map>
#include <string>
#include <vector>

extern void (*g_testRecompDispatchHook)(recomp_context *, uint32_t);

namespace {

using ps1::gool::goolobj;

// --- Guest memory map for the tests (all in main RAM) ---------------------
constexpr uint32_t kObj = 0x80110000;   // the interpreted object
constexpr uint32_t kOther = 0x80111000; // a second object (links, crash)
constexpr uint32_t kCode = 0x80120000;  // local->items[1]: executable code
constexpr uint32_t kData = 0x80121000;  // local->items[2]: ireg data
constexpr uint32_t kStates = 0x80122000; // local->items[4]: goolstateinfo[]
constexpr uint32_t kAnims = 0x80123000; // local->items[5]: animations
constexpr uint32_t kPool = 0x80124000;  // external->items[2]: pool data
constexpr uint32_t kLocal = 0x80125000; // nsentry
constexpr uint32_t kExternal = 0x80125100;
constexpr uint32_t kConstBuf = 0x80126000;
constexpr uint32_t kDispEnv = 0x80127000;
constexpr uint32_t kTrans = 0x80128000; // goolstateref
constexpr uint32_t kSp = 0x801F0000;    // MIPS stack
constexpr uint32_t kRaSentinel = 0x80001234;

// The GOOL stack lives inside the object (memory[]), as in the game: frame
// ranges are 16-bit offsets from `self`, so it has to be within 64 KB of it.
constexpr uint32_t kSelf = kObj + offsetof(goolobj, self);
constexpr uint32_t kFp = kObj + offsetof(goolobj, memory);
constexpr uint32_t kStack = kFp + 0x20;

// Game BSS globals the VM reads (Crash SCUS-94900).
constexpr uint32_t kGConstBuf = 0x80056480;
constexpr uint32_t kGActiveBuf = 0x80056484;
constexpr uint32_t kGCrash = 0x800566B4;
constexpr uint32_t kGRenderCount = 0x80057960;
constexpr uint32_t kGDispEnv = 0x80058404;
constexpr uint32_t kGTicks = 0x80060E04;
constexpr uint32_t kGGlobals = 0x8006188C;

// Native callouts.
constexpr uint32_t kFnRand = 0x8002F6B0;
constexpr uint32_t kFnTestControls = 0x8001FDC4;
constexpr uint32_t kFnSin = 0x8003905C;
constexpr uint32_t kFnChangeState = 0x8001D698;
constexpr uint32_t kFnCreateObject = 0x8001C6C8;
constexpr uint32_t kFnEvBroadcast = 0x80025134;
constexpr uint32_t kFnEvDirect = 0x80024040;
constexpr uint32_t kFnEvCascade = 0x800251B8;
constexpr uint32_t kFnNsLookup = 0x8001FC4C;
constexpr uint32_t kFnPageA = 0x80015118;
constexpr uint32_t kFnPageB = 0x80015458;
constexpr uint32_t kFnPageC = 0x800156D4;
constexpr uint32_t kFnPageD = 0x8001579C;
constexpr uint32_t kFnSolidRange = 0x80029C90;
constexpr uint32_t kFnObjectFlip = 0x8001D33C;

// Game error codes.
constexpr uint32_t kSuccess = 0xFFFFFF01;
constexpr uint32_t kAllocFailed = 0xFFFFFF02;
constexpr uint32_t kInvalidStateReturn = 0xFFFFFFE6;
constexpr uint32_t kInvalidReturn = 0xFFFFFFE7;
constexpr uint32_t kInvalid = 0xFFFFFFF2;

// --- Operand encodings (c1c GOOL_TranslateGop) -----------------------------
constexpr uint32_t kTop = 0xE1F;     // pop as input, push as output
constexpr uint32_t kNull = 0xBE0;    // null ref
uint32_t reg(uint32_t i) { return 0xE00u | i; }
// Integer literal: 9-bit signed, scaled by 256 (GOOL fixed point).
uint32_t lit(int32_t v) { return 0x800u | (static_cast<uint32_t>(v) & 0x1FF); }
// Fractional literal: 8-bit signed, scaled by 16.
uint32_t frac(int32_t v) {
  return 0xA00u | (static_cast<uint32_t>(v) & 0xFF);
}
// Frame-pointer-relative variable: 6-bit signed word index.
uint32_t var(int32_t i) { return 0xB00u | (static_cast<uint32_t>(i) & 0x3F); }
uint32_t ireg(uint32_t i) { return i & 0x3FF; }         // local data
uint32_t pool(uint32_t i) { return 0x400u | (i & 0x3FF); } // external data
uint32_t link(uint32_t li, uint32_t r) { return 0xC00u | (li << 6) | r; }

uint32_t ins(uint32_t op, uint32_t a = 0, uint32_t b = 0) {
  return (op << 24) | ((a & 0xFFF) << 12) | (b & 0xFFF);
}
// Branch / state / return family (0x82, 0x88, 0x89).
uint32_t flow(uint32_t op, uint32_t optype, uint32_t condtype, uint32_t creg,
              uint32_t low14) {
  return (op << 24) | (optype << 22) | (condtype << 20) | (creg << 14) |
         (low14 & 0x3FFF);
}
uint32_t branch(int32_t off, uint32_t varskip = 0, uint32_t condtype = 0,
                uint32_t creg = 0) {
  return flow(0x82, 0, condtype, creg,
              (varskip << 10) | (static_cast<uint32_t>(off) & 0x3FF));
}
uint32_t ret() { return flow(0x82, 2, 0, 0, 0); }
uint32_t jal(uint32_t addr, uint32_t argc) {
  return (0x86u << 24) | (argc << 20) | addr;
}
// With flags bit 3 clear, 0x88 returns INVALID at once: a side-effect-free halt.
const uint32_t kHalt = ins(0x88);

// --- Callout recorder ------------------------------------------------------
struct Call {
  uint32_t addr, a0, a1, a2, a3, s4, s5;
};
std::vector<Call> g_calls;
std::map<uint32_t, std::vector<uint32_t>> g_returns; // consumed in order
void (*g_sideEffect)(recomp_context *, uint32_t) = nullptr;

void recordCall(recomp_context *c, uint32_t addr) {
  // Stack args 4/5 sit at SP+16/SP+20 of the reserved o32 area.
  g_calls.push_back({addr, c->r[4], c->r[5], c->r[6], c->r[7],
                     c->mem->read32(c->r[29] + 16),
                     c->mem->read32(c->r[29] + 20)});
  uint32_t v0 = 0;
  auto it = g_returns.find(addr);
  if (it != g_returns.end() && !it->second.empty()) {
    v0 = it->second.front();
    it->second.erase(it->second.begin());
  }
  c->r[2] = v0;
  if (g_sideEffect)
    g_sideEffect(c, addr);
}

class GoolInterpTest : public ::testing::Test {
protected:
  ps1::Memory mem;
  recomp_context ctx;

  void SetUp() override {
    mem.reset();
    ctx.reset();
    ctx.mem = &mem;
    ps1::emuptr_set_ram(mem.ramPtr());
    ctx.r[29] = kSp;
    ctx.r[31] = kRaSentinel;
    g_calls.clear();
    g_returns.clear();
    g_sideEffect = nullptr;
    g_testRecompDispatchHook = recordCall;

    // nsentry: magic,id,type,itemcount then items[] at +16.
    mem.write32(kLocal + 16 + 1 * 4, kCode);
    mem.write32(kLocal + 16 + 2 * 4, kData);
    mem.write32(kLocal + 16 + 4 * 4, kStates);
    mem.write32(kLocal + 16 + 5 * 4, kAnims);
    mem.write32(kExternal + 16 + 2 * 4, kPool);
    mem.write32(kGConstBuf, kConstBuf);
    mem.write32(kGDispEnv, kDispEnv);

    field(offsetof(goolobj, local), kLocal);
    field(offsetof(goolobj, external), kExternal);
    field(offsetof(goolobj, self), kObj);
    field(offsetof(goolobj, pc), kCode);
    field(offsetof(goolobj, sp), kStack);
    field(offsetof(goolobj, fp), kFp);
  }

  void TearDown() override {
    g_testRecompDispatchHook = nullptr;
    g_sideEffect = nullptr;
    ps1::emuptr_set_ram(nullptr);
  }

  void field(size_t off, uint32_t v) { mem.write32(kObj + off, v); }
  uint32_t field(size_t off) { return mem.read32(kObj + off); }
  void setReg(uint32_t i, uint32_t v) { mem.write32(kSelf + i * 4, v); }
  uint32_t getReg(uint32_t i) { return mem.read32(kSelf + i * 4); }
  uint32_t sp() { return field(offsetof(goolobj, sp)); }
  uint32_t top() { return mem.read32(sp() - 4); }
  uint32_t depth() { return (sp() - kStack) / 4; }
  void pushWord(uint32_t v) {
    mem.write32(sp(), v);
    field(offsetof(goolobj, sp), sp() + 4);
  }

  // Hand-built frame at fp: [flags][return pc][caller fp|sp], returning to
  // fp = kFp, sp = kStack.
  void callerFrame(uint32_t retPc) {
    mem.write32(kFp + 0, 0);
    mem.write32(kFp + 4, retPc);
    mem.write32(kFp + 8, ((kFp - kSelf) << 16) | (kStack - kSelf));
  }

  // Program at code index `at` (pc starts at index 0).
  void program(std::initializer_list<uint32_t> words, uint32_t at = 0) {
    uint32_t a = kCode + at * 4;
    for (uint32_t w : words) {
      mem.write32(a, w);
      a += 4;
    }
  }

  uint32_t run(uint32_t flags = 0, uint32_t transition = kTrans) {
    ctx.r[4] = kObj;
    ctx.r[5] = flags;
    ctx.r[6] = transition;
    ps1::psyq::hle_gool_InterpretObject(&ctx);
    return ctx.r[2];
  }

  // Runs `op A B` once and returns the pushed value.
  uint32_t binop(uint32_t op, uint32_t a, uint32_t b) {
    field(offsetof(goolobj, pc), kCode);
    field(offsetof(goolobj, sp), kStack);
    setReg(8, a);
    setReg(9, b);
    program({ins(op, reg(8), reg(9)), kHalt});
    EXPECT_EQ(run(), kInvalid);
    EXPECT_EQ(depth(), 1u);
    return top();
  }
};

// --- Halt / frame -----------------------------------------------------------

// c1c GOOL_OP_ReturnStateTransition: event-status ops are only legal while
// handling an event (flags bit 3); otherwise the interpretation is aborted.
TEST_F(GoolInterpTest, EventStatusOpOutsideEventHandlerReturnsInvalid) {
  program({kHalt});
  EXPECT_EQ(run(), kInvalid);
  EXPECT_EQ(depth(), 0u);
  EXPECT_EQ(field(offsetof(goolobj, pc)), kCode + 4);
  EXPECT_EQ(ctx.r[29], kSp) << "native frame unwound";
  EXPECT_EQ(ctx.r[31], kRaSentinel);
}

// --- Arithmetic / logic (field B is the left operand) ----------------------

TEST_F(GoolInterpTest, AddPushesBPlusA) { EXPECT_EQ(binop(0x00, 7, 5), 12u); }
TEST_F(GoolInterpTest, SubPushesBMinusA) {
  EXPECT_EQ(binop(0x01, 7, 5), static_cast<uint32_t>(-2));
}
TEST_F(GoolInterpTest, MulPushesProduct) {
  EXPECT_EQ(binop(0x02, 6, 7), 42u);
}
TEST_F(GoolInterpTest, DivPushesBOverA) { EXPECT_EQ(binop(0x03, 4, 21), 5u); }
// The native handler uses DIVU; psx-spx "CPU Specifications": divu by zero
// leaves LO = FFFFFFFFh and HI = Rs, with no exception.
TEST_F(GoolInterpTest, DivByZeroYieldsAllOnesLikeDivu) {
  EXPECT_EQ(binop(0x03, 0, 21), 0xFFFFFFFFu);
}
TEST_F(GoolInterpTest, ModPushesBModA) { EXPECT_EQ(binop(0x0D, 4, 22), 2u); }
TEST_F(GoolInterpTest, ModByZeroYieldsDividendLikeDivuHi) {
  EXPECT_EQ(binop(0x0D, 0, 22), 22u);
}
TEST_F(GoolInterpTest, CeqIsOneOnlyForEqualOperands) {
  EXPECT_EQ(binop(0x04, 9, 9), 1u);
}
TEST_F(GoolInterpTest, CeqIsZeroForDifferentOperands) {
  EXPECT_EQ(binop(0x04, 9, 8), 0u);
}
TEST_F(GoolInterpTest, LogicalAndNeedsBothNonZero) {
  EXPECT_EQ(binop(0x05, 0x100, 0x2), 1u);
  EXPECT_EQ(binop(0x05, 0x100, 0x0), 0u);
  EXPECT_EQ(binop(0x05, 0x0, 0x2), 0u);
}
TEST_F(GoolInterpTest, LogicalOrNeedsEitherNonZero) {
  EXPECT_EQ(binop(0x06, 0, 0x40), 1u);
  EXPECT_EQ(binop(0x06, 0, 0), 0u);
}
TEST_F(GoolInterpTest, BitwiseAndOrXor) {
  EXPECT_EQ(binop(0x07, 0xF0F0, 0xFF00), 0xF000u);
  EXPECT_EQ(binop(0x08, 0xF0F0, 0xFF00), 0xFFF0u);
  EXPECT_EQ(binop(0x0E, 0xF0F0, 0xFF00), 0x0FF0u);
}
// TST (CrashEdit Tst.cs): true when every bit of A is also set in B.
TEST_F(GoolInterpTest, TstIsTrueWhenAllBitsOfAAreInB) {
  EXPECT_EQ(binop(0x0F, 0x6, 0xE), 1u);
  EXPECT_EQ(binop(0x0F, 0x6, 0xC), 0u);
}
// SHA: positive count shifts left, negative count is an arithmetic right shift.
TEST_F(GoolInterpTest, ShaShiftsLeftForPositiveCount) {
  EXPECT_EQ(binop(0x15, 4, 3), 48u);
}
TEST_F(GoolInterpTest, ShaShiftsRightArithmeticallyForNegativeCount) {
  EXPECT_EQ(binop(0x15, static_cast<uint32_t>(-2), static_cast<uint32_t>(-16)),
            static_cast<uint32_t>(-4));
}

// Operands are consumed A first, then B: with both as stack pops, A takes the
// top and B the word below it (c1c G_TRANS_GOPS order).
TEST_F(GoolInterpTest, StackOperandsPopAFirstThenB) {
  pushWord(10); // becomes B
  pushWord(3);  // top: becomes A
  program({ins(0x01, kTop, kTop), kHalt});
  run();
  EXPECT_EQ(depth(), 1u);
  EXPECT_EQ(top(), 7u) << "B - A = 10 - 3";
}

// --- Moves and unary ops ---------------------------------------------------

TEST_F(GoolInterpTest, MovStoresAIntoB) {
  setReg(8, 0x1234);
  program({ins(0x11, reg(8), reg(9)), kHalt});
  run();
  EXPECT_EQ(getReg(9), 0x1234u);
  EXPECT_EQ(depth(), 0u);
}
TEST_F(GoolInterpTest, MovToStackTopPushes) {
  setReg(8, 0x55);
  program({ins(0x11, reg(8), kTop), kHalt});
  run();
  EXPECT_EQ(depth(), 1u);
  EXPECT_EQ(top(), 0x55u);
}
// A null destination makes the move a no-op rather than a write to address 0.
TEST_F(GoolInterpTest, MovToNullRefIsDropped) {
  mem.write32(0x80000000, 0xA5A5A5A5);
  program({ins(0x11, lit(1), kNull), kHalt});
  EXPECT_EQ(run(), kInvalid);
  EXPECT_EQ(mem.read32(0x80000000), 0xA5A5A5A5u);
  EXPECT_EQ(depth(), 0u);
}
TEST_F(GoolInterpTest, NotlStoresLogicalNegation) {
  setReg(8, 0x700);
  program({ins(0x12, reg(8), reg(9)), ins(0x12, reg(10), reg(11)), kHalt});
  run();
  EXPECT_EQ(getReg(9), 0u);
  EXPECT_EQ(getReg(11), 1u) << "reg 10 is zero";
}
TEST_F(GoolInterpTest, NotbStoresBitwiseComplement) {
  setReg(8, 0x0000FFFF);
  program({ins(0x17, reg(8), reg(9)), kHalt});
  run();
  EXPECT_EQ(getReg(9), 0xFFFF0000u);
}
TEST_F(GoolInterpTest, AbsStoresMagnitude) {
  setReg(8, static_cast<uint32_t>(-300));
  setReg(10, 300);
  program({ins(0x19, reg(8), reg(9)), ins(0x19, reg(10), reg(11)), kHalt});
  run();
  EXPECT_EQ(getReg(9), 300u);
  EXPECT_EQ(getReg(11), 300u);
}
// LEA: stores the guest address of operand A, not its value.
TEST_F(GoolInterpTest, LeaStoresAddressOfOperand) {
  program({ins(0x14, reg(8), reg(9)), kHalt});
  run();
  EXPECT_EQ(getReg(9), kSelf + 8 * 4);
}
TEST_F(GoolInterpTest, PushPairPushesBThenA) {
  setReg(8, 0xAA);
  setReg(9, 0xBB);
  program({ins(0x16, reg(8), reg(9)), kHalt});
  run();
  ASSERT_EQ(depth(), 2u);
  EXPECT_EQ(mem.read32(kStack), 0xBBu);
  EXPECT_EQ(mem.read32(kStack + 4), 0xAAu);
}
TEST_F(GoolInterpTest, PushPairWithNullAPushesOnlyB) {
  setReg(9, 0xBB);
  program({ins(0x16, kNull, reg(9)), kHalt});
  run();
  ASSERT_EQ(depth(), 1u);
  EXPECT_EQ(top(), 0xBBu);
}
TEST_F(GoolInterpTest, PushPairWithNullBPushesNothing) {
  setReg(8, 0xAA);
  program({ins(0x16, reg(8), kNull), ins(0x26, reg(8), kNull), kHalt});
  run();
  EXPECT_EQ(depth(), 0u);
}
TEST_F(GoolInterpTest, PushAddressPairPushesAddresses) {
  program({ins(0x26, reg(8), reg(9)), kHalt});
  run();
  ASSERT_EQ(depth(), 2u);
  EXPECT_EQ(mem.read32(kStack), kSelf + 9 * 4);
  EXPECT_EQ(mem.read32(kStack + 4), kSelf + 8 * 4);
}
// 0x18 ("settrans" / code pointer): execcode + offset*4 into a register, or
// pushed when the register field is 0x1F.
TEST_F(GoolInterpTest, CodePointerStoresIntoRegisterOrPushes) {
  program({(0x18u << 24) | (12u << 14) | 0x20, (0x18u << 24) | (0x1Fu << 14) | 5,
           kHalt});
  run();
  EXPECT_EQ(getReg(12), kCode + 0x20 * 4);
  ASSERT_EQ(depth(), 1u);
  EXPECT_EQ(top(), kCode + 5 * 4);
}
TEST_F(GoolInterpTest, GetAnimStoresAnimationAddress) {
  setReg(8, 64 * 3);
  program({ins(0x27, reg(8), reg(9)), kHalt});
  run();
  EXPECT_EQ(getReg(9), kAnims + 3) << "byte offset a/64";
}

// --- Operand addressing modes ----------------------------------------------

TEST_F(GoolInterpTest, IntLiteralIsSignExtendedAndScaledBy256) {
  program({ins(0x11, lit(3), reg(8)), ins(0x11, lit(-1), reg(9)), kHalt});
  run();
  EXPECT_EQ(getReg(8), 0x300u);
  EXPECT_EQ(getReg(9), 0xFFFFFF00u);
}
TEST_F(GoolInterpTest, FracLiteralIsSignExtendedAndScaledBy16) {
  program({ins(0x11, frac(0x10), reg(8)), ins(0x11, frac(-128), reg(9)),
           kHalt});
  run();
  EXPECT_EQ(getReg(8), 0x100u);
  EXPECT_EQ(getReg(9), static_cast<uint32_t>(-2048));
}
// Two literals in one instruction must land in different const-buffer slots,
// or the second translation would overwrite the first before it is read.
TEST_F(GoolInterpTest, TwoLiteralsInOneInstructionDoNotAlias) {
  program({ins(0x01, lit(1), lit(4)), kHalt});
  run();
  EXPECT_EQ(top(), 0x300u) << "4*256 - 1*256";
}
TEST_F(GoolInterpTest, FrameVarIndexesFromFpWithSign) {
  mem.write32(kFp + 4, 0x11);
  mem.write32(kFp - 4, 0x22);
  program({ins(0x11, var(1), reg(8)), ins(0x11, var(-1), reg(9)), kHalt});
  run();
  EXPECT_EQ(getReg(8), 0x11u);
  EXPECT_EQ(getReg(9), 0x22u);
}
TEST_F(GoolInterpTest, IregReadsLocalDataAndPoolReadsExternalData) {
  mem.write32(kData + 3 * 4, 0x333);
  mem.write32(kPool + 5 * 4, 0x555);
  program({ins(0x11, ireg(3), reg(8)), ins(0x11, pool(5), reg(9)), kHalt});
  run();
  EXPECT_EQ(getReg(8), 0x333u);
  EXPECT_EQ(getReg(9), 0x555u);
}
TEST_F(GoolInterpTest, LinkRefAddressesAnotherObjectsRegister) {
  setReg(1, kOther); // parent
  mem.write32(kOther + offsetof(goolobj, self) + 10 * 4, 0xBEEF);
  program({ins(0x11, link(1, 10), reg(8)), ins(0x11, lit(2), link(1, 11)),
           kHalt});
  run();
  EXPECT_EQ(getReg(8), 0xBEEFu);
  EXPECT_EQ(mem.read32(kOther + offsetof(goolobj, self) + 11 * 4), 0x200u);
}
TEST_F(GoolInterpTest, LinkRefThroughNullObjectIsNull) {
  setReg(2, 0); // sibling: none
  // Where `self + 11` of a null object would land if the null were followed.
  const uint32_t stray = 0x80000000 + offsetof(goolobj, self) + 11 * 4;
  mem.write32(stray, 0xA5A5A5A5);
  program({ins(0x11, lit(2), link(2, 11)), kHalt});
  EXPECT_EQ(run(), kInvalid);
  EXPECT_EQ(mem.read32(stray), 0xA5A5A5A5u);
}

// --- Native-backed and global ops ------------------------------------------

TEST_F(GoolInterpTest, RandWithEqualBoundsSkipsCallout) {
  EXPECT_EQ(binop(0x10, 9, 9), 9u);
  EXPECT_TRUE(g_calls.empty());
}
TEST_F(GoolInterpTest, RandCallsNativeWithRangeAndAddsLowerBound) {
  g_returns[kFnRand] = {3};
  EXPECT_EQ(binop(0x10, 20, 5), 8u);
  ASSERT_EQ(g_calls.size(), 1u);
  EXPECT_EQ(g_calls[0].addr, kFnRand);
  EXPECT_EQ(g_calls[0].a0, 15u) << "a - b";
}
TEST_F(GoolInterpTest, PadQueriesTestControlsWithLow21Bits) {
  g_returns[kFnTestControls] = {0x77};
  program({(0x1Au << 24) | 0xFABCDE, kHalt});
  run();
  ASSERT_EQ(g_calls.size(), 1u);
  EXPECT_EQ(g_calls[0].a0, 0x1ABCDEu);
  EXPECT_EQ(g_calls[0].a1, 0u);
  EXPECT_EQ(top(), 0x77u);
}
// SPD: B + A * gamespeed / 1024, gamespeed (dispenv+0x84) capped at 0x66.
TEST_F(GoolInterpTest, SpeedScalesByGameSpeed) {
  mem.write32(kDispEnv + 0x84, 0x40);
  EXPECT_EQ(binop(0x1B, 0x100, 5), 5u + 0x10);
}
TEST_F(GoolInterpTest, SpeedClampsGameSpeedAt0x66) {
  mem.write32(kDispEnv + 0x84, 0x200);
  EXPECT_EQ(binop(0x1B, 0x100, 5), 5u + (0x100 * 0x66 >> 10));
}
// Negative products round toward zero (+0x3FF before >>10), like a signed div.
TEST_F(GoolInterpTest, SpeedRoundsNegativeTowardZero) {
  mem.write32(kDispEnv + 0x84, 0x40);
  EXPECT_EQ(binop(0x1B, static_cast<uint32_t>(-1), 5), 5u);
}
TEST_F(GoolInterpTest, PsinCallsSinWithPhaseAndScalesByAmplitude) {
  g_returns[kFnSin] = {0};
  // angle = (B << 11) / A - 0x400 = 0; result = (sin + 0x1000) * A >> 13.
  EXPECT_EQ(binop(0x1D, 0x400, 0x200), 0x200u);
  ASSERT_EQ(g_calls.size(), 1u);
  EXPECT_EQ(g_calls[0].addr, kFnSin);
  EXPECT_EQ(g_calls[0].a0, 0u);
}
TEST_F(GoolInterpTest, TickAddsRenderCountModuloB) {
  mem.write32(kGRenderCount, 10);
  EXPECT_EQ(binop(0x1E, 3, 4), 1u);
}
TEST_F(GoolInterpTest, GlobalsAreIndexedByBOver256) {
  setReg(8, 0xC0FFEE);
  program({ins(0x20, reg(8), lit(2)), ins(0x1F, 0, lit(2)), kHalt});
  run();
  EXPECT_EQ(mem.read32(kGGlobals + 2 * 4), 0xC0FFEEu);
  EXPECT_EQ(top(), 0xC0FFEEu);
}
// 0x23/0x24: 16-bit colour channel of the object linked by register 0..7.
TEST_F(GoolInterpTest, ColorChannelReadAndWrite) {
  const uint32_t color = kObj + offsetof(goolobj, colors);
  mem.write16(color + 2 * 2, 0x1234);
  setReg(8, 0x0ABC);
  program({(0x23u << 24) | (2u << 15) | (0u << 12),
           (0x24u << 24) | (5u << 15) | (0u << 12) | reg(8), kHalt});
  run();
  EXPECT_EQ(top(), 0x1234u);
  EXPECT_EQ(mem.read16(color + 5 * 2), 0x0ABCu);
}

// --- Control flow (0x82) ---------------------------------------------------

TEST_F(GoolInterpTest, BranchIsRelativeToNextInstruction) {
  program({branch(1), ins(0x11, lit(1), kTop), ins(0x11, lit(2), kTop),
           kHalt});
  run();
  ASSERT_EQ(depth(), 1u);
  EXPECT_EQ(top(), 0x200u) << "instruction 1 skipped";
}
TEST_F(GoolInterpTest, BranchBackwardWithNegativeOffset) {
  // 0: jump to 3; 1: push; 2: halt; 3: jump back to 1.
  program({branch(2), ins(0x11, lit(7), kTop), kHalt, branch(-3)});
  run();
  ASSERT_EQ(depth(), 1u);
  EXPECT_EQ(top(), 0x700u);
}
TEST_F(GoolInterpTest, BranchVarskipDropsStackWords) {
  pushWord(1);
  pushWord(2);
  pushWord(3);
  program({branch(0, 2), kHalt});
  run();
  EXPECT_EQ(depth(), 1u);
}
TEST_F(GoolInterpTest, ConditionalBranchOnRegister) {
  setReg(8, 1);
  setReg(9, 0);
  // bnez r8 (taken) over push 1; bnez r9 (not taken); push 2.
  program({branch(1, 0, 1, 8), ins(0x11, lit(1), kTop), branch(1, 0, 1, 9),
           ins(0x11, lit(2), kTop), kHalt});
  run();
  ASSERT_EQ(depth(), 1u);
  EXPECT_EQ(top(), 0x200u);
}
TEST_F(GoolInterpTest, InvertedConditionalBranchOnZero) {
  setReg(9, 0);
  program({branch(1, 0, 2, 9), ins(0x11, lit(1), kTop), kHalt});
  run();
  EXPECT_EQ(depth(), 0u);
}
TEST_F(GoolInterpTest, ConditionRegister0x1FPopsTheStack) {
  pushWord(1);
  program({branch(1, 0, 1, 0x1F), ins(0x11, lit(1), kTop), kHalt});
  run();
  EXPECT_EQ(depth(), 0u) << "condition popped, push skipped";
}
// condtype 3 reuses the condition of the previous instruction of this kind.
TEST_F(GoolInterpTest, Condtype3ReusesPreviousCondition) {
  setReg(9, 0);
  program({branch(0, 0, 1, 9), // evaluates false
           branch(1, 0, 3, 0), // reuses false: not taken
           ins(0x11, lit(1), kTop), kHalt});
  run();
  ASSERT_EQ(depth(), 1u);
  EXPECT_EQ(top(), 0x100u);
}
TEST_F(GoolInterpTest, Optype3NeverActs) {
  program({flow(0x82, 3, 0, 0, 1), ins(0x11, lit(1), kTop), kHalt});
  run();
  EXPECT_EQ(depth(), 1u);
}

// State change (optype 1): blocked when the target state's flags intersect
// statusc; otherwise ChangeObjectState(obj, state, 0, 0) runs natively.
TEST_F(GoolInterpTest, StateChangeCallsChangeObjectStateAndPops) {
  mem.write32(kStates + 2 * 16, 0x4); // goolstateinfo[2].flags
  pushWord(0x99);
  g_returns[kFnChangeState] = {kSuccess};
  program({flow(0x82, 1, 0, 0, 2), kHalt});
  EXPECT_EQ(run(), kInvalid) << "flags bit 1 clear: continue after the pop";
  ASSERT_EQ(g_calls.size(), 1u);
  EXPECT_EQ(g_calls[0].addr, kFnChangeState);
  EXPECT_EQ(g_calls[0].a0, kObj);
  EXPECT_EQ(g_calls[0].a1, 2u);
  EXPECT_EQ(depth(), 0u);
}
TEST_F(GoolInterpTest, StateChangeReturnsResultWhenFlagBit1Set) {
  g_returns[kFnChangeState] = {kSuccess};
  program({flow(0x82, 1, 0, 0, 2), ins(0x11, lit(1), kTop)});
  EXPECT_EQ(run(2), kSuccess);
  EXPECT_EQ(depth(), 0u);
}
TEST_F(GoolInterpTest, StateChangeBlockedByStatusc) {
  mem.write32(kStates + 2 * 16, 0x4);
  field(offsetof(goolobj, statusc), 0x4);
  program({flow(0x82, 1, 0, 0, 2), kHalt});
  run();
  EXPECT_TRUE(g_calls.empty());
}
// Display modes 2..4 additionally treat statusc bits 0x1002 as set.
TEST_F(GoolInterpTest, StateChangeBlockedByDisplayModeImplicitBits) {
  mem.write32(kStates + 2 * 16, 0x2);
  field(offsetof(goolobj, displaymode), 3);
  program({flow(0x82, 1, 0, 0, 2), kHalt});
  run();
  EXPECT_TRUE(g_calls.empty());
}

// --- Subroutine call / return ----------------------------------------------

TEST_F(GoolInterpTest, JalBuildsFrameAndReturnRestoresCaller) {
  setReg(9, 0);
  // main: push two args, call sub(2 args), push marker, halt.
  program({ins(0x11, lit(5), kTop), ins(0x11, lit(3), kTop), jal(16, 2),
           ins(0x11, lit(1), kTop), kHalt});
  // sub: r9 = arg0 - arg1 via fp-relative vars, then return.
  program({ins(0x01, var(-1), var(-2)), ins(0x11, kTop, reg(9)), ret()}, 16);
  EXPECT_EQ(run(), kInvalid);
  EXPECT_EQ(getReg(9), 0x200u) << "fp[-2] - fp[-1] = 5 - 3";
  ASSERT_EQ(depth(), 1u) << "return dropped the callee frame and both args";
  EXPECT_EQ(top(), 0x100u);
  EXPECT_EQ(field(offsetof(goolobj, fp)), kFp) << "caller fp restored";
}
TEST_F(GoolInterpTest, JalPushesFlagsReturnPcAndFrameRange) {
  program({jal(16, 0)});
  program({kHalt}, 16);
  run(0x40);
  ASSERT_EQ(depth(), 3u);
  EXPECT_EQ(field(offsetof(goolobj, fp)), kStack);
  EXPECT_EQ(mem.read32(kStack), 0x40u);
  EXPECT_EQ(mem.read32(kStack + 4), kCode + 4);
  EXPECT_EQ(mem.read32(kStack + 8),
            ((kFp - kSelf) << 16) | (kStack - kSelf));
}
TEST_F(GoolInterpTest, ReturnFromInitialFrameIsInvalidReturn) {
  mem.write32(kFp + 8, 0); // frame range with no caller fp
  program({ret()});
  EXPECT_EQ(run(), kInvalidReturn);
}
// Flags bit 0: return ends the interpretation once the caller is restored.
TEST_F(GoolInterpTest, ReturnWithFlagBit0EndsInterpretation) {
  callerFrame(kCode + 0x40);
  program({ret()});
  EXPECT_EQ(run(1), kSuccess);
  EXPECT_EQ(field(offsetof(goolobj, pc)), kCode + 0x40);
  EXPECT_EQ(depth(), 0u);
}
// Bit 4 additionally pushes 0, the initial value for a following wait.
TEST_F(GoolInterpTest, ReturnWithFlagBit4PushesWaitSeed) {
  callerFrame(kCode + 0x40);
  mem.write32(kStack, 0xDEAD);
  program({ret()});
  EXPECT_EQ(run(0x11), kSuccess);
  ASSERT_EQ(depth(), 1u);
  EXPECT_EQ(top(), 0u);
}
TEST_F(GoolInterpTest, ReturnInsideEventHandlerWithoutAcceptIsInvalid) {
  mem.write32(kFp + 8, 0);
  program({ret()});
  EXPECT_EQ(run(8), kInvalidStateReturn);
}

// --- Event status (0x88 reject / 0x89 accept) ------------------------------

TEST_F(GoolInterpTest, AcceptSetsGuardAndMarksReturnEvent) {
  mem.write32(kFp + 8, 0);
  mem.write32(kTrans, 0x1234);
  program({flow(0x89, 3, 0, 0, 0), ret()});
  EXPECT_EQ(run(8), kSuccess) << "RETURNEVENT set: ret becomes success";
  EXPECT_EQ(mem.read32(kTrans + 4), 1u) << "guard";
  EXPECT_EQ(mem.read32(kTrans), 0xFFu) << "null state";
}
TEST_F(GoolInterpTest, RejectClearsGuard) {
  mem.write32(kFp + 8, 0);
  mem.write32(kTrans + 4, 1);
  program({flow(0x88, 3, 0, 0, 0), ret()});
  run(8);
  EXPECT_EQ(mem.read32(kTrans + 4), 0u);
}
TEST_F(GoolInterpTest, AcceptAndChangeStateReturnsToCallerWithState) {
  callerFrame(kCode + 0x40);
  program({flow(0x89, 1, 0, 0, 0x2A)});
  EXPECT_EQ(run(8), kSuccess);
  EXPECT_EQ(mem.read32(kTrans), 0x2Au);
  EXPECT_EQ(mem.read32(kTrans + 4), 1u);
  EXPECT_EQ(field(offsetof(goolobj, pc)), kCode + 0x40);
  EXPECT_EQ(depth(), 0u);
}
TEST_F(GoolInterpTest, AcceptAndReturnSetsNullState) {
  callerFrame(kCode + 0x40);
  program({flow(0x89, 2, 0, 0, 0x2A)});
  EXPECT_EQ(run(8), kSuccess);
  EXPECT_EQ(mem.read32(kTrans), 0xFFu);
}
// JAL clears flags bits 0 and 3, so a subroutine cannot accept an event.
TEST_F(GoolInterpTest, JalLeavesEventHandlerMode) {
  program({jal(16, 0)});
  program({flow(0x89, 3, 0, 0, 0)}, 16);
  EXPECT_EQ(run(8), kInvalid);
}
TEST_F(GoolInterpTest, AcceptAndReturnFromInitialFrameLeavesState) {
  mem.write32(kFp + 8, 0);
  mem.write32(kTrans, 0x1234);
  program({flow(0x88, 1, 0, 0, 0x2A)});
  EXPECT_EQ(run(8), kInvalidReturn);
  EXPECT_EQ(mem.read32(kTrans), 0x1234u);
}
TEST_F(GoolInterpTest, FalseConditionBranchesInsteadOfAccepting) {
  mem.write32(kFp + 8, 0);
  setReg(9, 0);
  // 0: if r9 accept else skip one; 1: push; 2: ret.
  program({flow(0x89, 0, 1, 9, 1), ins(0x11, lit(1), kTop), ret()});
  EXPECT_EQ(run(8), kInvalidStateReturn) << "RETURNEVENT never set";
  EXPECT_EQ(mem.read32(kStack), 0u) << "push skipped by the branch";
}

// --- Events ----------------------------------------------------------------

uint32_t eventIns(uint32_t op, uint32_t mode, uint32_t argc, uint32_t creg,
                  uint32_t evgop) {
  return (op << 24) | (mode << 21) | (argc << 18) | (creg << 12) | evgop;
}

TEST_F(GoolInterpTest, BroadcastEventPassesArgsAndPopsThem) {
  setReg(9, 0x1300); // event id
  pushWord(0xA1);
  pushWord(0xA2);
  static std::vector<uint32_t> seenArgs;
  seenArgs.clear();
  g_sideEffect = [](recomp_context *c, uint32_t) {
    uint32_t args = c->mem->read32(c->r[29] + 16);
    seenArgs = {c->mem->read32(args), c->mem->read32(args + 4)};
  };
  program({eventIns(0x8F, 3, 2, 0, reg(9)), kHalt});
  run();
  ASSERT_EQ(g_calls.size(), 1u);
  EXPECT_EQ(g_calls[0].addr, kFnEvBroadcast);
  EXPECT_EQ(g_calls[0].a0, kObj);
  EXPECT_EQ(g_calls[0].a1, 0x1300u);
  EXPECT_EQ(g_calls[0].a2, 3u) << "mode";
  EXPECT_EQ(g_calls[0].a3, 2u) << "argc";
  EXPECT_EQ(seenArgs, (std::vector<uint32_t>{0xA1, 0xA2}));
  EXPECT_EQ(depth(), 0u);
}
TEST_F(GoolInterpTest, DirectEventGoesToRecipientRegister) {
  setReg(1, kOther); // recipient = parent (reg field shares mode bits)
  setReg(9, 0x1400);
  program({eventIns(0x87, 1, 0, 0, reg(9)), kHalt});
  run();
  ASSERT_EQ(g_calls.size(), 1u);
  EXPECT_EQ(g_calls[0].addr, kFnEvDirect);
  EXPECT_EQ(g_calls[0].a1, kOther);
  EXPECT_EQ(g_calls[0].a2, 0x1400u);
}
TEST_F(GoolInterpTest, CascadeEventPassesModeAndArgsOnStack) {
  setReg(1, kOther);
  setReg(9, 0x1500);
  pushWord(0xA1);
  program({eventIns(0x90, 1, 1, 0, reg(9)), kHalt});
  run();
  ASSERT_EQ(g_calls.size(), 1u);
  EXPECT_EQ(g_calls[0].addr, kFnEvCascade);
  EXPECT_EQ(g_calls[0].a3, 1u) << "mode";
  EXPECT_EQ(g_calls[0].s4, 1u) << "argc is the 5th argument";
  EXPECT_EQ(mem.read32(g_calls[0].s5), 0xA1u) << "args buffer, 6th argument";
}
TEST_F(GoolInterpTest, DirectEventWithoutRecipientClearsMiscAndSkips) {
  setReg(2, 0);
  setReg(9, 0x1400);
  field(offsetof(goolobj, misc_flag), 7);
  pushWord(0xA1);
  program({eventIns(0x87, 2, 1, 0, reg(9)), kHalt});
  run();
  EXPECT_TRUE(g_calls.empty());
  EXPECT_EQ(field(offsetof(goolobj, misc_flag)), 0u);
  EXPECT_EQ(depth(), 0u) << "args still popped";
}
TEST_F(GoolInterpTest, EventWithFalseConditionIsNotSent) {
  setReg(8, 0);
  setReg(9, 0x1400);
  program({eventIns(0x8F, 0, 0, 8, reg(9)), kHalt});
  run();
  EXPECT_TRUE(g_calls.empty());
}
void markStateChanged(recomp_context *c, uint32_t) {
  uint32_t a = kObj + offsetof(goolobj, statusa);
  c->mem->write32(a, c->mem->read32(a) | 0x20000);
}
// If the event changed the sender's state, the interpretation ends (flags
// bit 1) or one word is popped instead of the args.
TEST_F(GoolInterpTest, EventThatChangesStateReturnsWhenFlagBit1Set) {
  setReg(9, 0x1400);
  g_sideEffect = markStateChanged;
  program({eventIns(0x8F, 0, 0, 0, reg(9)), ins(0x11, lit(1), kTop)});
  EXPECT_EQ(run(2), kSuccess);
  EXPECT_EQ(depth(), 0u);
}
TEST_F(GoolInterpTest, EventThatChangesStatePopsOneWord) {
  setReg(9, 0x1400);
  g_sideEffect = markStateChanged;
  pushWord(1);
  pushWord(2);
  pushWord(3);
  program({eventIns(0x8F, 0, 2, 0, reg(9)), kHalt});
  run();
  EXPECT_EQ(depth(), 2u);
}
TEST_F(GoolInterpTest, EventClearsStateChangedMarkerFirst) {
  setReg(8, 0);
  setReg(9, 0x1400);
  field(offsetof(goolobj, statusa), 0x20001);
  pushWord(1);
  program({eventIns(0x8F, 0, 1, 8, reg(9)), kHalt});
  run();
  EXPECT_EQ(field(offsetof(goolobj, statusa)), 1u);
  EXPECT_EQ(depth(), 0u) << "stale marker must not take the state-changed path";
}

// --- Spawning --------------------------------------------------------------

uint32_t spawnIns(uint32_t op, uint32_t argc, uint32_t type, uint32_t sub,
                  uint32_t count) {
  return (op << 24) | (argc << 20) | (type << 12) | (sub << 6) | count;
}

TEST_F(GoolInterpTest, SpawnCreatesEachChildAndLinksCreator) {
  constexpr uint32_t kChildA = 0x80112000, kChildB = 0x80113000;
  g_returns[kFnCreateObject] = {kChildA, kChildB};
  pushWord(0xA1);
  program({spawnIns(0x8A, 1, 0x33, 5, 2), kHalt});
  run();
  ASSERT_EQ(g_calls.size(), 2u);
  EXPECT_EQ(g_calls[0].a0, kObj);
  EXPECT_EQ(g_calls[0].a1, 0x33u);
  EXPECT_EQ(g_calls[0].a2, 5u);
  EXPECT_EQ(g_calls[0].a3, 1u);
  EXPECT_EQ(g_calls[0].s4, kStack) << "args point at the pushed words";
  EXPECT_EQ(g_calls[0].s5, 0u) << "0x8A passes flag 0";
  EXPECT_EQ(field(offsetof(goolobj, misc_child)), kChildB);
  EXPECT_EQ(mem.read32(kChildA + offsetof(goolobj, creator)), kObj);
  EXPECT_EQ(mem.read32(kChildB + offsetof(goolobj, creator)), kObj);
  EXPECT_EQ(depth(), 0u);
}
TEST_F(GoolInterpTest, SpawnFailureClearsMiscChild) {
  g_returns[kFnCreateObject] = {kAllocFailed};
  field(offsetof(goolobj, misc_child), 0x80112000);
  program({spawnIns(0x8A, 0, 1, 0, 1), kHalt});
  run();
  EXPECT_EQ(field(offsetof(goolobj, misc_child)), 0u);
}

// --- Animation ---------------------------------------------------------------

uint32_t animIns(uint32_t flip, uint32_t wait, uint32_t anim, uint32_t frame) {
  return (0x83u << 24) | (flip << 22) | (wait << 16) | (anim << 7) | frame;
}

TEST_F(GoolInterpTest, ChangeAnimSetsSequenceFrameAndPushesWaitStamp) {
  mem.write32(kGTicks, 0x55);
  program({animIns(3, 6, 4, 2), kHalt});
  run();
  EXPECT_EQ(field(offsetof(goolobj, animseq)), kAnims + 4 * 4);
  EXPECT_EQ(field(offsetof(goolobj, animframe)), 2u << 8);
  ASSERT_EQ(depth(), 1u);
  EXPECT_EQ(top(), (6u << 24) | 0x55);
  EXPECT_TRUE(g_calls.empty()) << "non-solid object: no range test";
}
TEST_F(GoolInterpTest, ChangeAnimFlipModes) {
  const size_t sx = offsetof(goolobj, scale);
  field(sx, 100);
  program({animIns(0, 0, 0, 0), kHalt});
  run();
  EXPECT_EQ(static_cast<int32_t>(field(sx)), -100) << "flip 0: force negative";

  field(offsetof(goolobj, pc), kCode);
  program({animIns(1, 0, 0, 0), kHalt});
  run();
  EXPECT_EQ(static_cast<int32_t>(field(sx)), 100) << "flip 1: force positive";

  field(offsetof(goolobj, pc), kCode);
  program({animIns(2, 0, 0, 0), kHalt});
  run();
  EXPECT_EQ(static_cast<int32_t>(field(sx)), -100) << "flip 2: toggle";
}
TEST_F(GoolInterpTest, ChangeAnimOnSolidObjectFlipsWhenInRange) {
  field(offsetof(goolobj, statusb), 0x8);
  mem.write32(kGCrash, kOther);
  g_returns[kFnSolidRange] = {0};
  program({animIns(3, 0, 0, 0), kHalt});
  run();
  ASSERT_EQ(g_calls.size(), 2u);
  EXPECT_EQ(g_calls[0].addr, kFnSolidRange);
  EXPECT_EQ(g_calls[0].a1, kObj + offsetof(goolobj, trans));
  EXPECT_EQ(g_calls[0].a2, kOther + offsetof(goolobj, trans));
  EXPECT_EQ(g_calls[0].a3, 0x7D000u);
  EXPECT_EQ(g_calls[1].addr, kFnObjectFlip);
  EXPECT_EQ(g_calls[1].a1, kObj + offsetof(goolobj, scale));
}
TEST_F(GoolInterpTest, ChangeAnimOnSolidObjectOutOfRangeDoesNotFlip) {
  field(offsetof(goolobj, statusb), 0x10);
  g_returns[kFnSolidRange] = {1};
  program({animIns(3, 0, 0, 0), kHalt});
  run();
  ASSERT_EQ(g_calls.size(), 1u);
  EXPECT_EQ(g_calls[0].addr, kFnSolidRange);
}
TEST_F(GoolInterpTest, ChangeAnimReturnsWhenFlagBit2Set) {
  program({animIns(3, 0, 0, 0), ins(0x11, lit(1), kTop)});
  EXPECT_EQ(run(4), kSuccess);
  EXPECT_EQ(depth(), 1u) << "only the wait stamp";
}
TEST_F(GoolInterpTest, ChangeAnimFrameTakesFrameFromOperandAndFlips) {
  mem.write32(kGTicks, 0x7);
  setReg(9, 0x345);
  field(offsetof(goolobj, scale), static_cast<uint32_t>(-5));
  program({(0x84u << 24) | (1u << 22) | (2u << 16) | reg(9)});
  EXPECT_EQ(run(4), kSuccess);
  EXPECT_EQ(field(offsetof(goolobj, animframe)), 0x345u);
  EXPECT_EQ(top(), (2u << 24) | 0x7);
  EXPECT_EQ(field(offsetof(goolobj, scale)), 5u);
  ASSERT_EQ(g_calls.size(), 1u);
  EXPECT_EQ(g_calls[0].addr, kFnObjectFlip);
}

// --- NS paging (0x8B) --------------------------------------------------------
//
// Two NS lookups (operand A, operand B); the word behind the first selects the
// loader: 1/6 -> A (misc), 2 -> B (misc), 3 -> B (push), 4 -> C (push),
// 5 -> D (pops a count of words, pushes result). Anything else does nothing.

class GoolPagingTest : public GoolInterpTest {
protected:
  static constexpr uint32_t kKind = 0x80129000;
  static constexpr uint32_t kEntry = 0x80129100;
  void runPaging(uint32_t kind, uint32_t loader, uint32_t result) {
    mem.write32(kKind, kind);
    g_returns[kFnNsLookup] = {kKind, kEntry};
    g_returns[loader] = {result};
    field(offsetof(goolobj, pc), kCode);
    program({ins(0x8B, 0x12, 0x34), kHalt});
    run();
  }
};

TEST_F(GoolPagingTest, LooksUpBothOperands) {
  runPaging(1, kFnPageA, 0);
  ASSERT_GE(g_calls.size(), 2u);
  EXPECT_EQ(g_calls[0].addr, kFnNsLookup);
  EXPECT_EQ(g_calls[0].a0, kObj);
  EXPECT_EQ(g_calls[0].a1, 0x12u);
  EXPECT_EQ(g_calls[1].a1, 0x34u);
}
TEST_F(GoolPagingTest, Kind1LoadsThroughPageAIntoMisc) {
  runPaging(1, kFnPageA, 0xCAFE);
  ASSERT_EQ(g_calls.size(), 3u);
  EXPECT_EQ(g_calls[2].addr, kFnPageA);
  EXPECT_EQ(g_calls[2].a0, kEntry);
  EXPECT_EQ(g_calls[2].a1, 0u);
  EXPECT_EQ(g_calls[2].a2, 1u);
  EXPECT_EQ(field(offsetof(goolobj, misc_flag)), 0xCAFEu);
}
TEST_F(GoolPagingTest, Kind6LoadsThroughPageAWithFlag) {
  runPaging(6, kFnPageA, 0xCAFE);
  ASSERT_EQ(g_calls.size(), 3u);
  EXPECT_EQ(g_calls[2].a1, 1u);
}
TEST_F(GoolPagingTest, Kind2LoadsThroughPageBIntoMisc) {
  runPaging(2, kFnPageB, 0xBEEF);
  ASSERT_EQ(g_calls.size(), 3u);
  EXPECT_EQ(g_calls[2].addr, kFnPageB);
  EXPECT_EQ(g_calls[2].a1, 1u);
  EXPECT_EQ(field(offsetof(goolobj, misc_flag)), 0xBEEFu);
  EXPECT_EQ(depth(), 0u);
}
TEST_F(GoolPagingTest, Kind3PushesPageBResult) {
  runPaging(3, kFnPageB, 0xBEEF);
  ASSERT_EQ(g_calls.size(), 3u);
  EXPECT_EQ(g_calls[2].a1, 0u);
  ASSERT_EQ(depth(), 1u);
  EXPECT_EQ(top(), 0xBEEFu);
}
TEST_F(GoolPagingTest, Kind4PushesPageCResult) {
  runPaging(4, kFnPageC, 0xF00D);
  ASSERT_EQ(g_calls.size(), 3u);
  EXPECT_EQ(g_calls[2].addr, kFnPageC);
  ASSERT_EQ(depth(), 1u);
  EXPECT_EQ(top(), 0xF00Du);
}
TEST_F(GoolPagingTest, Kind5ReplacesCountedWordsWithPageDResult) {
  mem.write32(kEntry, 2); // word count behind the second lookup
  pushWord(0xA1);
  pushWord(0xA2);
  runPaging(5, kFnPageD, 0xD00D);
  ASSERT_EQ(g_calls.size(), 3u);
  EXPECT_EQ(g_calls[2].addr, kFnPageD);
  EXPECT_EQ(g_calls[2].a0, kStack) << "points at the first counted word";
  EXPECT_EQ(g_calls[2].a1, 2u);
  ASSERT_EQ(depth(), 1u);
  EXPECT_EQ(top(), 0xD00Du);
}
TEST_F(GoolPagingTest, UnknownKindLoadsNothing) {
  runPaging(7, kFnPageA, 0);
  EXPECT_EQ(g_calls.size(), 2u);
  runPaging(0, kFnPageA, 0);
  EXPECT_EQ(g_calls.size(), 4u) << "kind 0 wraps below the table too";
}

// --- EMU_* shim (emu_compat.cpp) -------------------------------------------

class EmuCompatTest : public ::testing::Test {
protected:
  ps1::Memory mem;
  recomp_context ctx;
  void SetUp() override {
    mem.reset();
    ctx.reset();
    ctx.mem = &mem;
    ps1::emuptr_set_ram(mem.ramPtr());
    ps1::gool::emu_bind(
        static_cast<uint8_t *>(ps1::emuptr_translate(0x80000000u)), &ctx);
  }
  void TearDown() override {
    ps1::gool::emu_unbind();
    g_testRecompDispatchHook = nullptr;
    ps1::emuptr_set_ram(nullptr);
  }
};

// The shim routes through Memory so the scratchpad (where Crash keeps the
// GOOL const buffer) is reachable, not just main RAM.
TEST_F(EmuCompatTest, NarrowAccessesRoundTripThroughScratchpad) {
  using namespace ps1::gool;
  EMU_Write8(0x1F800010, 0x80);
  EMU_Write16(0x1F800012, 0x8001);
  EXPECT_EQ(EMU_ReadU8(0x1F800010), 0x80u);
  EXPECT_EQ(EMU_ReadS8(0x1F800010), -128);
  EXPECT_EQ(EMU_ReadU16(0x1F800012), 0x8001u);
  EXPECT_EQ(EMU_ReadS16(0x1F800012), static_cast<int16_t>(0x8001));
  EXPECT_EQ(mem.read16(0x1F800012), 0x8001u);
}
TEST_F(EmuCompatTest, RegisterAliasesReachLoAndHi) {
  ps1::gool::emu_reg(32) = 0x11;
  ps1::gool::emu_reg(33) = 0x22;
  ps1::gool::emu_reg(4) = 0x33;
  EXPECT_EQ(ctx.lo, 0x11u);
  EXPECT_EQ(ctx.hi, 0x22u);
  EXPECT_EQ(ctx.r[4], 0x33u);
}
TEST_F(EmuCompatTest, AddressMapsHostPointerBackToKseg0) {
  auto *p = static_cast<uint8_t *>(ps1::emuptr_translate(0x80012340u));
  EXPECT_EQ(EMUADDR(p), 0x80012340u);
}
TEST_F(EmuCompatTest, AddressOfForeignPointerIsZero) {
  int local = 0;
  testing::internal::CaptureStderr();
  EXPECT_EQ(EMUADDR(&local), 0u);
  EXPECT_NE(testing::internal::GetCapturedStderr().find("outside bound RAM"),
            std::string::npos);
}
// MIPS MULT/MULTU: 64-bit product split into HI:LO.
TEST_F(EmuCompatTest, SignedMultiplySplitsIntoHiLo) {
  ps1::gool::EMU_SMultiply(-3, 5);
  EXPECT_EQ(ctx.lo, static_cast<uint32_t>(-15));
  EXPECT_EQ(ctx.hi, 0xFFFFFFFFu);
}
TEST_F(EmuCompatTest, UnsignedMultiplySplitsIntoHiLo) {
  ps1::gool::EMU_UMultiply(0xFFFFFFFFu, 2u);
  EXPECT_EQ(ctx.lo, 0xFFFFFFFEu);
  EXPECT_EQ(ctx.hi, 1u);
}
TEST_F(EmuCompatTest, OrdinaryUnsignedDivide) {
  ps1::gool::EMU_UDivide(23u, 5u);
  EXPECT_EQ(ctx.lo, 4u);
  EXPECT_EQ(ctx.hi, 3u);
}
// o32: args 0..3 in A0..A3, args 4+ in the caller's outgoing area at SP+16.
TEST_F(EmuCompatTest, InvokePlacesFifthArgumentOnStackAndRestoresSpRa) {
  ctx.r[29] = 0x801F0000;
  ctx.r[31] = 0x80001234;
  static uint32_t seenStackArg = 0;
  g_testRecompDispatchHook = [](recomp_context *c, uint32_t) {
    seenStackArg = c->mem->read32(c->r[29] + 16);
    c->r[31] = 0; // callee clobbers RA
    c->r[2] = 0x99;
  };
  uint32_t v0 = ps1::gool::EMU_Invoke(0x80010000, 5, 1u, 2u, 3u, 4u, 0x55u);
  EXPECT_EQ(v0, 0x99u);
  EXPECT_EQ(seenStackArg, 0x55u);
  EXPECT_EQ(ctx.r[7], 4u);
  EXPECT_EQ(ctx.r[29], 0x801F0000u);
  EXPECT_EQ(ctx.r[31], 0x80001234u);
}

} // namespace
