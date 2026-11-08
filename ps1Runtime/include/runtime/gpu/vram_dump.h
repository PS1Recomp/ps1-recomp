#pragma once

// ps1Runtime -- VRAM dump
// Full 1024x512 VRAM as a P6 PPM, the regression contract tools/smoke_test.py
// reads for game bring-up and the lockstep `shot` command writes.

#include <runtime/gpu/gpu.h>

namespace ps1::gpu {

/// Write VRAM (ABGR1555, 5-bit channels scaled to 8) to `path`; false when the
/// file cannot be opened.
bool writeVramPpm(const GPU &gpu, const char *path);

} // namespace ps1::gpu
