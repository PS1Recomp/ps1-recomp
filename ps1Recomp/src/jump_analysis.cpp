#include <ps1recomp/jump_analysis.h>

#include <ps1recomp/mips_decoder.h>

#include <algorithm>
#include <array>

namespace ps1recomp {

// The pattern is matched backwards from the `jr`, in the order the compiler
// emits it.  Steps 3 and 4 mirror the memory-table detector in main.cpp: trace
// the base to its LUI, and take the entry count from the SLTIU/SLTI that
// guards the index.
std::vector<uint32_t>
detectComputedCodeJump(const std::vector<uint32_t> &instrs, size_t jr_idx,
                       uint32_t funcAddr) {
  if (jr_idx == 0 || jr_idx >= instrs.size())
    return {};

  const uint32_t funcEnd = funcAddr + static_cast<uint32_t>(instrs.size() * 4);

  Instruction jr_inst = MipsDecoder::decode(instrs[jr_idx]);
  uint8_t target_reg = jr_inst.rs;

  // Step 1: the target register is produced by ADDU/SUBU, not loaded.
  int op_idx = -1;
  bool descending = false;
  uint8_t op_rs = 0, op_rt = 0;
  for (int j = (int)jr_idx - 1; j >= 0 && j >= (int)jr_idx - 8; j--) {
    Instruction inst = MipsDecoder::decode(instrs[j]);
    if ((inst.id == InstrId::ADDU || inst.id == InstrId::SUBU) &&
        inst.rd == target_reg) {
      op_idx = j;
      descending = (inst.id == InstrId::SUBU);
      op_rs = inst.rs;
      op_rt = inst.rt;
      break;
    }
  }
  if (op_idx < 0)
    return {};

  // Step 2: trace a candidate register back to its LUI (+ optional ADDIU).
  uint32_t lui_val = 0;
  int16_t addiu_imm = 0;
  auto traceToLui = [&](uint8_t reg) {
    lui_val = 0;
    addiu_imm = 0;
    for (int j = op_idx - 1; j >= 0 && j >= (int)jr_idx - 20; j--) {
      Instruction inst = MipsDecoder::decode(instrs[j]);
      if (inst.id == InstrId::LUI && inst.rt == reg) {
        lui_val = static_cast<uint32_t>(static_cast<uint16_t>(inst.imm16)) << 16;
        return true;
      }
      if ((inst.id == InstrId::ADDIU || inst.id == InstrId::ADDI) &&
          inst.rt == reg && inst.rs == reg) {
        addiu_imm = inst.imm16;
      }
    }
    return false;
  };

  // SUBU only makes sense as base - scaled, so the base must be the minuend.
  uint8_t scaled_reg = op_rt;
  if (!traceToLui(op_rs)) {
    if (descending)
      return {};
    scaled_reg = op_rs;
    if (!traceToLui(op_rt))
      return {};
  }

  uint32_t base = lui_val + static_cast<uint32_t>(addiu_imm);

  // Step 3: the base has to land inside this function -- that is what makes
  // the targets local labels rather than calls into another function.
  if (base < funcAddr || base >= funcEnd || (base & 3u) != 0)
    return {};

  // Step 4: recover the stride.  It is the index scaled by a constant the
  // compiler built out of shifts and adds, and only the whole chain gives the
  // real figure: `sll $r,$i,2; addu $r,$r,$i; sll $r,$r,1` is x10, while the
  // first shift alone reads as x4 -- half the entries, and the half the game
  // actually jumps to are the missing ones.
  //
  // Every register is tracked as `coef * <whatever some register held>`,
  // seeded with the identity.  Anything the evaluator does not model resets its
  // destination to the identity, which costs nothing: the index arrives in a
  // register that some earlier arithmetic wrote, and only the scaling applied
  // after that matters.
  struct Linear {
    uint8_t base;
    uint32_t coef;
  };
  std::array<Linear, 32> lin{};
  for (uint8_t r = 0; r < 32; ++r)
    lin[r] = {r, 1};

  const int win_start = std::max(0, static_cast<int>(jr_idx) - 28);
  for (int j = win_start; j < op_idx; ++j) {
    Instruction inst = MipsDecoder::decode(instrs[j]);
    int written = MipsDecoder::destGPR(inst);
    if (written <= 0)
      continue; // writes no GPR, or writes $zero
    const auto dst = static_cast<uint8_t>(written);

    Linear val{dst, 1}; // identity unless one of the scaling forms matches
    if (inst.id == InstrId::SLL && inst.shamt < 32) {
      val = {lin[inst.rt].base, lin[inst.rt].coef << inst.shamt};
    } else if (inst.id == InstrId::ADDU || inst.id == InstrId::ADD) {
      const Linear a = lin[inst.rs];
      const Linear b = lin[inst.rt];
      if (inst.rt == 0)
        val = a;
      else if (inst.rs == 0)
        val = b;
      else if (a.base == b.base)
        val = {a.base, a.coef + b.coef};
    }
    lin[dst] = val;
  }

  // Coefficient 1 is the identity: the register carries no scaling at all, so
  // this is not an indexed jump.
  const Linear scale = lin[scaled_reg];
  if (scale.coef < 2 || scale.coef > kJumpTableMaxStride)
    return {};
  const uint32_t stride = scale.coef;
  const uint8_t index_reg = scale.base;

  uint32_t max_entries = kJumpTableFallbackEntries;
  for (int j = op_idx - 1; j >= win_start; j--) {
    Instruction inst = MipsDecoder::decode(instrs[j]);
    if ((inst.id == InstrId::SLTIU || inst.id == InstrId::SLTI) &&
        inst.rs == index_reg) {
      uint32_t bound = static_cast<uint32_t>(static_cast<uint16_t>(inst.imm16));
      if (bound > 0 && bound <= kJumpTableMaxEntries)
        max_entries = bound;
      break;
    }
  }

  // Step 5: enumerate base -/+ i*stride, clipped to the function.  Extra
  // entries are inert -- they only become reachable if the computed value
  // matches.  A stride that is not a multiple of 4 puts some of them off an
  // instruction boundary; those the hardware could never reach, so drop them
  // rather than stopping -- the aligned ones after them are the real entries.
  // Measured 2026-08-30: walking BOTH directions here (an ADDU is modulo 2^32,
  // so a negative index reaches base - n*stride through it) does put
  // 0x80038030 into the third table of Crash's 0x80037D50 -- and the abort
  // there does not change, because the block that actually falls through is
  // the one based at 0x80037D54, whose entries are 4 mod 8 and so can never
  // hold it.  The gap is a base misdetected by +4, not the walk direction.
  // Bidirectional walking also breaks ComputedCodeJump.* (4 tests), which pin
  // the one-directional contract.  Fix the base before touching this.
  std::vector<uint32_t> targets;
  for (uint32_t i = 0; i < max_entries; i++) {
    uint32_t off = i * stride;
    if (descending && off > base - funcAddr)
      break;
    uint32_t target = descending ? base - off : base + off;
    if (target < funcAddr || target >= funcEnd)
      break;
    if ((target & 3u) != 0)
      continue;
    targets.push_back(target);
  }
  return targets;
}

// Only the explicit `lui $ra` + `addiu $ra, $ra` constant counts.  A return
// address that merely happens to land inside the function -- recursion, say --
// is an ordinary call and must keep its ordinary return.
std::vector<uint32_t>
detectInternalReturnTargets(const std::vector<uint32_t> &instrs,
                            uint32_t funcAddr) {
  constexpr uint8_t kRa = 31;
  const uint32_t funcEnd = funcAddr + static_cast<uint32_t>(instrs.size() * 4);

  std::vector<uint32_t> targets;
  auto add = [&](uint32_t addr) {
    if (addr >= funcAddr && addr < funcEnd && (addr & 3u) == 0 &&
        std::find(targets.begin(), targets.end(), addr) == targets.end())
      targets.push_back(addr);
  };
  uint32_t hi = 0;
  bool haveHi = false;
  uint32_t base = 0; // last in-function `$ra` constant, for a scaled add
  bool haveBase = false;
  uint8_t shiftOf[32] = {}; // `sll $rd, $rt, sa` seen last for each $rd
  bool shifted[32] = {};

  for (uint32_t word : instrs) {
    Instruction inst = MipsDecoder::decode(word);

    if (inst.id == InstrId::SLL && inst.rd != 0) {
      shiftOf[inst.rd] = inst.shamt;
      shifted[inst.rd] = true;
      continue;
    }

    if (inst.id == InstrId::LUI && inst.rt == kRa) {
      hi = static_cast<uint32_t>(static_cast<uint16_t>(inst.imm16)) << 16;
      haveHi = true;
      continue;
    }

    if ((inst.id == InstrId::ADDIU || inst.id == InstrId::ADDI) &&
        inst.rt == kRa && inst.rs == kRa) {
      if (!haveHi)
        continue;
      haveHi = false;
      base = hi + static_cast<uint32_t>(inst.imm16);
      haveBase = base >= funcAddr && base < funcEnd;
      add(base);
      continue;
    }

    // `addu $ra, $ra, $rx` with $rx = index << n: a computed jump into a
    // table of 2^n-byte blocks starting at the constant (func_80036340).
    if (inst.id == InstrId::ADDU && inst.rd == kRa && haveBase &&
        (inst.rs == kRa || inst.rt == kRa)) {
      const uint8_t idx = inst.rs == kRa ? inst.rt : inst.rs;
      if (shifted[idx] && shiftOf[idx] >= 2 && shiftOf[idx] < 16)
        for (uint32_t a = base; a < funcEnd; a += 1u << shiftOf[idx])
          add(a);
      haveBase = false;
    }
  }

  return targets;
}

} // namespace ps1recomp
