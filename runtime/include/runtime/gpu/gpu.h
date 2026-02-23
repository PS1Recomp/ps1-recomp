#pragma once

#include <cstdint>
#include <deque>
#include <vector>

namespace ps1::gpu {

// A 15-bit color with 1 bit for transparency (ABGR1555)
struct Color16 {
  uint16_t raw;
};

// Represents a 2D point/vertex
struct Vertex {
  int32_t x;
  int32_t y;
};

// Represents RGB color
struct Color24 {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

// Texture coordinates
struct TexCoord {
  uint8_t u;
  uint8_t v;
};

class GPU {
public:
  GPU();
  ~GPU();

  // Reset the GPU state
  void reset();

  // Handle a GP0 (Rendering & VRAM) command
  void writeGP0(uint32_t val);

  // Handle a GP1 (Display Control) command
  void writeGP1(uint32_t val);

  // Process a DMA linked list (Ordering Table) from main RAM
  void processLinkedList(uint32_t startAddr, const uint8_t *ram);

  // Read GPU response (GPUREAD)
  uint32_t readGPUREAD();

  // Read GPU Status Register (GPUSTAT)
  uint32_t readGPUSTAT() const;

  // Get a pointer to the 1024x512 VRAM framebuffer
  const Color16 *getVRAM() const { return vram_.data(); }

  // Dimensions of VRAM
  static constexpr uint32_t VRAM_WIDTH = 1024;
  static constexpr uint32_t VRAM_HEIGHT = 512;

private:
  // VRAM buffer
  std::vector<Color16> vram_;

  // GPU status registers
  uint32_t gpuStat_;
  uint32_t gpuRead_;

  // Command buffering
  std::deque<uint32_t> commandQueue_;
  uint32_t expectedCommandWords_;

  // Internal states
  bool isCommandExecuting_;

  // Drawing attributes
  int32_t drawOffsetX_;
  int32_t drawOffsetY_;
  int32_t drawAreaX1_;
  int32_t drawAreaY1_;
  int32_t drawAreaX2_;
  int32_t drawAreaY2_;

  // Display attributes
  uint32_t displayVRAMXStart_;
  uint32_t displayVRAMYStart_;
  uint32_t displayX1_, displayX2_;
  uint32_t displayY1_, displayY2_;

  // VRAM copy parameters
  struct {
    uint32_t srcX, srcY;
    uint32_t destX, destY;
    uint32_t currX, currY;
    uint32_t width, height;
    uint32_t transferWordsRemaining;
    bool isWritingToVRAM;
    bool isReadingFromVRAM;
    bool isCopyingVRAM;
  } vramTransfer_;

  // Internal decoding helpers
  void executeGP0Command();
  void executeGP1Command(uint32_t val);

  // GP0 specific commands
  void executeClearCache();
  void executeFillRect();
  void executeCopyVRAM();
  void executeCPUToVRAM();
  void executeVRAMToCPU();

  void executeMonochromePoly3();
  void executeMonochromePoly4();

  void executeTexturedPoly3();
  void executeTexturedPoly4();

  void executeLine();
  void executeRect();

  // Rasterize a solid monochrome triangle
  void rasterizeTriangle(Vertex v0, Vertex v1, Vertex v2, Color16 c);

  // Rasterize a textured triangle
  void rasterizeTexturedTriangle(Vertex v0, Vertex v1, Vertex v2, TexCoord t0,
                                 TexCoord t1, TexCoord t2, Color16 color,
                                 uint16_t clut, uint16_t tpage);

  // Helper methods for rasterization (to be implemented)
  void drawSolidPoly(bool quad);
  void drawTexturedPoly(bool quad, bool blend, bool raw);
};

} // namespace ps1::gpu
