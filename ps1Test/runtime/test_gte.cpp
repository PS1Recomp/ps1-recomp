// Tests for ps1Runtime — GTE (Geometry Transform Engine)
// Validates the 22 GTE commands with PS1-accurate fixed-point math

#include <cstring>
#include <gtest/gtest.h>
#include <runtime/gte.h>

using namespace ps1;

// Test Fixture

class GTETest : public ::testing::Test {
protected:
  CPUContext ctx;
  void SetUp() override { ctx.reset(); }

  // Helper to set rotation matrix (identity in 4.12 format)
  void setIdentityRotation() {
    // RT11=4096 RT12=0
    ctx.cop2c[GTE_RT11RT12] = static_cast<uint32_t>(4096) | (0u << 16);
    // RT13=0 RT21=0
    ctx.cop2c[GTE_RT13RT21] = 0;
    // RT22=4096 RT23=0
    ctx.cop2c[GTE_RT22RT23] = static_cast<uint32_t>(4096) | (0u << 16);
    // RT31=0 RT32=0
    ctx.cop2c[GTE_RT31RT32] = 0;
    // RT33=4096
    ctx.cop2c[GTE_RT33] = 4096;
  }

  void setTranslation(int32_t x, int32_t y, int32_t z) {
    ctx.cop2c[GTE_TRX] = static_cast<uint32_t>(x);
    ctx.cop2c[GTE_TRY] = static_cast<uint32_t>(y);
    ctx.cop2c[GTE_TRZ] = static_cast<uint32_t>(z);
  }

  void setV0(int16_t x, int16_t y, int16_t z) {
    ctx.cop2d[GTE_VXY0] =
        static_cast<uint32_t>(static_cast<uint16_t>(x)) |
        (static_cast<uint32_t>(static_cast<uint16_t>(y)) << 16);
    ctx.cop2d[GTE_VZ0] = static_cast<uint32_t>(static_cast<uint16_t>(z));
  }

  void setProjection(uint16_t h, int32_t ofx, int32_t ofy, int16_t dqa,
                     int32_t dqb) {
    ctx.cop2c[GTE_H] = h;
    ctx.cop2c[GTE_OFX] = static_cast<uint32_t>(ofx);
    ctx.cop2c[GTE_OFY] = static_cast<uint32_t>(ofy);
    ctx.cop2c[GTE_DQA] = static_cast<uint32_t>(static_cast<uint16_t>(dqa));
    ctx.cop2c[GTE_DQB] = static_cast<uint32_t>(dqb);
  }
};

// NCLIP

TEST_F(GTETest, NCLIP_CounterClockwise) {
  // Triangle: (0,0) (100,0) (0,100) → positive area
  ctx.cop2d[GTE_SXY0] = 0;                                // (0,0)
  ctx.cop2d[GTE_SXY1] = 100;                              // (100,0)
  ctx.cop2d[GTE_SXY2] = static_cast<uint32_t>(100) << 16; // (0,100)
  GTE::NCLIP(&ctx, true, true);
  int32_t mac0 = static_cast<int32_t>(ctx.cop2d[GTE_MAC0]);
  EXPECT_GT(mac0, 0); // Counter-clockwise → positive
}

TEST_F(GTETest, NCLIP_Clockwise) {
  // Reversed triangle: (0,0) (0,100) (100,0) → negative
  ctx.cop2d[GTE_SXY0] = 0;
  ctx.cop2d[GTE_SXY1] = static_cast<uint32_t>(100) << 16;
  ctx.cop2d[GTE_SXY2] = 100;
  GTE::NCLIP(&ctx, true, true);
  int32_t mac0 = static_cast<int32_t>(ctx.cop2d[GTE_MAC0]);
  EXPECT_LT(mac0, 0); // Clockwise → negative
}

// SQR

TEST_F(GTETest, SQR_Squares) {
  ctx.cop2d[GTE_IR1] = static_cast<uint32_t>(static_cast<int16_t>(100));
  ctx.cop2d[GTE_IR2] = static_cast<uint32_t>(static_cast<int16_t>(-50));
  ctx.cop2d[GTE_IR3] = static_cast<uint32_t>(static_cast<int16_t>(200));
  GTE::SQR(&ctx, false, false);
  EXPECT_EQ(static_cast<int32_t>(ctx.cop2d[GTE_MAC1]), 10000);
  EXPECT_EQ(static_cast<int32_t>(ctx.cop2d[GTE_MAC2]), 2500);
  EXPECT_EQ(static_cast<int32_t>(ctx.cop2d[GTE_MAC3]), 40000);
}

// AVSZ3 / AVSZ4

TEST_F(GTETest, AVSZ3_AverageZ) {
  ctx.cop2d[GTE_SZ1] = 100;
  ctx.cop2d[GTE_SZ2] = 200;
  ctx.cop2d[GTE_SZ3] = 300;
  // ZSF3 = 4096/3 ≈ 1365 in fixed 1.3.12
  ctx.cop2c[GTE_ZSF3] = static_cast<uint32_t>(static_cast<int16_t>(1365));
  GTE::AVSZ3(&ctx, true, true);
  // MAC0 = 1365 * (100+200+300) = 819000
  // OTZ = MAC0 >> 12 = 819000 >> 12 = 199
  EXPECT_EQ(ctx.cop2d[GTE_OTZ], 199u);
}

TEST_F(GTETest, AVSZ4_AverageZ) {
  ctx.cop2d[GTE_SZ0] = 100;
  ctx.cop2d[GTE_SZ1] = 200;
  ctx.cop2d[GTE_SZ2] = 300;
  ctx.cop2d[GTE_SZ3] = 400;
  // ZSF4 = 4096/4 = 1024
  ctx.cop2c[GTE_ZSF4] = static_cast<uint32_t>(static_cast<int16_t>(1024));
  GTE::AVSZ4(&ctx, true, true);
  // MAC0 = 1024 * 1000 = 1024000
  // OTZ = 1024000 >> 12 = 250
  EXPECT_EQ(ctx.cop2d[GTE_OTZ], 250u);
}

// OP (Outer Product)

TEST_F(GTETest, OP_CrossProduct) {
  // Rotation diagonal: R11=100, R22=200, R33=300
  ctx.cop2c[GTE_RT11RT12] = static_cast<uint32_t>(static_cast<uint16_t>(100));
  ctx.cop2c[GTE_RT22RT23] = static_cast<uint32_t>(static_cast<uint16_t>(200));
  ctx.cop2c[GTE_RT33] = static_cast<uint32_t>(static_cast<uint16_t>(300));
  // IR = (10, 20, 30)
  ctx.cop2d[GTE_IR1] = 10;
  ctx.cop2d[GTE_IR2] = 20;
  ctx.cop2d[GTE_IR3] = 30;
  GTE::OP(&ctx, false, false);
  // MAC1 = R22*IR3 - R33*IR2 = 200*30 - 300*20 = 6000-6000 = 0
  EXPECT_EQ(static_cast<int32_t>(ctx.cop2d[GTE_MAC1]), 0);
  // MAC2 = R33*IR1 - R11*IR3 = 300*10 - 100*30 = 3000-3000 = 0
  EXPECT_EQ(static_cast<int32_t>(ctx.cop2d[GTE_MAC2]), 0);
}

// GPF (General Purpose Interpolation)

TEST_F(GTETest, GPF_Multiply) {
  ctx.cop2d[GTE_IR0] =
      static_cast<uint32_t>(static_cast<int16_t>(4096)); // 1.0 in 4.12
  ctx.cop2d[GTE_IR1] = static_cast<uint32_t>(static_cast<int16_t>(100));
  ctx.cop2d[GTE_IR2] = static_cast<uint32_t>(static_cast<int16_t>(200));
  ctx.cop2d[GTE_IR3] = static_cast<uint32_t>(static_cast<int16_t>(300));
  GTE::GPF(&ctx, true, false); // sf=1 → shift 12
  // MAC1 = 4096 * 100 = 409600 >> 12 = 100
  EXPECT_EQ(static_cast<int32_t>(ctx.cop2d[GTE_MAC1]), 100);
  EXPECT_EQ(static_cast<int32_t>(ctx.cop2d[GTE_MAC2]), 200);
  EXPECT_EQ(static_cast<int32_t>(ctx.cop2d[GTE_MAC3]), 300);
}

// MVMVA

TEST_F(GTETest, MVMVA_Identity) {
  setIdentityRotation();
  setTranslation(0, 0, 0);
  setV0(100, 200, 300);
  GTE::MVMVA(&ctx, 0, 0, 0, true, false);
  // Identity rotation × (100,200,300) + (0,0,0) = (100,200,300)
  EXPECT_EQ(static_cast<int32_t>(ctx.cop2d[GTE_MAC1]), 100);
  EXPECT_EQ(static_cast<int32_t>(ctx.cop2d[GTE_MAC2]), 200);
  EXPECT_EQ(static_cast<int32_t>(ctx.cop2d[GTE_MAC3]), 300);
}

TEST_F(GTETest, MVMVA_WithTranslation) {
  setIdentityRotation();
  setTranslation(1000, 2000, 3000);
  setV0(100, 200, 300);
  GTE::MVMVA(&ctx, 0, 0, 0, true, false);
  // Identity × V0 + TR = (100+1000, 200+2000, 300+3000)
  EXPECT_EQ(static_cast<int32_t>(ctx.cop2d[GTE_MAC1]), 1100);
  EXPECT_EQ(static_cast<int32_t>(ctx.cop2d[GTE_MAC2]), 2200);
  EXPECT_EQ(static_cast<int32_t>(ctx.cop2d[GTE_MAC3]), 3300);
}

// FLAG register

TEST_F(GTETest, FlagClearOnCommand) {
  ctx.cop2c[GTE_FLAG] = 0xFFFFFFFF;
  ctx.cop2d[GTE_IR1] = 1;
  ctx.cop2d[GTE_IR2] = 1;
  ctx.cop2d[GTE_IR3] = 1;
  GTE::SQR(&ctx, false, false);
  // FLAG should have been reset at start (no overflow from 1*1)
  EXPECT_EQ(ctx.cop2c[GTE_FLAG] & 0x7F87E000, 0u);
}

// Register access

TEST_F(GTETest, LZCR_PositiveValue) {
  GTE::writeData(&ctx, GTE_LZCS, 0x00010000); // bit 16 set
  uint32_t lzcr = GTE::readData(&ctx, GTE_LZCR);
  EXPECT_EQ(lzcr, 15u); // 15 leading zeros before bit 16
}

TEST_F(GTETest, IRGB_ReadWrite) {
  GTE::writeData(&ctx, GTE_IRGB, 0x001F);    // R=31
  EXPECT_EQ(ctx.cop2d[GTE_IR1], 31u * 0x80); // 31 * 128 = 3968
}
