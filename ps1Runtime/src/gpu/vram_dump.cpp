#include <runtime/gpu/vram_dump.h>

#include <cstdio>
#include <fmt/format.h>
#include <runtime/metrics.h>
#include <vector>

namespace ps1::gpu {

bool writeVramPpm(const GPU &gpu, const char *path) {
  std::FILE *f = std::fopen(path, "wb");
  if (!f) {
    fmt::print(stderr, "[VRAM-DUMP] open failed: {}\n", path);
    return false;
  }
  constexpr uint32_t W = GPU::VRAM_WIDTH;
  constexpr uint32_t H = GPU::VRAM_HEIGHT;
  std::fprintf(f, "P6\n%u %u\n255\n", W, H);
  const Color16 *vram = gpu.getVRAM();
  std::vector<uint8_t> row(W * 3);
  uint64_t nonZero = 0;
  for (uint32_t y = 0; y < H; ++y) {
    for (uint32_t x = 0; x < W; ++x) {
      uint16_t p = vram[y * W + x].raw;
      row[x * 3 + 0] = static_cast<uint8_t>((p & 0x1F) << 3);
      row[x * 3 + 1] = static_cast<uint8_t>(((p >> 5) & 0x1F) << 3);
      row[x * 3 + 2] = static_cast<uint8_t>(((p >> 10) & 0x1F) << 3);
      if (p != 0)
        ++nonZero;
    }
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
  metrics::count("vram_dump.written");
  metrics::setState("vram_dump.nonzero_pixels", static_cast<int64_t>(nonZero));
  fmt::print("[VRAM-DUMP] wrote {} ({}x{}), {} non-zero pixels\n", path, W, H,
             nonZero);
  return true;
}

} // namespace ps1::gpu
