// ps1Runtime — GTE Runtime Implementation
// PS1-accurate fixed-point math for all 22 GTE commands

#include <algorithm>
#include <cstring>
#include <runtime/gte.h>

namespace ps1 {

// CLZ (Count Leading Zeros)

static int countLeadingZeros(uint32_t val) {
  if (val == 0)
    return 32;
#ifdef __GNUC__
  return __builtin_clz(val);
#else
  int n = 0;
  if ((val & 0xFFFF0000) == 0) {
    n += 16;
    val <<= 16;
  }
  if ((val & 0xFF000000) == 0) {
    n += 8;
    val <<= 8;
  }
  if ((val & 0xF0000000) == 0) {
    n += 4;
    val <<= 4;
  }
  if ((val & 0xC0000000) == 0) {
    n += 2;
    val <<= 2;
  }
  if ((val & 0x80000000) == 0) {
    n += 1;
  }
  return n;
#endif
}

// Newton-Raphson Division Table

static const uint8_t s_unrTable[257] = {
    0xFF, 0xFD, 0xFB, 0xF9, 0xF7, 0xF5, 0xF3, 0xF1, 0xEF, 0xEE, 0xEC, 0xEA,
    0xE8, 0xE6, 0xE4, 0xE3, 0xE1, 0xDF, 0xDD, 0xDC, 0xDA, 0xD8, 0xD6, 0xD5,
    0xD3, 0xD1, 0xD0, 0xCE, 0xCD, 0xCB, 0xC9, 0xC8, 0xC6, 0xC5, 0xC3, 0xC1,
    0xC0, 0xBE, 0xBD, 0xBB, 0xBA, 0xB8, 0xB7, 0xB5, 0xB4, 0xB2, 0xB1, 0xAF,
    0xAE, 0xAC, 0xAB, 0xAA, 0xA8, 0xA7, 0xA5, 0xA4, 0xA3, 0xA1, 0xA0, 0x9F,
    0x9D, 0x9C, 0x9B, 0x99, 0x98, 0x97, 0x95, 0x94, 0x93, 0x92, 0x90, 0x8F,
    0x8E, 0x8D, 0x8B, 0x8A, 0x89, 0x88, 0x87, 0x85, 0x84, 0x83, 0x82, 0x81,
    0x7F, 0x7E, 0x7D, 0x7C, 0x7B, 0x7A, 0x79, 0x78, 0x77, 0x75, 0x74, 0x73,
    0x72, 0x71, 0x70, 0x6F, 0x6E, 0x6D, 0x6C, 0x6B, 0x6A, 0x69, 0x68, 0x67,
    0x66, 0x65, 0x64, 0x63, 0x62, 0x61, 0x60, 0x5F, 0x5E, 0x5D, 0x5D, 0x5C,
    0x5B, 0x5A, 0x59, 0x58, 0x57, 0x56, 0x55, 0x54, 0x54, 0x53, 0x52, 0x51,
    0x50, 0x4F, 0x4F, 0x4E, 0x4D, 0x4C, 0x4B, 0x4B, 0x4A, 0x49, 0x48, 0x47,
    0x47, 0x46, 0x45, 0x44, 0x44, 0x43, 0x42, 0x41, 0x41, 0x40, 0x3F, 0x3E,
    0x3E, 0x3D, 0x3C, 0x3C, 0x3B, 0x3A, 0x39, 0x39, 0x38, 0x37, 0x37, 0x36,
    0x35, 0x35, 0x34, 0x33, 0x33, 0x32, 0x31, 0x31, 0x30, 0x2F, 0x2F, 0x2E,
    0x2E, 0x2D, 0x2C, 0x2C, 0x2B, 0x2A, 0x2A, 0x29, 0x29, 0x28, 0x27, 0x27,
    0x26, 0x26, 0x25, 0x24, 0x24, 0x23, 0x23, 0x22, 0x22, 0x21, 0x20, 0x20,
    0x1F, 0x1F, 0x1E, 0x1E, 0x1D, 0x1D, 0x1C, 0x1C, 0x1B, 0x1A, 0x1A, 0x19,
    0x19, 0x18, 0x18, 0x17, 0x17, 0x16, 0x16, 0x15, 0x15, 0x14, 0x14, 0x13,
    0x13, 0x12, 0x12, 0x11, 0x11, 0x10, 0x10, 0x0F, 0x0F, 0x0E, 0x0E, 0x0D,
    0x0D, 0x0C, 0x0C, 0x0B, 0x0B, 0x0A, 0x0A, 0x09, 0x09, 0x09, 0x08, 0x08,
    0x07, 0x07, 0x06, 0x06, 0x05};

// Register Access

uint32_t GTE::readData(CPUContext *ctx, uint8_t reg) {
  reg &= 0x1F;
  switch (reg) {
  case GTE_IRGB:
    return ((ctx->cop2d[GTE_IR1] / 0x80) & 0x1F) |
           (((ctx->cop2d[GTE_IR2] / 0x80) & 0x1F) << 5) |
           (((ctx->cop2d[GTE_IR3] / 0x80) & 0x1F) << 10);
  case GTE_ORGB:
    return ((ctx->cop2d[GTE_IR1] / 0x80) & 0x1F) |
           (((ctx->cop2d[GTE_IR2] / 0x80) & 0x1F) << 5) |
           (((ctx->cop2d[GTE_IR3] / 0x80) & 0x1F) << 10);
  case GTE_LZCS:
    return ctx->cop2d[GTE_LZCS];
  case GTE_LZCR: {
    int32_t val = static_cast<int32_t>(ctx->cop2d[GTE_LZCS]);
    if (val >= 0)
      return countLeadingZeros(val);
    return countLeadingZeros(~static_cast<uint32_t>(val));
  }
  default:
    return ctx->cop2d[reg];
  }
}

void GTE::writeData(CPUContext *ctx, uint8_t reg, uint32_t val) {
  reg &= 0x1F;
  switch (reg) {
  case GTE_IRGB:
    ctx->cop2d[GTE_IR1] = (val & 0x1F) * 0x80;
    ctx->cop2d[GTE_IR2] = ((val >> 5) & 0x1F) * 0x80;
    ctx->cop2d[GTE_IR3] = ((val >> 10) & 0x1F) * 0x80;
    break;
  case GTE_LZCR:
    break; // read-only
  case GTE_SXYP:
    ctx->cop2d[GTE_SXY0] = ctx->cop2d[GTE_SXY1];
    ctx->cop2d[GTE_SXY1] = ctx->cop2d[GTE_SXY2];
    ctx->cop2d[GTE_SXY2] = val;
    break;
  default:
    ctx->cop2d[reg] = val;
    break;
  }
}

uint32_t GTE::readControl(CPUContext *ctx, uint8_t reg) {
  return ctx->cop2c[reg & 0x1F];
}

void GTE::writeControl(CPUContext *ctx, uint8_t reg, uint32_t val) {
  ctx->cop2c[reg & 0x1F] = val;
}

// Internal Helpers

void GTE::getVector(CPUContext *ctx, uint8_t v, int16_t &vx, int16_t &vy,
                    int16_t &vz) {
  switch (v) {
  case 0:
    vx = lo16(ctx->cop2d[GTE_VXY0]);
    vy = hi16(ctx->cop2d[GTE_VXY0]);
    vz = lo16(ctx->cop2d[GTE_VZ0]);
    break;
  case 1:
    vx = lo16(ctx->cop2d[GTE_VXY1]);
    vy = hi16(ctx->cop2d[GTE_VXY1]);
    vz = lo16(ctx->cop2d[GTE_VZ1]);
    break;
  case 2:
    vx = lo16(ctx->cop2d[GTE_VXY2]);
    vy = hi16(ctx->cop2d[GTE_VXY2]);
    vz = lo16(ctx->cop2d[GTE_VZ2]);
    break;
  case 3:
    vx = static_cast<int16_t>(ctx->cop2d[GTE_IR1]);
    vy = static_cast<int16_t>(ctx->cop2d[GTE_IR2]);
    vz = static_cast<int16_t>(ctx->cop2d[GTE_IR3]);
    break;
  }
}

void GTE::getMatrix(CPUContext *ctx, uint8_t m, int16_t mat[3][3]) {
  uint8_t base = m * 8; // RT=0, L=8, LC=16
  if (m >= 3) {
    std::memset(mat, 0, sizeof(int16_t) * 9);
    return;
  }
  mat[0][0] = lo16(ctx->cop2c[base + 0]);
  mat[0][1] = hi16(ctx->cop2c[base + 0]);
  mat[0][2] = lo16(ctx->cop2c[base + 1]);
  mat[1][0] = hi16(ctx->cop2c[base + 1]);
  mat[1][1] = lo16(ctx->cop2c[base + 2]);
  mat[1][2] = hi16(ctx->cop2c[base + 2]);
  mat[2][0] = lo16(ctx->cop2c[base + 3]);
  mat[2][1] = hi16(ctx->cop2c[base + 3]);
  mat[2][2] = lo16(ctx->cop2c[base + 4]);
}

void GTE::getTranslation(CPUContext *ctx, uint8_t tv, int32_t tr[3]) {
  switch (tv) {
  case 0:
    tr[0] = static_cast<int32_t>(ctx->cop2c[GTE_TRX]);
    tr[1] = static_cast<int32_t>(ctx->cop2c[GTE_TRY]);
    tr[2] = static_cast<int32_t>(ctx->cop2c[GTE_TRZ]);
    break;
  case 1:
    tr[0] = static_cast<int32_t>(ctx->cop2c[GTE_RBK]);
    tr[1] = static_cast<int32_t>(ctx->cop2c[GTE_GBK]);
    tr[2] = static_cast<int32_t>(ctx->cop2c[GTE_BBK]);
    break;
  case 2:
    tr[0] = static_cast<int32_t>(ctx->cop2c[GTE_RFC]);
    tr[1] = static_cast<int32_t>(ctx->cop2c[GTE_GFC]);
    tr[2] = static_cast<int32_t>(ctx->cop2c[GTE_BFC]);
    break;
  case 3:
    tr[0] = 0;
    tr[1] = 0;
    tr[2] = 0;
    break;
  }
}

void GTE::checkMAC(CPUContext *ctx, int64_t val, uint8_t macIdx) {
  if (val > INT64_C(0x7FFFFFFF)) {
    if (macIdx == 0)
      ctx->cop2c[GTE_FLAG] |= gte_flag::MAC0_OF;
    else if (macIdx == 1)
      ctx->cop2c[GTE_FLAG] |= gte_flag::MAC1_OF;
    else if (macIdx == 2)
      ctx->cop2c[GTE_FLAG] |= gte_flag::MAC2_OF;
    else if (macIdx == 3)
      ctx->cop2c[GTE_FLAG] |= gte_flag::MAC3_OF;
  }
  if (val < INT64_C(-0x80000000)) {
    if (macIdx == 0)
      ctx->cop2c[GTE_FLAG] |= gte_flag::MAC0_UF;
    else if (macIdx == 1)
      ctx->cop2c[GTE_FLAG] |= gte_flag::MAC1_UF;
    else if (macIdx == 2)
      ctx->cop2c[GTE_FLAG] |= gte_flag::MAC2_UF;
    else if (macIdx == 3)
      ctx->cop2c[GTE_FLAG] |= gte_flag::MAC3_UF;
  }
}

int32_t GTE::clampIR(CPUContext *ctx, int32_t val, uint8_t irIdx, bool lm) {
  int32_t lo = lm ? 0 : -0x8000;
  int32_t hi = 0x7FFF;
  if (irIdx == 0) {
    lo = 0;
    hi = 0x1000;
  }

  if (val < lo) {
    if (irIdx == 0)
      ctx->cop2c[GTE_FLAG] |= gte_flag::IR0_SATURATED;
    else if (irIdx == 1)
      ctx->cop2c[GTE_FLAG] |= gte_flag::IR1_SATURATED;
    else if (irIdx == 2)
      ctx->cop2c[GTE_FLAG] |= gte_flag::IR2_SATURATED;
    else if (irIdx == 3)
      ctx->cop2c[GTE_FLAG] |= gte_flag::IR3_SATURATED;
    return lo;
  }
  if (val > hi) {
    if (irIdx == 0)
      ctx->cop2c[GTE_FLAG] |= gte_flag::IR0_SATURATED;
    else if (irIdx == 1)
      ctx->cop2c[GTE_FLAG] |= gte_flag::IR1_SATURATED;
    else if (irIdx == 2)
      ctx->cop2c[GTE_FLAG] |= gte_flag::IR2_SATURATED;
    else if (irIdx == 3)
      ctx->cop2c[GTE_FLAG] |= gte_flag::IR3_SATURATED;
    return hi;
  }
  return val;
}

void GTE::setMAC(CPUContext *ctx, int64_t mac1, int64_t mac2, int64_t mac3,
                 bool sf) {
  int shift = sf ? 12 : 0;
  checkMAC(ctx, mac1, 1);
  checkMAC(ctx, mac2, 2);
  checkMAC(ctx, mac3, 3);
  ctx->cop2d[GTE_MAC1] =
      static_cast<uint32_t>(static_cast<int32_t>(mac1 >> shift));
  ctx->cop2d[GTE_MAC2] =
      static_cast<uint32_t>(static_cast<int32_t>(mac2 >> shift));
  ctx->cop2d[GTE_MAC3] =
      static_cast<uint32_t>(static_cast<int32_t>(mac3 >> shift));
}

void GTE::setIR(CPUContext *ctx, int32_t ir1, int32_t ir2, int32_t ir3,
                bool lm) {
  ctx->cop2d[GTE_IR1] = static_cast<uint32_t>(clampIR(ctx, ir1, 1, lm));
  ctx->cop2d[GTE_IR2] = static_cast<uint32_t>(clampIR(ctx, ir2, 2, lm));
  ctx->cop2d[GTE_IR3] = static_cast<uint32_t>(clampIR(ctx, ir3, 3, lm));
}

void GTE::setMAC0(CPUContext *ctx, int64_t val) {
  checkMAC(ctx, val, 0);
  ctx->cop2d[GTE_MAC0] = static_cast<uint32_t>(static_cast<int32_t>(val));
}

void GTE::setIR0(CPUContext *ctx, int32_t val, bool /*lm*/) {
  ctx->cop2d[GTE_IR0] = static_cast<uint32_t>(clampIR(ctx, val, 0, true));
}

void GTE::pushSXY(CPUContext *ctx, int32_t sx, int32_t sy) {
  ctx->cop2d[GTE_SXY0] = ctx->cop2d[GTE_SXY1];
  ctx->cop2d[GTE_SXY1] = ctx->cop2d[GTE_SXY2];
  int32_t csx = sx, csy = sy;
  if (sx < -0x400) {
    csx = -0x400;
    ctx->cop2c[GTE_FLAG] |= gte_flag::SX2_SATURATED;
  }
  if (sx > 0x3FF) {
    csx = 0x3FF;
    ctx->cop2c[GTE_FLAG] |= gte_flag::SX2_SATURATED;
  }
  if (sy < -0x400) {
    csy = -0x400;
    ctx->cop2c[GTE_FLAG] |= gte_flag::SY2_SATURATED;
  }
  if (sy > 0x3FF) {
    csy = 0x3FF;
    ctx->cop2c[GTE_FLAG] |= gte_flag::SY2_SATURATED;
  }
  ctx->cop2d[GTE_SXY2] =
      static_cast<uint32_t>((static_cast<uint16_t>(csx) & 0xFFFF) |
                            (static_cast<uint16_t>(csy) << 16));
}

void GTE::pushSZ(CPUContext *ctx, int32_t sz) {
  ctx->cop2d[GTE_SZ0] = ctx->cop2d[GTE_SZ1];
  ctx->cop2d[GTE_SZ1] = ctx->cop2d[GTE_SZ2];
  ctx->cop2d[GTE_SZ2] = ctx->cop2d[GTE_SZ3];
  if (sz < 0) {
    sz = 0;
    ctx->cop2c[GTE_FLAG] |= gte_flag::SZ3_OTZ_SAT;
  }
  if (sz > 0xFFFF) {
    sz = 0xFFFF;
    ctx->cop2c[GTE_FLAG] |= gte_flag::SZ3_OTZ_SAT;
  }
  ctx->cop2d[GTE_SZ3] = static_cast<uint32_t>(sz);
}

void GTE::pushRGB(CPUContext *ctx, uint8_t r, uint8_t g, uint8_t b,
                  uint8_t cd) {
  ctx->cop2d[GTE_RGB0] = ctx->cop2d[GTE_RGB1];
  ctx->cop2d[GTE_RGB1] = ctx->cop2d[GTE_RGB2];
  ctx->cop2d[GTE_RGB2] = r | (static_cast<uint32_t>(g) << 8) |
                         (static_cast<uint32_t>(b) << 16) |
                         (static_cast<uint32_t>(cd) << 24);
}

uint32_t GTE::divide(CPUContext *ctx, uint16_t h, uint16_t sz3) {
  if (sz3 == 0) {
    ctx->cop2c[GTE_FLAG] |= gte_flag::DIV_OVERFLOW;
    return 0x1FFFF;
  }
  if (static_cast<uint32_t>(h) * 2 > static_cast<uint32_t>(sz3)) {
    ctx->cop2c[GTE_FLAG] |= gte_flag::DIV_OVERFLOW;
    return 0x1FFFF;
  }
  int shift = countLeadingZeros(sz3) - 16;
  uint64_t n = static_cast<uint64_t>(h) << shift;
  uint32_t d = static_cast<uint32_t>(sz3) << shift;
  uint32_t idx = (d >> 7) & 0xFF;
  if (idx > 256)
    idx = 256;
  int32_t factor = s_unrTable[idx] + 0x101;
  d = (d | 0x8000);
  uint32_t res = static_cast<uint32_t>(
      std::min<uint64_t>((n * factor + 0x8000) >> 16, 0x1FFFF));
  return res;
}

// MVMVA core

void GTE::doMVMVA(CPUContext *ctx, uint8_t mx, uint8_t mv, uint8_t tv, bool sf,
                  bool lm) {
  int16_t mat[3][3];
  int16_t vx, vy, vz;
  int32_t tr[3];
  getMatrix(ctx, mx, mat);
  getVector(ctx, mv, vx, vy, vz);
  getTranslation(ctx, tv, tr);

  int64_t mac1 = (static_cast<int64_t>(tr[0]) << 12) + mat[0][0] * vx +
                 mat[0][1] * vy + mat[0][2] * vz;
  int64_t mac2 = (static_cast<int64_t>(tr[1]) << 12) + mat[1][0] * vx +
                 mat[1][1] * vy + mat[1][2] * vz;
  int64_t mac3 = (static_cast<int64_t>(tr[2]) << 12) + mat[2][0] * vx +
                 mat[2][1] * vy + mat[2][2] * vz;

  setMAC(ctx, mac1, mac2, mac3, sf);
  setIR(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC2]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC3]), lm);
}

// RTPS core (single vertex)

void GTE::doRTPS(CPUContext *ctx, uint8_t vecIdx, bool sf, bool lm,
                 bool lastVertex) {
  // Rotation × Vector + Translation
  doMVMVA(ctx, 0, vecIdx, 0, sf, lm);

  // Push SZ FIFO
  pushSZ(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC3]) >> (sf ? 0 : 12));

  // Perspective division: n = H * 0x20000 / SZ3
  uint32_t quotient = divide(ctx, static_cast<uint16_t>(ctx->cop2c[GTE_H]),
                             static_cast<uint16_t>(ctx->cop2d[GTE_SZ3]));

  // Screen X,Y
  int64_t sx =
      static_cast<int64_t>(static_cast<int32_t>(ctx->cop2d[GTE_MAC1])) *
          quotient +
      static_cast<int64_t>(static_cast<int32_t>(ctx->cop2c[GTE_OFX]));
  int64_t sy =
      static_cast<int64_t>(static_cast<int32_t>(ctx->cop2d[GTE_MAC2])) *
          quotient +
      static_cast<int64_t>(static_cast<int32_t>(ctx->cop2c[GTE_OFY]));
  pushSXY(ctx, static_cast<int32_t>(sx >> 16), static_cast<int32_t>(sy >> 16));

  if (lastVertex) {
    // Depth queuing: MAC0 = DQB + DQA * quotient
    int64_t mac0val =
        static_cast<int64_t>(static_cast<int32_t>(ctx->cop2c[GTE_DQB])) +
        static_cast<int64_t>(static_cast<int16_t>(ctx->cop2c[GTE_DQA])) *
            quotient;
    setMAC0(ctx, mac0val);
    setIR0(ctx, static_cast<int32_t>(mac0val >> 12), lm);
  }
}

// Color clamp helper

static uint8_t clampColor(CPUContext *ctx, int32_t val, uint32_t satFlag) {
  if (val < 0) {
    ctx->cop2c[GTE_FLAG] |= satFlag;
    return 0;
  }
  if (val > 255) {
    ctx->cop2c[GTE_FLAG] |= satFlag;
    return 255;
  }
  return static_cast<uint8_t>(val);
}

// NCS core (single vertex)

void GTE::doNCS(CPUContext *ctx, uint8_t vecIdx, bool sf, bool lm) {
  // Light matrix × normal → IR
  doMVMVA(ctx, 1, vecIdx, 1, sf, lm); // L×V + BK
  // Light color matrix × IR → IR
  doMVMVA(ctx, 2, 3, 1, sf, lm); // LC×IR + BK
  // Color FIFO push
  uint8_t cd = static_cast<uint8_t>(ctx->cop2d[GTE_RGBC] >> 24);
  uint8_t r = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]) >> 4,
                         gte_flag::COLOR_R_SAT);
  uint8_t g = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC2]) >> 4,
                         gte_flag::COLOR_G_SAT);
  uint8_t b = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC3]) >> 4,
                         gte_flag::COLOR_B_SAT);
  pushRGB(ctx, r, g, b, cd);
}

// NCDS core

void GTE::doNCDS(CPUContext *ctx, uint8_t vecIdx, bool sf, bool lm) {
  doNCS(ctx, vecIdx, sf, lm);
  // Depth cue: interpolate between FC and IR based on IR0
  uint8_t cd = static_cast<uint8_t>(ctx->cop2d[GTE_RGBC] >> 24);
  int32_t ir0 = static_cast<int32_t>(static_cast<int16_t>(ctx->cop2d[GTE_IR0]));
  int64_t mac1 =
      (static_cast<int64_t>(static_cast<int32_t>(ctx->cop2c[GTE_RFC])) << 12) -
      (static_cast<int64_t>(static_cast<int16_t>(ctx->cop2d[GTE_IR1])) << 12);
  int64_t mac2 =
      (static_cast<int64_t>(static_cast<int32_t>(ctx->cop2c[GTE_GFC])) << 12) -
      (static_cast<int64_t>(static_cast<int16_t>(ctx->cop2d[GTE_IR2])) << 12);
  int64_t mac3 =
      (static_cast<int64_t>(static_cast<int32_t>(ctx->cop2c[GTE_BFC])) << 12) -
      (static_cast<int64_t>(static_cast<int16_t>(ctx->cop2d[GTE_IR3])) << 12);
  mac1 =
      (static_cast<int64_t>(static_cast<int16_t>(ctx->cop2d[GTE_IR1])) << 12) +
      ir0 * (mac1 >> 12);
  mac2 =
      (static_cast<int64_t>(static_cast<int16_t>(ctx->cop2d[GTE_IR2])) << 12) +
      ir0 * (mac2 >> 12);
  mac3 =
      (static_cast<int64_t>(static_cast<int16_t>(ctx->cop2d[GTE_IR3])) << 12) +
      ir0 * (mac3 >> 12);
  setMAC(ctx, mac1, mac2, mac3, sf);
  setIR(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC2]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC3]), lm);
  uint8_t r2 = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]) >> 4,
                          gte_flag::COLOR_R_SAT);
  uint8_t g2 = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC2]) >> 4,
                          gte_flag::COLOR_G_SAT);
  uint8_t b2 = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC3]) >> 4,
                          gte_flag::COLOR_B_SAT);
  pushRGB(ctx, r2, g2, b2, cd);
}

// NCCS core

void GTE::doNCCS(CPUContext *ctx, uint8_t vecIdx, bool sf, bool lm) {
  // Light matrix × normal → IR
  doMVMVA(ctx, 1, vecIdx, 1, sf, lm);
  // Color matrix × IR + BK → IR
  doMVMVA(ctx, 2, 3, 1, sf, lm);
  // Multiply by RGBC
  uint8_t rgbc_r = ctx->cop2d[GTE_RGBC] & 0xFF;
  uint8_t rgbc_g = (ctx->cop2d[GTE_RGBC] >> 8) & 0xFF;
  uint8_t rgbc_b = (ctx->cop2d[GTE_RGBC] >> 16) & 0xFF;
  uint8_t cd = static_cast<uint8_t>(ctx->cop2d[GTE_RGBC] >> 24);
  int64_t mac1 = static_cast<int64_t>(rgbc_r) *
                 static_cast<int16_t>(ctx->cop2d[GTE_IR1]) * 16;
  int64_t mac2 = static_cast<int64_t>(rgbc_g) *
                 static_cast<int16_t>(ctx->cop2d[GTE_IR2]) * 16;
  int64_t mac3 = static_cast<int64_t>(rgbc_b) *
                 static_cast<int16_t>(ctx->cop2d[GTE_IR3]) * 16;
  setMAC(ctx, mac1, mac2, mac3, sf);
  setIR(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC2]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC3]), lm);
  uint8_t r = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]) >> 4,
                         gte_flag::COLOR_R_SAT);
  uint8_t g = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC2]) >> 4,
                         gte_flag::COLOR_G_SAT);
  uint8_t b = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC3]) >> 4,
                         gte_flag::COLOR_B_SAT);
  pushRGB(ctx, r, g, b, cd);
}

// DPCS core

void GTE::doDPCS(CPUContext *ctx, bool sf, bool lm, uint8_t r, uint8_t g,
                 uint8_t b) {
  int32_t ir0 = static_cast<int32_t>(static_cast<int16_t>(ctx->cop2d[GTE_IR0]));
  int64_t mac1 =
      (static_cast<int64_t>(static_cast<int32_t>(ctx->cop2c[GTE_RFC])) << 12) -
      (static_cast<int64_t>(r) << 16);
  int64_t mac2 =
      (static_cast<int64_t>(static_cast<int32_t>(ctx->cop2c[GTE_GFC])) << 12) -
      (static_cast<int64_t>(g) << 16);
  int64_t mac3 =
      (static_cast<int64_t>(static_cast<int32_t>(ctx->cop2c[GTE_BFC])) << 12) -
      (static_cast<int64_t>(b) << 16);
  mac1 = (static_cast<int64_t>(r) << 16) + ir0 * (mac1 >> 12);
  mac2 = (static_cast<int64_t>(g) << 16) + ir0 * (mac2 >> 12);
  mac3 = (static_cast<int64_t>(b) << 16) + ir0 * (mac3 >> 12);
  setMAC(ctx, mac1, mac2, mac3, sf);
  setIR(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC2]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC3]), lm);
  uint8_t cd = static_cast<uint8_t>(ctx->cop2d[GTE_RGBC] >> 24);
  uint8_t cr = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]) >> 4,
                          gte_flag::COLOR_R_SAT);
  uint8_t cg = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC2]) >> 4,
                          gte_flag::COLOR_G_SAT);
  uint8_t cb = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC3]) >> 4,
                          gte_flag::COLOR_B_SAT);
  pushRGB(ctx, cr, cg, cb, cd);
}

// PUBLIC GTE COMMANDS (22 total)

void GTE::RTPS(CPUContext *ctx, bool sf, bool lm) {
  ctx->cop2c[GTE_FLAG] = 0;
  doRTPS(ctx, 0, sf, lm, true);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::RTPT(CPUContext *ctx, bool sf, bool lm) {
  ctx->cop2c[GTE_FLAG] = 0;
  doRTPS(ctx, 0, sf, lm, false);
  doRTPS(ctx, 1, sf, lm, false);
  doRTPS(ctx, 2, sf, lm, true);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::NCLIP(CPUContext *ctx, bool /*sf*/, bool /*lm*/) {
  ctx->cop2c[GTE_FLAG] = 0;
  int16_t sx0 = lo16(ctx->cop2d[GTE_SXY0]), sy0 = hi16(ctx->cop2d[GTE_SXY0]);
  int16_t sx1 = lo16(ctx->cop2d[GTE_SXY1]), sy1 = hi16(ctx->cop2d[GTE_SXY1]);
  int16_t sx2 = lo16(ctx->cop2d[GTE_SXY2]), sy2 = hi16(ctx->cop2d[GTE_SXY2]);
  int64_t mac0 = static_cast<int64_t>(sx0) * (sy1 - sy2) +
                 static_cast<int64_t>(sx1) * (sy2 - sy0) +
                 static_cast<int64_t>(sx2) * (sy0 - sy1);
  setMAC0(ctx, mac0);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::OP(CPUContext *ctx, bool sf, bool lm) {
  ctx->cop2c[GTE_FLAG] = 0;
  int16_t r11 = lo16(ctx->cop2c[GTE_RT11RT12]);
  int16_t r22 = lo16(ctx->cop2c[GTE_RT22RT23]);
  int16_t r33 = lo16(ctx->cop2c[GTE_RT33]);
  int16_t ir1 = static_cast<int16_t>(ctx->cop2d[GTE_IR1]);
  int16_t ir2 = static_cast<int16_t>(ctx->cop2d[GTE_IR2]);
  int16_t ir3 = static_cast<int16_t>(ctx->cop2d[GTE_IR3]);
  int64_t mac1 =
      static_cast<int64_t>(r22) * ir3 - static_cast<int64_t>(r33) * ir2;
  int64_t mac2 =
      static_cast<int64_t>(r33) * ir1 - static_cast<int64_t>(r11) * ir3;
  int64_t mac3 =
      static_cast<int64_t>(r11) * ir2 - static_cast<int64_t>(r22) * ir1;
  setMAC(ctx, mac1, mac2, mac3, sf);
  setIR(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC2]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC3]), lm);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::DPCS(CPUContext *ctx, bool sf, bool lm) {
  ctx->cop2c[GTE_FLAG] = 0;
  uint8_t r = ctx->cop2d[GTE_RGBC] & 0xFF;
  uint8_t g = (ctx->cop2d[GTE_RGBC] >> 8) & 0xFF;
  uint8_t b = (ctx->cop2d[GTE_RGBC] >> 16) & 0xFF;
  doDPCS(ctx, sf, lm, r, g, b);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::DPCT(CPUContext *ctx, bool sf, bool lm) {
  ctx->cop2c[GTE_FLAG] = 0;
  for (int i = 0; i < 3; i++) {
    uint8_t r = ctx->cop2d[GTE_RGB0] & 0xFF;
    uint8_t g = (ctx->cop2d[GTE_RGB0] >> 8) & 0xFF;
    uint8_t b = (ctx->cop2d[GTE_RGB0] >> 16) & 0xFF;
    doDPCS(ctx, sf, lm, r, g, b);
  }
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::INTPL(CPUContext *ctx, bool sf, bool lm) {
  ctx->cop2c[GTE_FLAG] = 0;
  int32_t ir0 = static_cast<int32_t>(static_cast<int16_t>(ctx->cop2d[GTE_IR0]));
  int16_t ir1 = static_cast<int16_t>(ctx->cop2d[GTE_IR1]);
  int16_t ir2 = static_cast<int16_t>(ctx->cop2d[GTE_IR2]);
  int16_t ir3 = static_cast<int16_t>(ctx->cop2d[GTE_IR3]);
  int64_t mac1 =
      (static_cast<int64_t>(static_cast<int32_t>(ctx->cop2c[GTE_RFC])) << 12) -
      (static_cast<int64_t>(ir1) << 12);
  int64_t mac2 =
      (static_cast<int64_t>(static_cast<int32_t>(ctx->cop2c[GTE_GFC])) << 12) -
      (static_cast<int64_t>(ir2) << 12);
  int64_t mac3 =
      (static_cast<int64_t>(static_cast<int32_t>(ctx->cop2c[GTE_BFC])) << 12) -
      (static_cast<int64_t>(ir3) << 12);
  mac1 = (static_cast<int64_t>(ir1) << 12) + ir0 * (mac1 >> 12);
  mac2 = (static_cast<int64_t>(ir2) << 12) + ir0 * (mac2 >> 12);
  mac3 = (static_cast<int64_t>(ir3) << 12) + ir0 * (mac3 >> 12);
  setMAC(ctx, mac1, mac2, mac3, sf);
  setIR(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC2]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC3]), lm);
  uint8_t cd = static_cast<uint8_t>(ctx->cop2d[GTE_RGBC] >> 24);
  uint8_t cr = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]) >> 4,
                          gte_flag::COLOR_R_SAT);
  uint8_t cg = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC2]) >> 4,
                          gte_flag::COLOR_G_SAT);
  uint8_t cb = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC3]) >> 4,
                          gte_flag::COLOR_B_SAT);
  pushRGB(ctx, cr, cg, cb, cd);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::MVMVA(CPUContext *ctx, uint8_t mx, uint8_t mv, uint8_t tv, bool sf,
                bool lm) {
  ctx->cop2c[GTE_FLAG] = 0;
  doMVMVA(ctx, mx, mv, tv, sf, lm);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::NCDS(CPUContext *ctx, bool sf, bool lm) {
  ctx->cop2c[GTE_FLAG] = 0;
  doNCDS(ctx, 0, sf, lm);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::NCDT(CPUContext *ctx, bool sf, bool lm) {
  ctx->cop2c[GTE_FLAG] = 0;
  doNCDS(ctx, 0, sf, lm);
  doNCDS(ctx, 1, sf, lm);
  doNCDS(ctx, 2, sf, lm);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::CDP(CPUContext *ctx, bool sf, bool lm) {
  ctx->cop2c[GTE_FLAG] = 0;
  // Color matrix × IR + BK
  doMVMVA(ctx, 2, 3, 1, sf, lm);
  // Depth cue interpolation
  int32_t ir0 = static_cast<int32_t>(static_cast<int16_t>(ctx->cop2d[GTE_IR0]));
  int64_t mac1 =
      (static_cast<int64_t>(static_cast<int32_t>(ctx->cop2c[GTE_RFC])) << 12) -
      (static_cast<int64_t>(static_cast<int16_t>(ctx->cop2d[GTE_IR1])) << 12);
  int64_t mac2 =
      (static_cast<int64_t>(static_cast<int32_t>(ctx->cop2c[GTE_GFC])) << 12) -
      (static_cast<int64_t>(static_cast<int16_t>(ctx->cop2d[GTE_IR2])) << 12);
  int64_t mac3 =
      (static_cast<int64_t>(static_cast<int32_t>(ctx->cop2c[GTE_BFC])) << 12) -
      (static_cast<int64_t>(static_cast<int16_t>(ctx->cop2d[GTE_IR3])) << 12);
  mac1 =
      (static_cast<int64_t>(static_cast<int16_t>(ctx->cop2d[GTE_IR1])) << 12) +
      ir0 * (mac1 >> 12);
  mac2 =
      (static_cast<int64_t>(static_cast<int16_t>(ctx->cop2d[GTE_IR2])) << 12) +
      ir0 * (mac2 >> 12);
  mac3 =
      (static_cast<int64_t>(static_cast<int16_t>(ctx->cop2d[GTE_IR3])) << 12) +
      ir0 * (mac3 >> 12);
  setMAC(ctx, mac1, mac2, mac3, sf);
  setIR(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC2]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC3]), lm);
  uint8_t cd = static_cast<uint8_t>(ctx->cop2d[GTE_RGBC] >> 24);
  uint8_t cr = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]) >> 4,
                          gte_flag::COLOR_R_SAT);
  uint8_t cg = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC2]) >> 4,
                          gte_flag::COLOR_G_SAT);
  uint8_t cb = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC3]) >> 4,
                          gte_flag::COLOR_B_SAT);
  pushRGB(ctx, cr, cg, cb, cd);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::NCCS(CPUContext *ctx, bool sf, bool lm) {
  ctx->cop2c[GTE_FLAG] = 0;
  doNCCS(ctx, 0, sf, lm);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::NCCT(CPUContext *ctx, bool sf, bool lm) {
  ctx->cop2c[GTE_FLAG] = 0;
  doNCCS(ctx, 0, sf, lm);
  doNCCS(ctx, 1, sf, lm);
  doNCCS(ctx, 2, sf, lm);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::CC(CPUContext *ctx, bool sf, bool lm) {
  ctx->cop2c[GTE_FLAG] = 0;
  // Light color matrix × IR + BK
  doMVMVA(ctx, 2, 3, 1, sf, lm);
  // Multiply by RGBC
  uint8_t rgbc_r = ctx->cop2d[GTE_RGBC] & 0xFF;
  uint8_t rgbc_g = (ctx->cop2d[GTE_RGBC] >> 8) & 0xFF;
  uint8_t rgbc_b = (ctx->cop2d[GTE_RGBC] >> 16) & 0xFF;
  uint8_t cd = static_cast<uint8_t>(ctx->cop2d[GTE_RGBC] >> 24);
  int64_t mac1 = static_cast<int64_t>(rgbc_r) *
                 static_cast<int16_t>(ctx->cop2d[GTE_IR1]) * 16;
  int64_t mac2 = static_cast<int64_t>(rgbc_g) *
                 static_cast<int16_t>(ctx->cop2d[GTE_IR2]) * 16;
  int64_t mac3 = static_cast<int64_t>(rgbc_b) *
                 static_cast<int16_t>(ctx->cop2d[GTE_IR3]) * 16;
  setMAC(ctx, mac1, mac2, mac3, sf);
  setIR(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC2]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC3]), lm);
  uint8_t r = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]) >> 4,
                         gte_flag::COLOR_R_SAT);
  uint8_t g = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC2]) >> 4,
                         gte_flag::COLOR_G_SAT);
  uint8_t b = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC3]) >> 4,
                         gte_flag::COLOR_B_SAT);
  pushRGB(ctx, r, g, b, cd);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::NCS(CPUContext *ctx, bool sf, bool lm) {
  ctx->cop2c[GTE_FLAG] = 0;
  doNCS(ctx, 0, sf, lm);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::NCT(CPUContext *ctx, bool sf, bool lm) {
  ctx->cop2c[GTE_FLAG] = 0;
  doNCS(ctx, 0, sf, lm);
  doNCS(ctx, 1, sf, lm);
  doNCS(ctx, 2, sf, lm);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::SQR(CPUContext *ctx, bool sf, bool lm) {
  ctx->cop2c[GTE_FLAG] = 0;
  int16_t ir1 = static_cast<int16_t>(ctx->cop2d[GTE_IR1]);
  int16_t ir2 = static_cast<int16_t>(ctx->cop2d[GTE_IR2]);
  int16_t ir3 = static_cast<int16_t>(ctx->cop2d[GTE_IR3]);
  int64_t mac1 = static_cast<int64_t>(ir1) * ir1;
  int64_t mac2 = static_cast<int64_t>(ir2) * ir2;
  int64_t mac3 = static_cast<int64_t>(ir3) * ir3;
  setMAC(ctx, mac1, mac2, mac3, sf);
  setIR(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC2]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC3]), lm);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::DCPL(CPUContext *ctx, bool sf, bool lm) {
  ctx->cop2c[GTE_FLAG] = 0;
  int32_t ir0 = static_cast<int32_t>(static_cast<int16_t>(ctx->cop2d[GTE_IR0]));
  uint8_t rgbc_r = ctx->cop2d[GTE_RGBC] & 0xFF;
  uint8_t rgbc_g = (ctx->cop2d[GTE_RGBC] >> 8) & 0xFF;
  uint8_t rgbc_b = (ctx->cop2d[GTE_RGBC] >> 16) & 0xFF;
  // Multiply color × IR
  int64_t cr =
      static_cast<int64_t>(rgbc_r) * static_cast<int16_t>(ctx->cop2d[GTE_IR1]);
  int64_t cg =
      static_cast<int64_t>(rgbc_g) * static_cast<int16_t>(ctx->cop2d[GTE_IR2]);
  int64_t cb =
      static_cast<int64_t>(rgbc_b) * static_cast<int16_t>(ctx->cop2d[GTE_IR3]);
  // Interpolate with far color
  int64_t mac1 =
      (static_cast<int64_t>(static_cast<int32_t>(ctx->cop2c[GTE_RFC])) << 12) -
      (cr << 4);
  int64_t mac2 =
      (static_cast<int64_t>(static_cast<int32_t>(ctx->cop2c[GTE_GFC])) << 12) -
      (cg << 4);
  int64_t mac3 =
      (static_cast<int64_t>(static_cast<int32_t>(ctx->cop2c[GTE_BFC])) << 12) -
      (cb << 4);
  mac1 = (cr << 4) + ir0 * (mac1 >> 12);
  mac2 = (cg << 4) + ir0 * (mac2 >> 12);
  mac3 = (cb << 4) + ir0 * (mac3 >> 12);
  setMAC(ctx, mac1, mac2, mac3, sf);
  setIR(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC2]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC3]), lm);
  uint8_t cd = static_cast<uint8_t>(ctx->cop2d[GTE_RGBC] >> 24);
  uint8_t r = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]) >> 4,
                         gte_flag::COLOR_R_SAT);
  uint8_t g = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC2]) >> 4,
                         gte_flag::COLOR_G_SAT);
  uint8_t b = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC3]) >> 4,
                         gte_flag::COLOR_B_SAT);
  pushRGB(ctx, r, g, b, cd);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::AVSZ3(CPUContext *ctx, bool /*sf*/, bool /*lm*/) {
  ctx->cop2c[GTE_FLAG] = 0;
  int16_t zsf3 = static_cast<int16_t>(ctx->cop2c[GTE_ZSF3]);
  int64_t mac0 =
      static_cast<int64_t>(zsf3) *
      (ctx->cop2d[GTE_SZ1] + ctx->cop2d[GTE_SZ2] + ctx->cop2d[GTE_SZ3]);
  setMAC0(ctx, mac0);
  int32_t otz = static_cast<int32_t>(mac0 >> 12);
  if (otz < 0) {
    otz = 0;
    ctx->cop2c[GTE_FLAG] |= gte_flag::SZ3_OTZ_SAT;
  }
  if (otz > 0xFFFF) {
    otz = 0xFFFF;
    ctx->cop2c[GTE_FLAG] |= gte_flag::SZ3_OTZ_SAT;
  }
  ctx->cop2d[GTE_OTZ] = static_cast<uint32_t>(otz);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::AVSZ4(CPUContext *ctx, bool /*sf*/, bool /*lm*/) {
  ctx->cop2c[GTE_FLAG] = 0;
  int16_t zsf4 = static_cast<int16_t>(ctx->cop2c[GTE_ZSF4]);
  int64_t mac0 =
      static_cast<int64_t>(zsf4) * (ctx->cop2d[GTE_SZ0] + ctx->cop2d[GTE_SZ1] +
                                    ctx->cop2d[GTE_SZ2] + ctx->cop2d[GTE_SZ3]);
  setMAC0(ctx, mac0);
  int32_t otz = static_cast<int32_t>(mac0 >> 12);
  if (otz < 0) {
    otz = 0;
    ctx->cop2c[GTE_FLAG] |= gte_flag::SZ3_OTZ_SAT;
  }
  if (otz > 0xFFFF) {
    otz = 0xFFFF;
    ctx->cop2c[GTE_FLAG] |= gte_flag::SZ3_OTZ_SAT;
  }
  ctx->cop2d[GTE_OTZ] = static_cast<uint32_t>(otz);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::GPF(CPUContext *ctx, bool sf, bool lm) {
  ctx->cop2c[GTE_FLAG] = 0;
  int16_t ir0 = static_cast<int16_t>(ctx->cop2d[GTE_IR0]);
  int16_t ir1 = static_cast<int16_t>(ctx->cop2d[GTE_IR1]);
  int16_t ir2 = static_cast<int16_t>(ctx->cop2d[GTE_IR2]);
  int16_t ir3 = static_cast<int16_t>(ctx->cop2d[GTE_IR3]);
  int64_t mac1 = static_cast<int64_t>(ir0) * ir1;
  int64_t mac2 = static_cast<int64_t>(ir0) * ir2;
  int64_t mac3 = static_cast<int64_t>(ir0) * ir3;
  setMAC(ctx, mac1, mac2, mac3, sf);
  setIR(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC2]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC3]), lm);
  uint8_t cd = static_cast<uint8_t>(ctx->cop2d[GTE_RGBC] >> 24);
  uint8_t r = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]) >> 4,
                         gte_flag::COLOR_R_SAT);
  uint8_t g = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC2]) >> 4,
                         gte_flag::COLOR_G_SAT);
  uint8_t b = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC3]) >> 4,
                         gte_flag::COLOR_B_SAT);
  pushRGB(ctx, r, g, b, cd);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::GPL(CPUContext *ctx, bool sf, bool lm) {
  ctx->cop2c[GTE_FLAG] = 0;
  int shift = sf ? 12 : 0;
  int16_t ir0 = static_cast<int16_t>(ctx->cop2d[GTE_IR0]);
  int16_t ir1 = static_cast<int16_t>(ctx->cop2d[GTE_IR1]);
  int16_t ir2 = static_cast<int16_t>(ctx->cop2d[GTE_IR2]);
  int16_t ir3 = static_cast<int16_t>(ctx->cop2d[GTE_IR3]);
  int64_t mac1 =
      (static_cast<int64_t>(static_cast<int32_t>(ctx->cop2d[GTE_MAC1]))
       << shift) +
      static_cast<int64_t>(ir0) * ir1;
  int64_t mac2 =
      (static_cast<int64_t>(static_cast<int32_t>(ctx->cop2d[GTE_MAC2]))
       << shift) +
      static_cast<int64_t>(ir0) * ir2;
  int64_t mac3 =
      (static_cast<int64_t>(static_cast<int32_t>(ctx->cop2d[GTE_MAC3]))
       << shift) +
      static_cast<int64_t>(ir0) * ir3;
  setMAC(ctx, mac1, mac2, mac3, sf);
  setIR(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC2]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC3]), lm);
  uint8_t cd = static_cast<uint8_t>(ctx->cop2d[GTE_RGBC] >> 24);
  uint8_t r = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]) >> 4,
                         gte_flag::COLOR_R_SAT);
  uint8_t g = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC2]) >> 4,
                         gte_flag::COLOR_G_SAT);
  uint8_t b = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC3]) >> 4,
                         gte_flag::COLOR_B_SAT);
  pushRGB(ctx, r, g, b, cd);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

} // namespace ps1
