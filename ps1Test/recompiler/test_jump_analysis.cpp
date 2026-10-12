// Computed in-function jump (Duff's device) detection.
//
// Ground truth is func_80033FBC in Crash Bandicoot (SCUS-94900), the PsyQ
// memcpy: an unrolled copy loop entered partway through.  Before this
// detector the `jr` emitted a runtime dispatch on a mid-function address and
// the game aborted with `unmapped call to 0x80034048` the moment the title
// screen accepted START.

#include <gtest/gtest.h>
#include <ps1recomp/instruction_emitter.h>
#include <ps1recomp/jump_analysis.h>

#include <algorithm>

using namespace ps1recomp;

namespace {

// Hand-assembled MIPS words, so the test pins the encoding the detector must
// match rather than trusting a second copy of the pattern matcher.
constexpr uint32_t lui(uint8_t rt, uint16_t imm) {
  return (0x0Fu << 26) | (static_cast<uint32_t>(rt) << 16) | imm;
}
constexpr uint32_t addiu(uint8_t rt, uint8_t rs, uint16_t imm) {
  return (0x09u << 26) | (static_cast<uint32_t>(rs) << 21) |
         (static_cast<uint32_t>(rt) << 16) | imm;
}
constexpr uint32_t sltiu(uint8_t rt, uint8_t rs, uint16_t imm) {
  return (0x0Bu << 26) | (static_cast<uint32_t>(rs) << 21) |
         (static_cast<uint32_t>(rt) << 16) | imm;
}
constexpr uint32_t sll(uint8_t rd, uint8_t rt, uint8_t sa) {
  return (static_cast<uint32_t>(rt) << 16) | (static_cast<uint32_t>(rd) << 11) |
         (static_cast<uint32_t>(sa) << 6);
}
constexpr uint32_t subu(uint8_t rd, uint8_t rs, uint8_t rt) {
  return (static_cast<uint32_t>(rs) << 21) | (static_cast<uint32_t>(rt) << 16) |
         (static_cast<uint32_t>(rd) << 11) | 0x23u;
}
constexpr uint32_t addu(uint8_t rd, uint8_t rs, uint8_t rt) {
  return (static_cast<uint32_t>(rs) << 21) | (static_cast<uint32_t>(rt) << 16) |
         (static_cast<uint32_t>(rd) << 11) | 0x21u;
}
constexpr uint32_t jr(uint8_t rs) {
  return (static_cast<uint32_t>(rs) << 21) | 0x08u;
}
constexpr uint32_t kNop = 0u;

constexpr uint32_t kFuncAddr = 0x80033FBC;

// The shape at func_80033FBC+108: base 0x80034058 is the loop tail, $a2 the
// remaining count, guarded by `sltiu $at, $a2, 9`.
std::vector<uint32_t> duffFunction(uint32_t base, bool descending) {
  std::vector<uint32_t> f((0x80034210u - kFuncAddr) / 4, kNop);
  const size_t jrIdx = (0x80034028u - kFuncAddr) / 4;
  const uint16_t lo = static_cast<uint16_t>(base & 0xFFFFu);
  f[jrIdx - 5] = sltiu(1, 6, 9);            // sltiu $at, $a2, 9
  f[jrIdx - 4] = lui(8, base >> 16);        // lui   $t0, 0x8003
  f[jrIdx - 3] = addiu(8, 8, lo);           // addiu $t0, $t0, lo
  f[jrIdx - 2] = sll(1, 6, 2);              // sll   $at, $a2, 2
  f[jrIdx - 1] = descending ? subu(8, 8, 1) // subu  $t0, $t0, $at
                            : addu(8, 8, 1);
  f[jrIdx] = jr(8);
  return f;
}

} // namespace

TEST(ComputedCodeJump, DescendingDuffDeviceResolvesToInFunctionTargets) {
  auto f = duffFunction(0x80034058u, /*descending=*/true);
  auto targets =
      detectComputedCodeJump(f, (0x80034028u - kFuncAddr) / 4, kFuncAddr);

  // sltiu bound 9 -> base - 0..8 words.
  ASSERT_EQ(targets.size(), 9u);
  EXPECT_EQ(targets.front(), 0x80034058u);
  EXPECT_EQ(targets.back(), 0x80034038u);

  // The address the game actually reached with count == 4, and the one that
  // used to abort the run.
  EXPECT_NE(std::find(targets.begin(), targets.end(), 0x80034048u),
            targets.end());
}

TEST(ComputedCodeJump, AscendingFormWalksForwardFromTheBase) {
  auto f = duffFunction(0x80034058u, /*descending=*/false);
  auto targets =
      detectComputedCodeJump(f, (0x80034028u - kFuncAddr) / 4, kFuncAddr);

  ASSERT_EQ(targets.size(), 9u);
  EXPECT_EQ(targets.front(), 0x80034058u);
  EXPECT_EQ(targets.back(), 0x80034078u);
}

// The other in-function form, at func_80037D50 in the same binary: the base
// addresses a table of `bgez $zero, ...` branches, each followed by its delay
// slot, so the index is scaled by 8 rather than 4.
TEST(ComputedCodeJump, EightByteSlotTableUsesTheShiftAsStride) {
  constexpr uint32_t kFn = 0x80037D50;
  constexpr uint32_t kBase = 0x80037ED4;
  constexpr size_t kJrIdx = (0x80037E9Cu - kFn) / 4;

  std::vector<uint32_t> f((0x8003864Cu - kFn) / 4, kNop);
  f[kJrIdx - 4] = lui(1, kBase >> 16);
  f[kJrIdx - 3] = addiu(1, 1, static_cast<uint16_t>(kBase & 0xFFFFu));
  f[kJrIdx - 2] = sll(2, 2, 3); // sll $v0, $v0, 3
  f[kJrIdx - 1] = addu(1, 1, 2);
  f[kJrIdx] = jr(1);

  auto targets = detectComputedCodeJump(f, kJrIdx, kFn);

  ASSERT_FALSE(targets.empty());
  EXPECT_EQ(targets[0], kBase);
  EXPECT_EQ(targets[1], kBase + 8); // stride 8, not 4
  EXPECT_LT(targets.back(), 0x8003864Cu);
}

// func_80033878+0x54C in the same binary. The offset is built as
// `sll $s3,$at,2; addu $s3,$s3,$at; sll $s3,$s3,1` -- x10, not the x4 the
// first shift alone reads as. With x4 the table stopped at 0x80033DD8 and the
// game dispatched 0x80033DD0, an address in the middle of the function.
TEST(ComputedCodeJump, ShiftAndAddChainGivesTheWholeStride) {
  constexpr uint32_t kFn = 0x80033878;
  constexpr uint32_t kBase = 0x80033DF8;
  constexpr size_t kJrIdx = (0x80033D8Cu - kFn) / 4;

  std::vector<uint32_t> f((0x80033EF8u - kFn) / 4, kNop);
  f[kJrIdx - 7] = sltiu(18, 1, 9);
  f[kJrIdx - 6] = lui(20, kBase >> 16);
  f[kJrIdx - 5] = addiu(20, 20, static_cast<uint16_t>(kBase & 0xFFFFu));
  f[kJrIdx - 4] = sll(19, 1, 2);   // sll  $s3, $at, 2   -> x4
  f[kJrIdx - 3] = addu(19, 19, 1); // addu $s3, $s3, $at -> x5
  f[kJrIdx - 2] = sll(19, 19, 1);  // sll  $s3, $s3, 1   -> x10
  f[kJrIdx - 1] = subu(20, 20, 19);
  f[kJrIdx] = jr(20);

  auto targets = detectComputedCodeJump(f, kJrIdx, kFn);

  // The index is masked to a multiple of 4 before scaling, so only every
  // other step lands on an instruction boundary -- the odd ones are dropped.
  EXPECT_NE(std::find(targets.begin(), targets.end(), 0x80033DD0u),
            targets.end());
  for (uint32_t t : targets)
    EXPECT_EQ(t & 3u, 0u) << std::hex << t;
  EXPECT_EQ(targets.front(), kBase);
  EXPECT_EQ(targets[1], kBase - 20);
}

TEST(ComputedCodeJump, BaseOutsideTheFunctionIsRejected) {
  // A computed address pointing at another function is a call, not a local
  // branch -- resolving it to a goto would jump across function bodies.
  auto f = duffFunction(0x80034058u, /*descending=*/true);
  auto targets = detectComputedCodeJump(f, (0x80034028u - kFuncAddr) / 4,
                                        /*funcAddr=*/0x80040000u);
  EXPECT_TRUE(targets.empty());
}

TEST(ComputedCodeJump, MissingScaledIndexIsRejected) {
  // Without `sll $rs, $ridx, 2` the value is not an unrolled-loop entry and
  // the target set cannot be enumerated.
  auto f = duffFunction(0x80034058u, /*descending=*/true);
  f[(0x80034028u - kFuncAddr) / 4 - 2] = kNop; // drop the SLL
  auto targets =
      detectComputedCodeJump(f, (0x80034028u - kFuncAddr) / 4, kFuncAddr);
  EXPECT_TRUE(targets.empty());
}

TEST(ComputedCodeJump, TableShapedJumpIsLeftToTheTableDetector) {
  // `jr` whose target came out of memory has no ADDU/SUBU producing it.
  std::vector<uint32_t> f(64, kNop);
  f[32] = jr(8);
  EXPECT_TRUE(detectComputedCodeJump(f, 32, kFuncAddr).empty());
}

// Hijacked return address
//
// Ground truth is func_80033878 in the same binary, the PsyQ decompressor.
// Its prologue stashes $s0-$ra in the scratchpad and sets $ra = 0x80033C28,
// an address inside itself.  The `jr $s4` at 0x80033EEC enters one of the
// out-of-line copy stubs at 0x80033EF8+n*32; each stub ends in `jr $ra` and
// so lands back at 0x80033C28, which is where the epilogue restores the
// scratchpad.  Emitting the `jr $s4` as dispatch-then-return skips that
// epilogue: $sp keeps the copy loop's source pointer, and $s0/$s1 keep its
// working state.  Measured: func_80029B0C entered with sp=0x801FFD98 and
// returned with sp=0x8018AF4C, after which func_80025A60 spun 91M times on
// a loop counter that came back as garbage.

namespace {
constexpr uint32_t kDecompressor = 0x80033878;
constexpr uint32_t kHijackedRa = 0x80033C28;
constexpr size_t kDecompressorLen = (0x80033EF8u - kDecompressor) / 4;

// lui $ra, hi / addiu $ra, $ra, lo, then a `jr $s4` into the copy stubs.
std::vector<uint32_t> hijackedRaFunction(uint32_t raTarget) {
  std::vector<uint32_t> f(kDecompressorLen, kNop);
  f[0] = lui(31, raTarget >> 16);
  f[1] = addiu(31, 31, static_cast<uint16_t>(raTarget & 0xFFFFu));
  f[(0x80033EECu - kDecompressor) / 4] = jr(20); // jr $s4
  return f;
}
} // namespace

TEST(HijackedReturnAddress, ConstantRaInsideTheFunctionIsReported) {
  auto targets =
      detectInternalReturnTargets(hijackedRaFunction(kHijackedRa), kDecompressor);

  ASSERT_EQ(targets.size(), 1u);
  EXPECT_EQ(targets.front(), kHijackedRa);
}

TEST(HijackedReturnAddress, RaPointingOutsideTheFunctionIsARealCall) {
  // `lui/addiu $ra` to another function is the ordinary "call this, come back
  // there" idiom -- the plain return is correct and must stay.
  auto targets = detectInternalReturnTargets(
      hijackedRaFunction(0x80041000u), kDecompressor);
  EXPECT_TRUE(targets.empty());
}

TEST(HijackedReturnAddress, FunctionWithoutARaConstantReportsNothing) {
  std::vector<uint32_t> f(kDecompressorLen, kNop);
  f[0] = lui(8, 0x8003); // $t0, not $ra
  f[1] = addiu(8, 8, 0x3C28);
  EXPECT_TRUE(detectInternalReturnTargets(f, kDecompressor).empty());
}

TEST(HijackedReturnAddress, IndirectJumpResumesInsteadOfReturning) {
  ps1recomp::InstructionEmitter em;
  ps1recomp::RecompFunction f;
  f.name = "func_80033878";
  f.address = kDecompressor;
  f.instructions = hijackedRaFunction(kHijackedRa);
  f.isLabelTarget.assign(f.instructions.size(), false);
  f.size = static_cast<uint32_t>(f.instructions.size() * 4);

  const std::string out = em.emitFunction(f);

  // The stub's `jr $ra` comes back as a C++ return, so the dispatch must not
  // return with it -- it has to resume at the hijacked address.
  EXPECT_NE(out.find("if (ctx->r31 == 0x80033C28u) goto L_80033C28;"),
            std::string::npos)
      << out;

  // And that label has to be the real one at 0x80033C28, not a dispatch stub
  // synthesised by the undefined-label post-pass.
  EXPECT_EQ(out.find("recomp_dispatch(rdram, ctx, 0x80033C28)"),
            std::string::npos)
      << out;
}

TEST(HijackedReturnAddress, OrdinaryIndirectJumpStillReturns) {
  ps1recomp::InstructionEmitter em;
  ps1recomp::RecompFunction f;
  f.name = "plain";
  f.address = kDecompressor;
  f.instructions = hijackedRaFunction(0x80041000u); // $ra points elsewhere
  f.isLabelTarget.assign(f.instructions.size(), false);
  f.size = static_cast<uint32_t>(f.instructions.size() * 4);

  const std::string out = em.emitFunction(f);

  // The plain form, which returns.  Every indirect site also carries its own
  // guest address so the dispatcher can name it when a target does not
  // resolve, hence the _AT suffix and the trailing argument.
  EXPECT_NE(out.find("JUMP_INDIRECT_AT(ctx, ctx->r20, 0x"), std::string::npos)
      << out;
  EXPECT_EQ(out.find("JUMP_INDIRECT_RESUME"), std::string::npos) << out;
  EXPECT_EQ(out.find("ctx->r31 =="), std::string::npos) << out;
}

// An indirect call carries its own guest address too, not just the return
// address it writes to $ra.
TEST(IndirectSite, JalrRecordsTheCallSiteAddress) {
  ps1recomp::InstructionEmitter em;
  ps1recomp::RecompFunction f;
  f.name = "callsite";
  f.address = 0x80010000u;
  // jalr $ra, $t9 ; nop ; jr $ra ; nop
  f.instructions = {0x0320F809u, 0x00000000u, 0x03E00008u, 0x00000000u};
  f.isLabelTarget.assign(f.instructions.size(), false);
  f.size = static_cast<uint32_t>(f.instructions.size() * 4);

  const std::string out = em.emitFunction(f);

  EXPECT_NE(out.find("CALL_INDIRECT_AT(ctx, ctx->r25, 0x80010000u);"),
            std::string::npos)
      << out;
}

// Computed jump through $ra
//
// Ground truth is func_80036340, one of Crash's hand-written polygon
// renderers.  It parks $sp as a data register, then picks one of several
// 128-byte blocks inside itself with
//
//   lui $ra, 0x8003 / addiu $ra, $ra, 0x64D4   ; base 0x800364D4
//   sll $sp, $at, 7 / addu $ra, $ra, $sp       ; + index * 128
//   jr  $ra
//
// Emitting that `jr $ra` as a C++ return left the function mid-body with $sp
// still holding data (0x100): CORE_Loop came back on a stack in low memory,
// and level 0x0A spun forever in an object traversal whose saved registers
// that stack kept overwriting.
namespace {
constexpr uint32_t kRenderer = 0x80036340;
constexpr uint32_t kBlockBase = 0x800364D4;
constexpr size_t kRendererLen = (0x80036BF4u - kRenderer) / 4;

std::vector<uint32_t> raComputedJumpFunction() {
  std::vector<uint32_t> f(kRendererLen, kNop);
  const size_t at = 80;
  f[at + 0] = lui(31, 0x8003);
  f[at + 1] = addiu(31, 31, 0x64D4);
  f[at + 2] = sll(29, 1, 7);
  f[at + 3] = addu(31, 31, 29);
  f[at + 4] = jr(31);
  return f;
}
} // namespace

TEST(RaComputedJump, EveryStridedBlockInsideTheFunctionIsReported) {
  auto targets = detectInternalReturnTargets(raComputedJumpFunction(), kRenderer);
  const uint32_t end = kRenderer + kRendererLen * 4;
  for (uint32_t t = kBlockBase; t < end; t += 128)
    EXPECT_NE(std::find(targets.begin(), targets.end(), t), targets.end())
        << std::hex << t;
}

TEST(RaComputedJump, JrRaJumpsInsideInsteadOfReturning) {
  ps1recomp::InstructionEmitter em;
  ps1recomp::RecompFunction f;
  f.name = "func_80036340";
  f.address = kRenderer;
  f.instructions = raComputedJumpFunction();
  f.isLabelTarget.assign(f.instructions.size(), false);
  f.size = static_cast<uint32_t>(f.instructions.size() * 4);

  const std::string out = em.emitFunction(f);
  EXPECT_NE(out.find("if (ctx->r31 == 0x80036554u) goto L_80036554;"),
            std::string::npos)
      << out;
}
