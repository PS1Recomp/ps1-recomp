#include "runtime/gpu/gpu.h"
#include <algorithm>
#include <cstdint>
#include <fmt/format.h>
#include <initializer_list>
#include <utility>

namespace ps1::gpu {

GPU::GPU() {
  vram_.resize(VRAM_WIDTH * VRAM_HEIGHT);
  reset();
}

GPU::~GPU() {}

void GPU::reset() {
  gpuStat_ = 0x14802000;
  gpuRead_ = 0;
  expectedCommandWords_ = 0;
  isCommandExecuting_ = false;
  commandQueue_.clear();

  drawOffsetX_ = 0;
  drawOffsetY_ = 0;
  drawAreaX1_ = 0;
  drawAreaY1_ = 0;
  drawAreaX2_ = 1023;
  drawAreaY2_ = 511;
  ditherEnable_ = false;

  displayVRAMXStart_ = 0;
  displayVRAMYStart_ = 0;
  displayX1_ = 0;
  displayX2_ = 256;
  displayY1_ = 0;
  displayY2_ = 240;

  vramTransfer_.transferWordsRemaining = 0;
  vramTransfer_.isWritingToVRAM = false;
  vramTransfer_.isReadingFromVRAM = false;
  vramTransfer_.isCopyingVRAM = false;

  std::fill(vram_.begin(), vram_.end(), Color16{0});
}

uint32_t GPU::readGPUSTAT() const { return gpuStat_; }

uint32_t GPU::readGPUREAD() {
  if (vramTransfer_.isReadingFromVRAM) {
    auto takePixel = [&]() -> uint16_t {
      uint32_t x = vramTransfer_.currX;
      uint32_t y = vramTransfer_.currY;
      uint16_t p = vram_[y * VRAM_WIDTH + x].raw;

      vramTransfer_.currX++;
      if (vramTransfer_.currX >= vramTransfer_.srcX + vramTransfer_.width) {
        vramTransfer_.currX = vramTransfer_.srcX;
        vramTransfer_.currY++;
      }
      return p;
    };

    uint16_t p1 = takePixel();
    uint16_t p2 = takePixel();

    vramTransfer_.transferWordsRemaining--;
    if (vramTransfer_.transferWordsRemaining == 0) {
      vramTransfer_.isReadingFromVRAM = false;
    }

    return p1 | (p2 << 16);
  }
  return gpuRead_;
}

void GPU::writeGP0(uint32_t val) {
  if (vramTransfer_.isWritingToVRAM) {
    uint16_t p1 = val & 0xFFFF;
    uint16_t p2 = (val >> 16) & 0xFFFF;

    auto putPixel = [&](uint16_t p) {
      uint32_t x = vramTransfer_.currX;
      uint32_t y = vramTransfer_.currY;
      vram_[y * VRAM_WIDTH + x] = Color16{p};
      vramTransfer_.currX++;
      if (vramTransfer_.currX >= vramTransfer_.destX + vramTransfer_.width) {
        vramTransfer_.currX = vramTransfer_.destX;
        vramTransfer_.currY++;
      }
    };

    putPixel(p1);
    putPixel(p2);

    vramTransfer_.transferWordsRemaining--;
    if (vramTransfer_.transferWordsRemaining == 0) {
      vramTransfer_.isWritingToVRAM = false;
    }
    return;
  }

  // Push word to command queue
  commandQueue_.push_back(val);

  if (!isCommandExecuting_) {
    // Start decoding a new command
    uint32_t opcode = val >> 24;
    switch (opcode) {
    case 0x00:
      expectedCommandWords_ = 1;
      break; // NOP
    case 0x01:
      expectedCommandWords_ = 1;
      break; // Clear Cache
    case 0x02:
      expectedCommandWords_ = 3;
      break; // Fill Rectangle in VRAM
    case 0x20:
      expectedCommandWords_ = 4;
      break; // Monochrome 3-point polygon
    case 0x24:
      expectedCommandWords_ = 7;
      break; // Textured 3-point polygon
    case 0x28:
      expectedCommandWords_ = 5;
      break; // Monochrome 4-point polygon
    case 0x2C:
      expectedCommandWords_ = 9;
      break; // Textured 4-point polygon
    case 0x30:
    case 0x32:
      expectedCommandWords_ = 6;
      break; // Gouraud 3-point polygon
    case 0x34:
    case 0x36:
      expectedCommandWords_ = 9;
      break; // Gouraud Textured 3-point polygon
    case 0x38:
    case 0x3A:
      expectedCommandWords_ = 8;
      break; // Gouraud 4-point polygon
    case 0x3C:
    case 0x3E:
      expectedCommandWords_ = 12;
      break; // Gouraud Textured 4-point polygon
    case 0x40:
      expectedCommandWords_ = 3;
      break; // Monochrome Line
    case 0x48:
    case 0x4A:
    case 0x4C:
    case 0x4E:
      expectedCommandWords_ =
          3; // Poly-line is variable, 3 is just for the first segment
      break;
    case 0x50:
      expectedCommandWords_ = 4;
      break; // Gouraud Line
    case 0x60:
      expectedCommandWords_ = 3;
      break; // Variable Rect
    case 0x64:
      expectedCommandWords_ = 4;
      break; // Variable Tex Rect
    case 0x68:
      expectedCommandWords_ = 2;
      break; // 1x1 Rect
    case 0x70:
      expectedCommandWords_ = 2;
      break; // 8x8 Rect
    case 0x74:
      expectedCommandWords_ = 3;
      break; // 8x8 Tex Rect
    case 0x78:
      expectedCommandWords_ = 2;
      break; // 16x16 Rect
    case 0x7C:
      expectedCommandWords_ = 3;
      break; // 16x16 Tex Rect
    case 0xA0:
      expectedCommandWords_ = 3;
      break; // Copy Rectangle (CPU to VRAM)
    case 0xC0:
      expectedCommandWords_ = 3;
      break; // Copy Rectangle (VRAM to CPU)
    case 0xE1:
      expectedCommandWords_ = 1;
      break; // Draw Mode setting
    case 0xE2:
      expectedCommandWords_ = 1;
      break; // Texture Window setting
    case 0xE3:
      expectedCommandWords_ = 1;
      break; // Set Drawing Area top left
    case 0xE4:
      expectedCommandWords_ = 1;
      break; // Set Drawing Area bottom right
    case 0xE5:
      expectedCommandWords_ = 1;
      break; // Set Drawing Offset
    case 0xE6:
      expectedCommandWords_ = 1;
      break; // Mask Bit setting
    default:
      // Unknown or unimplemented command
      fmt::print("[GPU] WARNING: Unknown GP0 opcode 0x{:02X}\n", opcode);
      expectedCommandWords_ = 1;
      break;
    }
    isCommandExecuting_ = true;
  }

  if (commandQueue_.size() >= expectedCommandWords_) {
    executeGP0Command();
    commandQueue_.clear();
    isCommandExecuting_ = false;
  }
}

void GPU::writeGP1(uint32_t val) {
  uint32_t opcode = val >> 24;
  switch (opcode) {
  case 0x00: // Reset GPU
    reset();
    break;
  case 0x01: // Reset Command Buffer
    commandQueue_.clear();
    isCommandExecuting_ = false;
    vramTransfer_.isWritingToVRAM = false;
    vramTransfer_.isReadingFromVRAM = false;
    break;
  case 0x03: // Display Enable
    // Bit 0: 0=On, 1=Off. Updates GPUSTAT bit 23.
    gpuStat_ = (gpuStat_ & ~(1 << 23)) | ((val & 1) << 23);
    break;
  case 0x04: // DMA Direction
    // Update GPUSTAT bits 29-30
    gpuStat_ = (gpuStat_ & ~(3 << 29)) | ((val & 3) << 29);
    break;
  case 0x05: // Start of Display Area (in VRAM)
    displayVRAMXStart_ = val & 0x3FF;
    displayVRAMYStart_ = (val >> 10) & 0x1FF;
    break;
  case 0x06: // Horizontal Display Range
    displayX1_ = val & 0xFFF;
    displayX2_ = (val >> 12) & 0xFFF;
    break;
  case 0x07: // Vertical Display Range
    displayY1_ = val & 0x3FF;
    displayY2_ = (val >> 10) & 0x3FF;
    break;
  case 0x08: // Display Mode
    // Bit 0-1 Horizontal Resolution 1 + 2
    // Bit 2   Vertical Resolution
    // Bit 3   Video Mode (NTSC/PAL)
    // Bit 4   Display Area Color Depth (0=15bit, 1=24bit)
    // Bit 5   Vertical Interlace
    // Bit 6   Horizontal Resolution 2
    // Bit 7   Reverseflag (0=Normal, 1=Distorted)
    // We update GPUSTAT bits 17-19 (Hres), 20 (Vres), etc.
    gpuStat_ = (gpuStat_ & ~0x7F0000) | ((val & 0x7F) << 17);
    break;
  default:
    fmt::print("[GPU] WARNING: Unknown GP1 opcode 0x{:02X}\n", opcode);
    break;
  }
}

void GPU::processLinkedList(uint32_t startAddr, const uint8_t *ram) {
  uint32_t currentAddr = startAddr & 0x1FFFFC;
  uint32_t maxNodes = 0x10000; // safety net to prevent infinite loops

  while (currentAddr != 0x00FFFFFF && currentAddr != 0xFFFFFF &&
         maxNodes-- > 0) {
    uint32_t header =
        (ram[currentAddr] | (ram[currentAddr + 1] << 8) |
         (ram[currentAddr + 2] << 16) | (ram[currentAddr + 3] << 24));

    uint32_t nextAddr = header & 0xFFFFFF;
    uint32_t numWords = header >> 24;

    for (uint32_t i = 1; i <= numWords; ++i) {
      uint32_t wordAddr = (currentAddr + i * 4) & 0x1FFFFC;
      uint32_t word = (ram[wordAddr] | (ram[wordAddr + 1] << 8) |
                       (ram[wordAddr + 2] << 16) | (ram[wordAddr + 3] << 24));
      writeGP0(word);
    }

    if (nextAddr == 0xFFFFFF || nextAddr == 0x00FFFFFF) {
      break;
    }
    currentAddr = nextAddr & 0x1FFFFC;
  }
}

void GPU::executeGP0Command() {
  uint32_t cmd = commandQueue_.front();
  uint32_t opcode = cmd >> 24;

  switch (opcode) {
  case 0x00: // NOP
    break;
  case 0x01: // Clear Cache
    executeClearCache();
    break;
  case 0x02: // Fill Rect
    executeFillRect();
    break;
  case 0x20: // Monochrome 3-point polygon
    executeMonochromePoly3();
    break;
  case 0x24: // Textured 3-point polygon
    executeTexturedPoly3();
    break;
  case 0x28: // Monochrome 4-point polygon
    executeMonochromePoly4();
    break;
  case 0x2C: // Textured 4-point polygon
    executeTexturedPoly4();
    break;
  case 0x30: // Gouraud 3-point polygon
  case 0x32: // Gouraud Blended 3-point polygon
    executeGouraudPoly3();
    break;
  case 0x34: // Gouraud Textured 3-point polygon
  case 0x36:
    executeGouraudTexturedPoly3();
    break;
  case 0x38: // Gouraud 4-point polygon
  case 0x3A: // Gouraud Blended 4-point polygon
    executeGouraudPoly4();
    break;
  case 0x3C: // Gouraud Textured 4-point polygon
  case 0x3E:
    executeGouraudTexturedPoly4();
    break;
  case 0x40: // Monochrome Line
  case 0x48: // Mono Poly Line
  case 0x50: // Gouraud Line
  case 0x58: // Gouraud Poly Line
    executeLine();
    break;
  case 0x60: // Variable Rect
  case 0x64: // Variable Tex Rect
  case 0x68: // 1x1 Rect
  case 0x70: // 8x8 Rect
  case 0x74: // 8x8 Tex Rect
  case 0x78: // 16x16 Rect
  case 0x7C: // 16x16 Tex Rect
    executeRect();
    break;
  case 0x80: // VRAM to VRAM
    executeCopyVRAM();
    break;
  case 0xA0: // CPU to VRAM
    executeCPUToVRAM();
    break;
  case 0xC0: // VRAM to CPU
    executeVRAMToCPU();
    break;
  case 0xE2: // Set Texture Window
    executeTextureWindow();
    break;
  case 0xE3: // Set Drawing Area top left
    drawAreaX1_ = cmd & 0x3FF;
    drawAreaY1_ = (cmd >> 10) & 0x3FF;
    break;
  case 0xE4: // Set Drawing Area bottom right
    drawAreaX2_ = cmd & 0x3FF;
    drawAreaY2_ = (cmd >> 10) & 0x3FF;
    break;
  case 0xE5: // Set Drawing Offset
    drawOffsetX_ = cmd & 0x7FF;
    if (drawOffsetX_ & 0x400)
      drawOffsetX_ |= 0xFFFFF800; // Sign extend 11-bit
    drawOffsetY_ = (cmd >> 11) & 0x7FF;
    if (drawOffsetY_ & 0x400)
      drawOffsetY_ |= 0xFFFFF800; // Sign extend 11-bit
    break;
  case 0xE1: // Draw Mode parameter (Dither flag is bit 9, Blend Mode is bits
             // 5-6)
    ditherEnable_ = (cmd & (1 << 9)) != 0;
    blendMode_ = (cmd >> 5) & 3;
    break;
  case 0xE6: // Mask Bit
    // Bit 0 = Mask while drawing, Bit 1 = Set Mask bit on draw
    break;
  default:
    // Some commands like E1, E6 are unimplemented but don't crash
    break;
  }
}

void GPU::executeTextureWindow() {
  uint32_t cmd = commandQueue_.front();
  texWindowMaskX_ = (cmd & 0x1F) * 8;
  texWindowMaskY_ = ((cmd >> 5) & 0x1F) * 8;
  texWindowOffsetX_ = ((cmd >> 10) & 0x1F) * 8;
  texWindowOffsetY_ = ((cmd >> 15) & 0x1F) * 8;
}

void GPU::executeMonochromePoly3() {
  uint32_t c = commandQueue_[0] & 0xFFFFFF;
  Color16 c16;
  c16.raw = ((c & 0xFF) >> 3) | ((((c >> 8) & 0xFF) >> 3) << 5) |
            ((((c >> 16) & 0xFF) >> 3) << 10);

  Vertex v[3];
  for (int i = 0; i < 3; i++) {
    int16_t x = commandQueue_[1 + i] & 0xFFFF;
    int16_t y = commandQueue_[1 + i] >> 16;
    v[i].x = x + drawOffsetX_;
    v[i].y = y + drawOffsetY_;
  }
  uint32_t opcode = commandQueue_[0] >> 24;
  bool isBlend = (opcode & 2) != 0;

  rasterizeTriangle(v[0], v[1], v[2], c16, isBlend);
}

void GPU::executeMonochromePoly4() {
  uint32_t c = commandQueue_[0] & 0xFFFFFF;
  Color16 c16;
  c16.raw = ((c & 0xFF) >> 3) | ((((c >> 8) & 0xFF) >> 3) << 5) |
            ((((c >> 16) & 0xFF) >> 3) << 10);

  Vertex v[4];
  for (int i = 0; i < 4; i++) {
    int16_t x = commandQueue_[1 + i] & 0xFFFF;
    int16_t y = commandQueue_[1 + i] >> 16;
    v[i].x = x + drawOffsetX_;
    v[i].y = y + drawOffsetY_;
  }

  uint32_t opcode = commandQueue_[0] >> 24;
  bool isBlend = (opcode & 2) != 0;

  // Draw as two triangles: (0, 1, 2) and (1, 2, 3) because PS1 quadrilaterals
  // are usually ordered like Z
  rasterizeTriangle(v[0], v[1], v[2], c16, isBlend);
  rasterizeTriangle(v[1], v[2], v[3], c16, isBlend);
}

void GPU::executeTexturedPoly3() {
  uint32_t c = commandQueue_[0] & 0xFFFFFF;
  Color16 c16;
  c16.raw = ((c & 0xFF) >> 3) | ((((c >> 8) & 0xFF) >> 3) << 5) |
            ((((c >> 16) & 0xFF) >> 3) << 10);

  Vertex v[3];
  TexCoord t[3];
  uint16_t clut, tpage;

  v[0].x = (int16_t)(commandQueue_[1] & 0xFFFF) + drawOffsetX_;
  v[0].y = (int16_t)(commandQueue_[1] >> 16) + drawOffsetY_;
  t[0].u = commandQueue_[2] & 0xFF;
  t[0].v = (commandQueue_[2] >> 8) & 0xFF;
  clut = (commandQueue_[2] >> 16) & 0xFFFF;

  v[1].x = (int16_t)(commandQueue_[3] & 0xFFFF) + drawOffsetX_;
  v[1].y = (int16_t)(commandQueue_[3] >> 16) + drawOffsetY_;
  t[1].u = commandQueue_[4] & 0xFF;
  t[1].v = (commandQueue_[4] >> 8) & 0xFF;
  tpage = (commandQueue_[4] >> 16) & 0xFFFF;

  v[2].x = (int16_t)(commandQueue_[5] & 0xFFFF) + drawOffsetX_;
  v[2].y = (int16_t)(commandQueue_[5] >> 16) + drawOffsetY_;
  t[2].u = commandQueue_[6] & 0xFF;
  t[2].v = (commandQueue_[6] >> 8) & 0xFF;

  uint32_t opcode = commandQueue_[0] >> 24;
  bool isRaw = (opcode & 1) != 0;
  bool isBlend = (opcode & 2) != 0;

  rasterizeTexturedTriangle(v[0], v[1], v[2], t[0], t[1], t[2], c16, clut,
                            tpage, isRaw, isBlend);
}

void GPU::executeTexturedPoly4() {
  uint32_t c = commandQueue_[0] & 0xFFFFFF;
  Color16 c16;
  c16.raw = ((c & 0xFF) >> 3) | ((((c >> 8) & 0xFF) >> 3) << 5) |
            ((((c >> 16) & 0xFF) >> 3) << 10);

  Vertex v[4];
  TexCoord t[4];
  uint16_t clut, tpage;

  v[0].x = (int16_t)(commandQueue_[1] & 0xFFFF) + drawOffsetX_;
  v[0].y = (int16_t)(commandQueue_[1] >> 16) + drawOffsetY_;
  t[0].u = commandQueue_[2] & 0xFF;
  t[0].v = (commandQueue_[2] >> 8) & 0xFF;
  clut = (commandQueue_[2] >> 16) & 0xFFFF;

  v[1].x = (int16_t)(commandQueue_[3] & 0xFFFF) + drawOffsetX_;
  v[1].y = (int16_t)(commandQueue_[3] >> 16) + drawOffsetY_;
  t[1].u = commandQueue_[4] & 0xFF;
  t[1].v = (commandQueue_[4] >> 8) & 0xFF;
  tpage = (commandQueue_[4] >> 16) & 0xFFFF;

  v[2].x = (int16_t)(commandQueue_[5] & 0xFFFF) + drawOffsetX_;
  v[2].y = (int16_t)(commandQueue_[5] >> 16) + drawOffsetY_;
  t[2].u = commandQueue_[6] & 0xFF;
  t[2].v = (commandQueue_[6] >> 8) & 0xFF;

  v[3].x = (int16_t)(commandQueue_[7] & 0xFFFF) + drawOffsetX_;
  v[3].y = (int16_t)(commandQueue_[7] >> 16) + drawOffsetY_;
  t[3].u = commandQueue_[8] & 0xFF;
  t[3].v = (commandQueue_[8] >> 8) & 0xFF;

  uint32_t opcode = commandQueue_[0] >> 24;
  bool isRaw = (opcode & 1) != 0;
  bool isBlend = (opcode & 2) != 0;

  rasterizeTexturedTriangle(v[0], v[1], v[2], t[0], t[1], t[2], c16, clut,
                            tpage, isRaw, isBlend);
  rasterizeTexturedTriangle(v[1], v[2], v[3], t[1], t[2], t[3], c16, clut,
                            tpage, isRaw, isBlend);
}

static int edgeFunction(const Vertex &a, const Vertex &b, const Vertex &c) {
  return (c.x - a.x) * (b.y - a.y) - (c.y - a.y) * (b.x - a.x);
}

Color16 GPU::applyDither(Color16 baseColor, int x, int y) {
  if (!ditherEnable_) {
    return baseColor;
  }

  // PS1 Dither Matrix 4x4
  static const int8_t ditherMatrix[4][4] = {
      {-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};

  int offset = ditherMatrix[y & 3][x & 3];

  // Apply to 24-bit components extracted from 15-bit color
  int r = ((baseColor.raw & 0x1F) << 3) + offset;
  int g = (((baseColor.raw >> 5) & 0x1F) << 3) + offset;
  int b = (((baseColor.raw >> 10) & 0x1F) << 3) + offset;

  // Clamp 0..255
  if (r < 0)
    r = 0;
  else if (r > 255)
    r = 255;
  if (g < 0)
    g = 0;
  else if (g > 255)
    g = 255;
  if (b < 0)
    b = 0;
  else if (b > 255)
    b = 255;

  Color16 c;
  c.raw = ((r >> 3) & 0x1F) | (((g >> 3) & 0x1F) << 5) |
          (((b >> 3) & 0x1F) << 10) | (baseColor.raw & 0x8000);
  return c;
}

Color16 GPU::applyBlend(Color16 fg, Color16 bg) {
  int rF = (fg.raw & 0x1F) << 3;
  int gF = ((fg.raw >> 5) & 0x1F) << 3;
  int bF = ((fg.raw >> 10) & 0x1F) << 3;

  int rB = (bg.raw & 0x1F) << 3;
  int gB = ((bg.raw >> 5) & 0x1F) << 3;
  int bB = ((bg.raw >> 10) & 0x1F) << 3;

  int rOut = rF, gOut = gF, bOut = bF;

  switch (blendMode_) {
  case 0: // 0.5 * B + 0.5 * F
    rOut = (rB + rF) / 2;
    gOut = (gB + gF) / 2;
    bOut = (bB + bF) / 2;
    break;
  case 1: // 1.0 * B + 1.0 * F
    rOut = rB + rF;
    gOut = gB + gF;
    bOut = bB + bF;
    break;
  case 2: // 1.0 * B - 1.0 * F
    rOut = rB - rF;
    gOut = gB - gF;
    bOut = bB - bF;
    break;
  case 3: // 1.0 * B + 0.25 * F
    rOut = rB + (rF / 4);
    gOut = gB + (gF / 4);
    bOut = bB + (bF / 4);
    break;
  }

  if (rOut < 0)
    rOut = 0;
  else if (rOut > 255)
    rOut = 255;
  if (gOut < 0)
    gOut = 0;
  else if (gOut > 255)
    gOut = 255;
  if (bOut < 0)
    bOut = 0;
  else if (bOut > 255)
    bOut = 255;

  Color16 c;
  c.raw = ((rOut >> 3) & 0x1F) | (((gOut >> 3) & 0x1F) << 5) |
          (((bOut >> 3) & 0x1F) << 10) | (bg.raw & 0x8000);
  return c;
}

void GPU::rasterizeTriangle(Vertex v0, Vertex v1, Vertex v2, Color16 color,
                            bool blend) {
  // Check winding and swap if necessary so we have CCW
  int area = edgeFunction(v0, v1, v2);
  if (area == 0)
    return; // Degenerate
  if (area < 0) {
    std::swap(v1, v2); // Make CCW
  }

  // Bounding box
  int minX = std::min({v0.x, v1.x, v2.x});
  int minY = std::min({v0.y, v1.y, v2.y});
  int maxX = std::max({v0.x, v1.x, v2.x});
  int maxY = std::max({v0.y, v1.y, v2.y});

  // Clip against draw area (DrawAreaX1, Y1 / X2, Y2)
  minX = std::max(minX, drawAreaX1_);
  minY = std::max(minY, drawAreaY1_);
  maxX = std::min(maxX, drawAreaX2_);
  maxY = std::min(maxY, drawAreaY2_);

  // Rasterize
  Vertex p;
  for (p.y = minY; p.y <= maxY; ++p.y) {
    for (p.x = minX; p.x <= maxX; ++p.x) {
      int w0 = edgeFunction(v1, v2, p);
      int w1 = edgeFunction(v2, v0, p);
      int w2 = edgeFunction(v0, v1, p);

      if (w0 >= 0 && w1 >= 0 && w2 >= 0) {
        // Draw pixel
        Color16 finalColor = applyDither(color, p.x, p.y);

        uint32_t idx = (p.y % VRAM_HEIGHT) * VRAM_WIDTH + (p.x % VRAM_WIDTH);
        if (blend) {
          Color16 bg = vram_[idx];
          finalColor = applyBlend(finalColor, bg);
        }

        vram_[idx] = finalColor;
      }
    }
  }
}

void GPU::rasterizeTexturedTriangle(Vertex v0, Vertex v1, Vertex v2,
                                    TexCoord t0, TexCoord t1, TexCoord t2,
                                    Color16 color, uint16_t clut,
                                    uint16_t tpage, bool isRaw, bool blend) {
  int area = edgeFunction(v0, v1, v2);
  if (area == 0)
    return;
  if (area < 0) {
    std::swap(v1, v2);
    std::swap(t1, t2);
    area = -area;
  }

  int minX = std::max({drawAreaX1_, std::min({v0.x, v1.x, v2.x})});
  int minY = std::max({drawAreaY1_, std::min({v0.y, v1.y, v2.y})});
  int maxX = std::min({drawAreaX2_, std::max({v0.x, v1.x, v2.x})});
  int maxY = std::min({drawAreaY2_, std::max({v0.y, v1.y, v2.y})});

  uint32_t tpX = (tpage & 0xF) * 64;
  uint32_t tpY = ((tpage >> 4) & 1) * 256;
  uint32_t depth = (tpage >> 7) & 3;

  uint32_t clutX = (clut & 0x3F) * 16;
  uint32_t clutY = (clut >> 6) & 0x1FF;

  for (int y = minY; y <= maxY; ++y) {
    for (int x = minX; x <= maxX; ++x) {
      Vertex p{x, y};
      int w0 = edgeFunction(v1, v2, p);
      int w1 = edgeFunction(v2, v0, p);
      int w2 = edgeFunction(v0, v1, p);

      if (w0 >= 0 && w1 >= 0 && w2 >= 0) {
        int u = (w0 * t0.u + w1 * t1.u + w2 * t2.u) / area;
        int v = (w0 * t0.v + w1 * t1.v + w2 * t2.v) / area;

        // Apply Texture Window (E2) masking and offsets
        if (texWindowMaskX_ != 0 || texWindowOffsetX_ != 0) {
          u = (u & ~(texWindowMaskX_)) | (texWindowOffsetX_ & texWindowMaskX_);
        }
        if (texWindowMaskY_ != 0 || texWindowOffsetY_ != 0) {
          v = (v & ~(texWindowMaskY_)) | (texWindowOffsetY_ & texWindowMaskY_);
        }

        Color16 texColor;
        if (depth == 0) { // 4-bit
          uint32_t tx = tpX + (u / 4);
          uint32_t ty = tpY + v;
          uint16_t block =
              vram_[(ty % VRAM_HEIGHT) * VRAM_WIDTH + (tx % VRAM_WIDTH)].raw;
          uint8_t index = (block >> ((u % 4) * 4)) & 0xF;
          texColor = vram_[(clutY % VRAM_HEIGHT) * VRAM_WIDTH +
                           ((clutX + index) % VRAM_WIDTH)];
        } else if (depth == 1) { // 8-bit
          uint32_t tx = tpX + (u / 2);
          uint32_t ty = tpY + v;
          uint16_t block =
              vram_[(ty % VRAM_HEIGHT) * VRAM_WIDTH + (tx % VRAM_WIDTH)].raw;
          uint8_t index = (block >> ((u % 2) * 8)) & 0xFF;
          texColor = vram_[(clutY % VRAM_HEIGHT) * VRAM_WIDTH +
                           ((clutX + index) % VRAM_WIDTH)];
        } else { // 15-bit
          uint32_t tx = tpX + u;
          uint32_t ty = tpY + v;
          texColor = vram_[(ty % VRAM_HEIGHT) * VRAM_WIDTH + (tx % VRAM_WIDTH)];
        }

        if (texColor.raw == 0)
          continue; // Transparency

        uint32_t idx = (y % VRAM_HEIGHT) * VRAM_WIDTH + (x % VRAM_WIDTH);

        if (isRaw) {
          vram_[idx] = texColor;
        } else {
          // For modulated textures, we apply dither onto texture read.
          vram_[idx] = applyDither(texColor, p.x, p.y);
        }
      }
    }
  }
}

void GPU::executeLine() {
  uint32_t c = commandQueue_[0] & 0xFFFFFF;
  uint32_t opcode = commandQueue_[0] >> 24;
  Color16 c16;
  c16.raw = ((c & 0xFF) >> 3) | ((((c >> 8) & 0xFF) >> 3) << 5) |
            ((((c >> 16) & 0xFF) >> 3) << 10);

  bool isGouraud = (opcode & 0x10) != 0;
  bool isBlend = (opcode & 0x02) != 0;

  Vertex v0, v1;
  v0.x = (int16_t)(commandQueue_[1] & 0xFFFF) + drawOffsetX_;
  v0.y = (int16_t)(commandQueue_[1] >> 16) + drawOffsetY_;

  if (!isGouraud) {
    v1.x = (int16_t)(commandQueue_[2] & 0xFFFF) + drawOffsetX_;
    v1.y = (int16_t)(commandQueue_[2] >> 16) + drawOffsetY_;
  } else {
    v1.x = (int16_t)(commandQueue_[3] & 0xFFFF) + drawOffsetX_;
    v1.y = (int16_t)(commandQueue_[3] >> 16) + drawOffsetY_;
  }

  int dx = std::abs(v1.x - v0.x);
  int sx = v0.x < v1.x ? 1 : -1;
  int dy = -std::abs(v1.y - v0.y);
  int sy = v0.y < v1.y ? 1 : -1;
  int err = dx + dy;

  int x = v0.x;
  int y = v0.y;

  while (true) {
    if (x >= drawAreaX1_ && x <= drawAreaX2_ && y >= drawAreaY1_ &&
        y <= drawAreaY2_) {
      uint32_t idx = (y % VRAM_HEIGHT) * VRAM_WIDTH + (x % VRAM_WIDTH);
      Color16 finalColor = c16;
      if (isBlend) {
        finalColor = applyBlend(finalColor, vram_[idx]);
      }
      vram_[idx] = finalColor;
    }
    if (x == v1.x && y == v1.y)
      break;
    int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y += sy;
    }
  }
}

void GPU::executeRect() {
  uint32_t c = commandQueue_[0] & 0xFFFFFF;
  uint32_t opcode = commandQueue_[0] >> 24;
  Color16 c16;
  c16.raw = ((c & 0xFF) >> 3) | ((((c >> 8) & 0xFF) >> 3) << 5) |
            ((((c >> 16) & 0xFF) >> 3) << 10);

  Vertex v;
  v.x = (int16_t)(commandQueue_[1] & 0xFFFF) + drawOffsetX_;
  v.y = (int16_t)(commandQueue_[1] >> 16) + drawOffsetY_;

  int w = 0, h = 0;
  bool isTextured = (opcode & 0x04) != 0;
  bool isBlend = (opcode & 0x02) != 0;
  int sizeWordIdx = isTextured ? 3 : 2;

  switch ((opcode >> 3) & 3) {
  case 0: // Variable size
    w = commandQueue_[sizeWordIdx] & 0xFFFF;
    h = commandQueue_[sizeWordIdx] >> 16;
    break;
  case 1: // 1x1
    w = 1;
    h = 1;
    break;
  case 2: // 8x8
    w = 8;
    h = 8;
    break;
  case 3: // 16x16
    w = 16;
    h = 16;
    break;
  }

  for (int dy = 0; dy < h; ++dy) {
    for (int dx = 0; dx < w; ++dx) {
      int px = v.x + dx;
      int py = v.y + dy;
      if (px >= drawAreaX1_ && px <= drawAreaX2_ && py >= drawAreaY1_ &&
          py <= drawAreaY2_) {
        uint32_t idx = (py % VRAM_HEIGHT) * VRAM_WIDTH + (px % VRAM_WIDTH);
        Color16 finalColor = c16;
        if (isBlend) {
          finalColor = applyBlend(finalColor, vram_[idx]);
        }
        vram_[idx] = finalColor;
      }
    }
  }
}

void GPU::executeClearCache() {
  // Clears the texture cache. NOP for our accurate software rasterizer since we
  // read directly from VRAM.
}

void GPU::executeFillRect() {
  uint32_t color = commandQueue_[0] & 0xFFFFFF;
  uint32_t pos = commandQueue_[1];
  uint32_t size = commandQueue_[2];

  uint32_t x = pos & 0x3FF;
  uint32_t y = (pos >> 16) & 0x1FF;
  uint32_t w = size & 0x3FF;
  // width/height of 0 means 1024/512
  w = ((w - 1) & 0x3FF) + 1;
  uint32_t h = (size >> 16) & 0x1FF;
  h = ((h - 1) & 0x1FF) + 1;

  // Convert 24-bit RGB to 15-bit
  uint16_t r5 = (color & 0xFF) >> 3;
  uint16_t g5 = ((color >> 8) & 0xFF) >> 3;
  uint16_t b5 = ((color >> 16) & 0xFF) >> 3;
  uint16_t a1 = 0; // Fill commands don't set the mask bit usually

  Color16 c16;
  c16.raw = r5 | (g5 << 5) | (b5 << 10) | (a1 << 15);

  for (uint32_t dy = 0; dy < h; ++dy) {
    for (uint32_t dx = 0; dx < w; ++dx) {
      uint32_t px = (x + dx) % VRAM_WIDTH;
      uint32_t py = (y + dy) % VRAM_HEIGHT;
      vram_[py * VRAM_WIDTH + px] = c16;
    }
  }
}

void GPU::executeCPUToVRAM() {
  uint32_t pos = commandQueue_[1];
  uint32_t size = commandQueue_[2];

  vramTransfer_.destX = pos & 0x3FF;
  vramTransfer_.destY = (pos >> 16) & 0x1FF;

  uint32_t w = size & 0xFFFF; // Actual width isn't masked to 0x3FF for
                              // transfers, but for size calculation
  if (w == 0)
    w = 1024;
  uint32_t h = (size >> 16) & 0xFFFF;
  if (h == 0)
    h = 512;

  vramTransfer_.width = w;
  vramTransfer_.height = h;

  vramTransfer_.currX = vramTransfer_.destX;
  vramTransfer_.currY = vramTransfer_.destY;

  uint32_t pixels = w * h;
  vramTransfer_.transferWordsRemaining = (pixels + 1) / 2;
  vramTransfer_.isWritingToVRAM = true;
}

void GPU::executeCopyVRAM() {
  uint32_t srcPos = commandQueue_[1];
  uint32_t dstPos = commandQueue_[2];
  uint32_t size = commandQueue_[3];

  uint32_t sx = srcPos & 0x3FF;
  uint32_t sy = (srcPos >> 16) & 0x1FF;
  uint32_t dx = dstPos & 0x3FF;
  uint32_t dy = (dstPos >> 16) & 0x1FF;

  uint32_t w = size & 0xFFFF;
  if (w == 0)
    w = 1024;
  uint32_t h = (size >> 16) & 0xFFFF;
  if (h == 0)
    h = 512;

  // The copy can be overlapping, so it's typically safe to do it pixel-by-pixel
  // (normally left to right, top to bottom, but hardware might handle overlap
  // differently)
  for (uint32_t py = 0; py < h; ++py) {
    for (uint32_t px = 0; px < w; ++px) {
      uint32_t s_idx =
          ((sy + py) % VRAM_HEIGHT) * VRAM_WIDTH + ((sx + px) % VRAM_WIDTH);
      uint32_t d_idx =
          ((dy + py) % VRAM_HEIGHT) * VRAM_WIDTH + ((dx + px) % VRAM_WIDTH);
      vram_[d_idx] = vram_[s_idx];
    }
  }
}

void GPU::executeVRAMToCPU() {
  uint32_t pos = commandQueue_[1];
  uint32_t size = commandQueue_[2];

  vramTransfer_.srcX = pos & 0x3FF;
  vramTransfer_.srcY = (pos >> 16) & 0x1FF;

  uint32_t w = size & 0xFFFF;
  if (w == 0)
    w = 1024;
  uint32_t h = (size >> 16) & 0xFFFF;
  if (h == 0)
    h = 512;

  vramTransfer_.width = w;
  vramTransfer_.height = h;

  vramTransfer_.currX = vramTransfer_.srcX;
  vramTransfer_.currY = vramTransfer_.srcY;

  uint32_t pixels = w * h;
  vramTransfer_.transferWordsRemaining = (pixels + 1) / 2;
  vramTransfer_.isReadingFromVRAM = true;
}

// ─── Gouraud Shading Polygons ───────────────────────────────────────────────

static Color24 extractColor24(uint32_t word) {
  Color24 c;
  c.r = word & 0xFF;
  c.g = (word >> 8) & 0xFF;
  c.b = (word >> 16) & 0xFF;
  return c;
}

void GPU::executeGouraudPoly3() {
  // Word layout: c0+cmd, v0, c1, v1, c2, v2
  Color24 c[3];
  Vertex v[3];
  c[0] = extractColor24(commandQueue_[0]);
  v[0].x = (int16_t)(commandQueue_[1] & 0xFFFF) + drawOffsetX_;
  v[0].y = (int16_t)(commandQueue_[1] >> 16) + drawOffsetY_;
  c[1] = extractColor24(commandQueue_[2]);
  v[1].x = (int16_t)(commandQueue_[3] & 0xFFFF) + drawOffsetX_;
  v[1].y = (int16_t)(commandQueue_[3] >> 16) + drawOffsetY_;
  c[2] = extractColor24(commandQueue_[4]);
  v[2].x = (int16_t)(commandQueue_[5] & 0xFFFF) + drawOffsetX_;
  v[2].y = (int16_t)(commandQueue_[5] >> 16) + drawOffsetY_;

  uint32_t opcode = commandQueue_[0] >> 24;
  bool isBlend = (opcode & 2) != 0;

  rasterizeGouraudTriangle(v[0], v[1], v[2], c[0], c[1], c[2], isBlend);
}

void GPU::executeGouraudPoly4() {
  // Word layout: c0+cmd, v0, c1, v1, c2, v2, c3, v3
  Color24 c[4];
  Vertex v[4];
  c[0] = extractColor24(commandQueue_[0]);
  v[0].x = (int16_t)(commandQueue_[1] & 0xFFFF) + drawOffsetX_;
  v[0].y = (int16_t)(commandQueue_[1] >> 16) + drawOffsetY_;
  c[1] = extractColor24(commandQueue_[2]);
  v[1].x = (int16_t)(commandQueue_[3] & 0xFFFF) + drawOffsetX_;
  v[1].y = (int16_t)(commandQueue_[3] >> 16) + drawOffsetY_;
  c[2] = extractColor24(commandQueue_[4]);
  v[2].x = (int16_t)(commandQueue_[5] & 0xFFFF) + drawOffsetX_;
  v[2].y = (int16_t)(commandQueue_[5] >> 16) + drawOffsetY_;
  c[3] = extractColor24(commandQueue_[6]);
  v[3].x = (int16_t)(commandQueue_[7] & 0xFFFF) + drawOffsetX_;
  v[3].y = (int16_t)(commandQueue_[7] >> 16) + drawOffsetY_;

  uint32_t opcode = commandQueue_[0] >> 24;
  bool isBlend = (opcode & 2) != 0;

  rasterizeGouraudTriangle(v[0], v[1], v[2], c[0], c[1], c[2], isBlend);
  rasterizeGouraudTriangle(v[1], v[2], v[3], c[1], c[2], c[3], isBlend);
}

void GPU::executeGouraudTexturedPoly3() {
  // Word layout: c0+cmd, v0, uv0+clut, c1, v1, uv1+tpage, c2, v2, uv2
  Color24 c[3];
  Vertex v[3];
  TexCoord t[3];
  uint16_t clut, tpage;

  c[0] = extractColor24(commandQueue_[0]);
  v[0].x = (int16_t)(commandQueue_[1] & 0xFFFF) + drawOffsetX_;
  v[0].y = (int16_t)(commandQueue_[1] >> 16) + drawOffsetY_;
  t[0].u = commandQueue_[2] & 0xFF;
  t[0].v = (commandQueue_[2] >> 8) & 0xFF;
  clut = (commandQueue_[2] >> 16) & 0xFFFF;

  c[1] = extractColor24(commandQueue_[3]);
  v[1].x = (int16_t)(commandQueue_[4] & 0xFFFF) + drawOffsetX_;
  v[1].y = (int16_t)(commandQueue_[4] >> 16) + drawOffsetY_;
  t[1].u = commandQueue_[5] & 0xFF;
  t[1].v = (commandQueue_[5] >> 8) & 0xFF;
  tpage = (commandQueue_[5] >> 16) & 0xFFFF;

  c[2] = extractColor24(commandQueue_[6]);
  v[2].x = (int16_t)(commandQueue_[7] & 0xFFFF) + drawOffsetX_;
  v[2].y = (int16_t)(commandQueue_[7] >> 16) + drawOffsetY_;
  t[2].u = commandQueue_[8] & 0xFF;
  t[2].v = (commandQueue_[8] >> 8) & 0xFF;

  uint32_t opcode = commandQueue_[0] >> 24;
  bool isRaw = (opcode & 1) != 0;
  bool isBlend = (opcode & 2) != 0;

  rasterizeGouraudTexturedTriangle(v[0], v[1], v[2], t[0], t[1], t[2], c[0],
                                   c[1], c[2], clut, tpage, isRaw, isBlend);
}

void GPU::executeGouraudTexturedPoly4() {
  // Word layout: c0+cmd, v0, uv0+clut, c1, v1, uv1+tpage, c2, v2, uv2, c3,
  // v3, uv3
  Color24 c[4];
  Vertex v[4];
  TexCoord t[4];
  uint16_t clut, tpage;

  c[0] = extractColor24(commandQueue_[0]);
  v[0].x = (int16_t)(commandQueue_[1] & 0xFFFF) + drawOffsetX_;
  v[0].y = (int16_t)(commandQueue_[1] >> 16) + drawOffsetY_;
  t[0].u = commandQueue_[2] & 0xFF;
  t[0].v = (commandQueue_[2] >> 8) & 0xFF;
  clut = (commandQueue_[2] >> 16) & 0xFFFF;

  c[1] = extractColor24(commandQueue_[3]);
  v[1].x = (int16_t)(commandQueue_[4] & 0xFFFF) + drawOffsetX_;
  v[1].y = (int16_t)(commandQueue_[4] >> 16) + drawOffsetY_;
  t[1].u = commandQueue_[5] & 0xFF;
  t[1].v = (commandQueue_[5] >> 8) & 0xFF;
  tpage = (commandQueue_[5] >> 16) & 0xFFFF;

  c[2] = extractColor24(commandQueue_[6]);
  v[2].x = (int16_t)(commandQueue_[7] & 0xFFFF) + drawOffsetX_;
  v[2].y = (int16_t)(commandQueue_[7] >> 16) + drawOffsetY_;
  t[2].u = commandQueue_[8] & 0xFF;
  t[2].v = (commandQueue_[8] >> 8) & 0xFF;

  c[3] = extractColor24(commandQueue_[9]);
  v[3].x = (int16_t)(commandQueue_[10] & 0xFFFF) + drawOffsetX_;
  v[3].y = (int16_t)(commandQueue_[10] >> 16) + drawOffsetY_;
  t[3].u = commandQueue_[11] & 0xFF;
  t[3].v = (commandQueue_[11] >> 8) & 0xFF;

  uint32_t opcode = commandQueue_[0] >> 24;
  bool isRaw = (opcode & 1) != 0;
  bool isBlend = (opcode & 2) != 0;

  rasterizeGouraudTexturedTriangle(v[0], v[1], v[2], t[0], t[1], t[2], c[0],
                                   c[1], c[2], clut, tpage, isRaw, isBlend);
  rasterizeGouraudTexturedTriangle(v[1], v[2], v[3], t[1], t[2], t[3], c[1],
                                   c[2], c[3], clut, tpage, isRaw, isBlend);
}

void GPU::rasterizeGouraudTriangle(Vertex v0, Vertex v1, Vertex v2, Color24 c0,
                                   Color24 c1, Color24 c2, bool blend) {
  int area = edgeFunction(v0, v1, v2);
  if (area == 0)
    return;
  if (area < 0) {
    std::swap(v1, v2);
    std::swap(c1, c2);
    area = -area;
  }

  int minX = std::max(drawAreaX1_, std::min({v0.x, v1.x, v2.x}));
  int minY = std::max(drawAreaY1_, std::min({v0.y, v1.y, v2.y}));
  int maxX = std::min(drawAreaX2_, std::max({v0.x, v1.x, v2.x}));
  int maxY = std::min(drawAreaY2_, std::max({v0.y, v1.y, v2.y}));

  for (int y = minY; y <= maxY; ++y) {
    for (int x = minX; x <= maxX; ++x) {
      Vertex p{x, y};
      int w0 = edgeFunction(v1, v2, p);
      int w1 = edgeFunction(v2, v0, p);
      int w2 = edgeFunction(v0, v1, p);

      if (w0 >= 0 && w1 >= 0 && w2 >= 0) {
        // Interpolate RGB per-vertex using barycentric coordinates
        int r = (w0 * c0.r + w1 * c1.r + w2 * c2.r) / area;
        int g = (w0 * c0.g + w1 * c1.g + w2 * c2.g) / area;
        int b = (w0 * c0.b + w1 * c1.b + w2 * c2.b) / area;

        // Clamp
        r = std::clamp(r, 0, 255);
        g = std::clamp(g, 0, 255);
        b = std::clamp(b, 0, 255);

        Color16 c16;
        c16.raw = ((r >> 3) & 0x1F) | (((g >> 3) & 0x1F) << 5) |
                  (((b >> 3) & 0x1F) << 10);

        Color16 finalColor = applyDither(c16, x, y);

        uint32_t idx = (y % VRAM_HEIGHT) * VRAM_WIDTH + (x % VRAM_WIDTH);
        if (blend) {
          finalColor = applyBlend(finalColor, vram_[idx]);
        }
        vram_[idx] = finalColor;
      }
    }
  }
}

void GPU::rasterizeGouraudTexturedTriangle(Vertex v0, Vertex v1, Vertex v2,
                                           TexCoord t0, TexCoord t1,
                                           TexCoord t2, Color24 c0, Color24 c1,
                                           Color24 c2, uint16_t clut,
                                           uint16_t tpage, bool isRaw,
                                           bool blend) {
  int area = edgeFunction(v0, v1, v2);
  if (area == 0)
    return;
  if (area < 0) {
    std::swap(v1, v2);
    std::swap(t1, t2);
    std::swap(c1, c2);
    area = -area;
  }

  int minX = std::max(drawAreaX1_, std::min({v0.x, v1.x, v2.x}));
  int minY = std::max(drawAreaY1_, std::min({v0.y, v1.y, v2.y}));
  int maxX = std::min(drawAreaX2_, std::max({v0.x, v1.x, v2.x}));
  int maxY = std::min(drawAreaY2_, std::max({v0.y, v1.y, v2.y}));

  uint32_t tpX = (tpage & 0xF) * 64;
  uint32_t tpY = ((tpage >> 4) & 1) * 256;
  uint32_t depth = (tpage >> 7) & 3;

  uint32_t clutX = (clut & 0x3F) * 16;
  uint32_t clutY = (clut >> 6) & 0x1FF;

  for (int y = minY; y <= maxY; ++y) {
    for (int x = minX; x <= maxX; ++x) {
      Vertex p{x, y};
      int w0 = edgeFunction(v1, v2, p);
      int w1 = edgeFunction(v2, v0, p);
      int w2 = edgeFunction(v0, v1, p);

      if (w0 >= 0 && w1 >= 0 && w2 >= 0) {
        int u = (w0 * t0.u + w1 * t1.u + w2 * t2.u) / area;
        int v = (w0 * t0.v + w1 * t1.v + w2 * t2.v) / area;

        // Apply Texture Window
        if (texWindowMaskX_ != 0 || texWindowOffsetX_ != 0) {
          u = (u & ~(texWindowMaskX_)) | (texWindowOffsetX_ & texWindowMaskX_);
        }
        if (texWindowMaskY_ != 0 || texWindowOffsetY_ != 0) {
          v = (v & ~(texWindowMaskY_)) | (texWindowOffsetY_ & texWindowMaskY_);
        }

        // Texture lookup (same as flat-textured)
        Color16 texColor;
        if (depth == 0) { // 4-bit
          uint32_t tx = tpX + (u / 4);
          uint32_t ty = tpY + v;
          uint16_t block =
              vram_[(ty % VRAM_HEIGHT) * VRAM_WIDTH + (tx % VRAM_WIDTH)].raw;
          uint8_t index = (block >> ((u % 4) * 4)) & 0xF;
          texColor = vram_[(clutY % VRAM_HEIGHT) * VRAM_WIDTH +
                           ((clutX + index) % VRAM_WIDTH)];
        } else if (depth == 1) { // 8-bit
          uint32_t tx = tpX + (u / 2);
          uint32_t ty = tpY + v;
          uint16_t block =
              vram_[(ty % VRAM_HEIGHT) * VRAM_WIDTH + (tx % VRAM_WIDTH)].raw;
          uint8_t index = (block >> ((u % 2) * 8)) & 0xFF;
          texColor = vram_[(clutY % VRAM_HEIGHT) * VRAM_WIDTH +
                           ((clutX + index) % VRAM_WIDTH)];
        } else { // 15-bit
          uint32_t tx = tpX + u;
          uint32_t ty = tpY + v;
          texColor = vram_[(ty % VRAM_HEIGHT) * VRAM_WIDTH + (tx % VRAM_WIDTH)];
        }

        if (texColor.raw == 0)
          continue; // Transparency

        uint32_t idx = (y % VRAM_HEIGHT) * VRAM_WIDTH + (x % VRAM_WIDTH);

        if (isRaw) {
          vram_[idx] = texColor;
        } else {
          // Modulate texture color with interpolated vertex color
          int r = (w0 * c0.r + w1 * c1.r + w2 * c2.r) / area;
          int g = (w0 * c0.g + w1 * c1.g + w2 * c2.g) / area;
          int b = (w0 * c0.b + w1 * c1.b + w2 * c2.b) / area;

          // Extract texel RGB
          int tR = (texColor.raw & 0x1F) << 3;
          int tG = ((texColor.raw >> 5) & 0x1F) << 3;
          int tB = ((texColor.raw >> 10) & 0x1F) << 3;

          // Modulate: (tex * vertex) / 128, clamped to 255
          int mR = std::clamp((tR * r) / 128, 0, 255);
          int mG = std::clamp((tG * g) / 128, 0, 255);
          int mB = std::clamp((tB * b) / 128, 0, 255);

          Color16 modColor;
          modColor.raw = ((mR >> 3) & 0x1F) | (((mG >> 3) & 0x1F) << 5) |
                         (((mB >> 3) & 0x1F) << 10) | (texColor.raw & 0x8000);

          Color16 finalColor = applyDither(modColor, x, y);
          if (blend) {
            finalColor = applyBlend(finalColor, vram_[idx]);
          }
          vram_[idx] = finalColor;
        }
      }
    }
  }
}

} // namespace ps1::gpu
