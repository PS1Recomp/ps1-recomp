#include "runtime/save_state.h"
#include <fmt/format.h>

namespace ps1 {

void SaveState::writeBytes(std::ofstream &f, const void *data, size_t size) {
  f.write(reinterpret_cast<const char *>(data), size);
}

bool SaveState::readBytes(std::ifstream &f, void *data, size_t size) {
  f.read(reinterpret_cast<char *>(data), size);
  return f.good() || f.eof();
}

uint32_t SaveState::computeChecksum(const void *data, size_t size) {
  uint32_t checksum = 0;
  const auto *bytes = reinterpret_cast<const uint8_t *>(data);
  for (size_t i = 0; i < size; i++) {
    checksum ^= static_cast<uint32_t>(bytes[i]) << ((i % 4) * 8);
  }
  return checksum;
}

bool SaveState::save(const std::string &path, const recomp_context &ctx,
                     const Memory &mem, const gpu::GPU &gpu,
                     const spu::SPU &spu, const DMA &dma,
                     const cdrom::CdromController & /*cdrom*/,
                     const Timers & /*timers*/, const InterruptController &irq,
                     const input::InputController & /*input*/) {
  std::ofstream f(path, std::ios::binary);
  if (!f) {
    fmt::print(stderr, "[SaveState] Cannot open '{}' for writing\n", path);
    return false;
  }

  // Write header placeholder (we'll update checksum at the end)
  SaveStateHeader header;
  auto headerPos = f.tellp();
  writeBytes(f, &header, sizeof(header));

  // ── CPU Context ──────────────────────────────────
  // GPRs (32 × 4 bytes = 128 bytes)
  writeBytes(f, ctx.r, sizeof(ctx.r));
  // HI, LO, PC
  writeBytes(f, &ctx.hi, sizeof(ctx.hi));
  writeBytes(f, &ctx.lo, sizeof(ctx.lo));
  writeBytes(f, &ctx.pc, sizeof(ctx.pc));
  // COP0 (16 × 4 bytes)
  writeBytes(f, ctx.cop0, sizeof(ctx.cop0));
  // COP2 data + control (64 × 4 bytes)
  writeBytes(f, ctx.cop2d, sizeof(ctx.cop2d));
  writeBytes(f, ctx.cop2c, sizeof(ctx.cop2c));

  // ── RAM (2MB) ────────────────────────────────────
  writeBytes(f, mem.ramPtr(), Memory::RAM_SIZE);

  // ── Scratchpad (1KB) ─────────────────────────────
  writeBytes(f, mem.scratchpadPtr(), Memory::SCRATCHPAD_SIZE);

  // ── GPU VRAM (1024×512×2 = 1MB) ──────────────────
  // Write a placeholder — actual VRAM save requires non-const vramPtr
  std::vector<uint8_t> vramZero(1024 * 512 * 2, 0);
  writeBytes(f, vramZero.data(), vramZero.size());

  // ── GPU State ────────────────────────────────────
  uint32_t gpustat = gpu.readGPUSTAT();
  writeBytes(f, &gpustat, sizeof(gpustat));

  // ── SPU Sound RAM (512KB) ────────────────────────
  writeBytes(f, spu.soundRamPtr(), 512 * 1024);

  // ── SPU Registers snapshot ───────────────────────
  // Save key registers (SPUCNT, master volume, key on/off state)
  uint16_t spuCnt = spu.readRegister(0x1F801DAA);
  writeBytes(f, &spuCnt, sizeof(spuCnt));

  // ── DMA State ────────────────────────────────────
  // Save DPCR and DICR
  uint32_t dpcr = dma.readRegister(0x1F8010F0);
  uint32_t dicr = dma.readRegister(0x1F8010F4);
  writeBytes(f, &dpcr, sizeof(dpcr));
  writeBytes(f, &dicr, sizeof(dicr));
  // Save all 7 channel registers (base, block, chcr)
  for (uint32_t ch = 0; ch < 7; ch++) {
    uint32_t base = dma.readRegister(0x1F801080 + ch * 0x10);
    uint32_t block = dma.readRegister(0x1F801084 + ch * 0x10);
    uint32_t chcr = dma.readRegister(0x1F801088 + ch * 0x10);
    writeBytes(f, &base, sizeof(base));
    writeBytes(f, &block, sizeof(block));
    writeBytes(f, &chcr, sizeof(chcr));
  }

  // ── IRQ State ────────────────────────────────────
  uint32_t istat = irq.readIStat();
  uint32_t imask = irq.readIMask();
  writeBytes(f, &istat, sizeof(istat));
  writeBytes(f, &imask, sizeof(imask));

  // ── Update header with checksum ──────────────────
  // Compute checksum of RAM only (for speed)
  header.checksum = computeChecksum(mem.ramPtr(), Memory::RAM_SIZE);
  f.seekp(headerPos);
  writeBytes(f, &header, sizeof(header));

  f.close();
  fmt::print("[SaveState] Saved to '{}'\n", path);
  return true;
}

bool SaveState::load(const std::string &path, recomp_context &ctx, Memory &mem,
                     gpu::GPU & /*gpu*/, spu::SPU & /*spu*/, DMA &dma,
                     cdrom::CdromController & /*cdrom*/, Timers & /*timers*/,
                     InterruptController &irq,
                     input::InputController & /*input*/) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    fmt::print(stderr, "[SaveState] Cannot open '{}' for reading\n", path);
    return false;
  }

  // Read header
  SaveStateHeader header;
  readBytes(f, &header, sizeof(header));

  if (header.magic[0] != 'P' || header.magic[1] != 'S' ||
      header.magic[2] != '1' || header.magic[3] != 'S') {
    fmt::print(stderr, "[SaveState] Invalid magic in '{}'\n", path);
    return false;
  }
  if (header.version != 1) {
    fmt::print(stderr, "[SaveState] Unsupported version {} in '{}'\n",
               header.version, path);
    return false;
  }

  // ── CPU Context ──────────────────────────────────
  readBytes(f, ctx.r, sizeof(ctx.r));
  readBytes(f, &ctx.hi, sizeof(ctx.hi));
  readBytes(f, &ctx.lo, sizeof(ctx.lo));
  readBytes(f, &ctx.pc, sizeof(ctx.pc));
  readBytes(f, ctx.cop0, sizeof(ctx.cop0));
  readBytes(f, ctx.cop2d, sizeof(ctx.cop2d));
  readBytes(f, ctx.cop2c, sizeof(ctx.cop2c));

  // ── RAM (2MB) ────────────────────────────────────
  readBytes(f, mem.ramPtr(), Memory::RAM_SIZE);

  // ── Scratchpad (1KB) ─────────────────────────────
  readBytes(f, mem.scratchpadPtr(), Memory::SCRATCHPAD_SIZE);

  // ── GPU VRAM (skip for now — need mutable vramPtr) ──
  // We'll read it but need a way to write back to GPU
  std::vector<uint8_t> vramData(1024 * 512 * 2);
  readBytes(f, vramData.data(), vramData.size());
  // TODO: gpu.loadVram(vramData.data()) when available

  // ── GPU State ────────────────────────────────────
  uint32_t gpustat;
  readBytes(f, &gpustat, sizeof(gpustat));

  // ── SPU Sound RAM (512KB) ────────────────────────
  // Read into SPU's sound RAM
  std::vector<uint8_t> spuRam(512 * 1024);
  readBytes(f, spuRam.data(), spuRam.size());
  // TODO: spu.loadSoundRam(spuRam.data()) when available

  // ── SPU Registers ────────────────────────────────
  uint16_t spuCnt;
  readBytes(f, &spuCnt, sizeof(spuCnt));

  // ── DMA State ────────────────────────────────────
  uint32_t dpcr, dicr;
  readBytes(f, &dpcr, sizeof(dpcr));
  readBytes(f, &dicr, sizeof(dicr));
  dma.writeRegister(0x1F8010F0, dpcr);
  dma.writeRegister(0x1F8010F4, dicr);
  for (uint32_t ch = 0; ch < 7; ch++) {
    uint32_t base, block, chcr;
    readBytes(f, &base, sizeof(base));
    readBytes(f, &block, sizeof(block));
    readBytes(f, &chcr, sizeof(chcr));
    dma.writeRegister(0x1F801080 + ch * 0x10, base);
    dma.writeRegister(0x1F801084 + ch * 0x10, block);
    dma.writeRegister(0x1F801088 + ch * 0x10, chcr);
  }

  // ── IRQ State ────────────────────────────────────
  uint32_t istat, imask;
  readBytes(f, &istat, sizeof(istat));
  readBytes(f, &imask, sizeof(imask));
  irq.writeIMask(imask);
  // Restore I_STAT by raising all bits that were set
  // First clear, then raise
  irq.writeIStat(0);
  for (uint32_t bit = 0; bit < 11; bit++) {
    if (istat & (1u << bit))
      irq.raiseInterrupt(1u << bit);
  }

  // Verify checksum
  uint32_t checksum = computeChecksum(mem.ramPtr(), Memory::RAM_SIZE);
  if (checksum != header.checksum) {
    fmt::print(stderr,
               "[SaveState] WARNING: Checksum mismatch (got 0x{:08X}, "
               "expected 0x{:08X})\n",
               checksum, header.checksum);
  }

  f.close();
  fmt::print("[SaveState] Loaded from '{}'\n", path);
  return true;
}

bool SaveState::isValid(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f)
    return false;

  SaveStateHeader header;
  f.read(reinterpret_cast<char *>(&header), sizeof(header));
  if (!f.good())
    return false;

  return header.magic[0] == 'P' && header.magic[1] == 'S' &&
         header.magic[2] == '1' && header.magic[3] == 'S' &&
         header.version == 1;
}

} // namespace ps1
