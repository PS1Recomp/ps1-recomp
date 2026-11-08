#include "runtime/metrics.h"

#include "runtime/gpu/gpu.h"

#include <cstdio>
#include <fstream>
#include <gtest/gtest.h>
#include <cstdlib>
#include <sstream>
#include <string>
#include <unistd.h>

namespace {

// Unique per process and call, so parallel ctest runs never share a file.
std::string uniqueJsonPath(const char *tag) {
  static int counter = 0;
  return "/tmp/ps1_metrics_" + std::to_string(::getpid()) + "_" +
         std::to_string(++counter) + "_" + tag + ".json";
}

std::string slurp(const std::string &path) {
  std::ifstream in(path);
  std::stringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

class MetricsTest : public ::testing::Test {
protected:
  void SetUp() override {
    ps1::metrics::reset();
    ps1::metrics::setEnabledForTesting(true);
  }
  void TearDown() override {
    ps1::metrics::reset();
    ps1::metrics::setEnabledForTesting(false);
  }
};

TEST_F(MetricsTest, CountAccumulatesAcrossCalls) {
  ps1::metrics::count("gp0.op.7C");
  ps1::metrics::count("gp0.op.7C", 5);
  EXPECT_EQ(ps1::metrics::get("gp0.op.7C"), 6u);
}

TEST_F(MetricsTest, CountIsUncappedPastLegacyLogLimits) {
  // The bug this whole effort exists for: the old logs stopped at 10 or 20.
  for (int i = 0; i < 5000; ++i)
    ps1::metrics::count("fill_rect");
  EXPECT_EQ(ps1::metrics::get("fill_rect"), 5000u);
}

TEST_F(MetricsTest, UnknownCounterReadsZero) {
  EXPECT_EQ(ps1::metrics::get("never.touched"), 0u);
}

TEST_F(MetricsTest, SetStateKeepsMostRecentValue) {
  ps1::metrics::setState("display.x", 0);
  ps1::metrics::setState("display.x", 512);
  EXPECT_EQ(ps1::metrics::getState("display.x"), 512);
}

TEST_F(MetricsTest, DisabledCollectorRecordsNothing) {
  ps1::metrics::setEnabledForTesting(false);
  ps1::metrics::count("ignored");
  EXPECT_EQ(ps1::metrics::get("ignored"), 0u);
}

TEST_F(MetricsTest, DumpJsonWritesCountersAndState) {
  ps1::metrics::count("frames", 42);
  ps1::metrics::setState("display.y", -7);

  const std::string pathStr = uniqueJsonPath("dump");
  const char *path = pathStr.c_str();
  ASSERT_TRUE(ps1::metrics::dumpJsonTo(path));

  std::ifstream in(path);
  ASSERT_TRUE(in.good());
  std::stringstream buf;
  buf << in.rdbuf();
  const std::string body = buf.str();

  EXPECT_NE(body.find("\"frames\": 42"), std::string::npos) << body;
  EXPECT_NE(body.find("\"display.y\": -7"), std::string::npos) << body;
  std::remove(path);
}

// main_host.cpp reaches GPU::publishMetrics() from two shutdown paths -- the
// normal join and the forced `_Exit` taken whenever the recompiled game thread
// is still spinning -- and count() accumulates.  Publishing the whole
// histogram on both paths doubled every reported gp0.op.XX and gp0.words.
TEST_F(MetricsTest, GpuPublishMetricsDoesNotDoubleOnSecondShutdownPath) {
  ps1::gpu::GPU gpu;
  gpu.writeGP0(0xE1000000u); // draw mode
  gpu.writeGP0(0xE1000000u);
  gpu.writeGP0(0xE6000000u); // mask bit setting

  gpu.publishMetrics();
  ASSERT_EQ(ps1::metrics::get("gp0.op.E1"), 2u);
  ASSERT_EQ(ps1::metrics::get("gp0.op.E6"), 1u);
  ASSERT_EQ(ps1::metrics::get("gp0.words"), 3u);

  gpu.publishMetrics();
  EXPECT_EQ(ps1::metrics::get("gp0.op.E1"), 2u);
  EXPECT_EQ(ps1::metrics::get("gp0.op.E6"), 1u);
  EXPECT_EQ(ps1::metrics::get("gp0.words"), 3u);
}

// ...but the second call is not a plain no-op: the game thread keeps issuing
// GP0 words during main_host's 2s join deadline, and the forced-exit report
// has to include them so gp0.* stays consistent with the live counters
// (cpu_to_vram etc.) written in that same window.
TEST_F(MetricsTest, GpuPublishMetricsReportsTrafficBetweenCalls) {
  ps1::gpu::GPU gpu;
  gpu.writeGP0(0xE1000000u);
  gpu.publishMetrics();
  ASSERT_EQ(ps1::metrics::get("gp0.op.E1"), 1u);

  gpu.writeGP0(0xE1000000u);
  gpu.publishMetrics();
  EXPECT_EQ(ps1::metrics::get("gp0.op.E1"), 2u);
  EXPECT_EQ(ps1::metrics::get("gp0.words"), 2u);
}

} // namespace

TEST_F(MetricsTest, DumpJsonToANullPathFails) {
  EXPECT_FALSE(ps1::metrics::dumpJsonTo(nullptr));
}

TEST_F(MetricsTest, DumpJsonToAnUnwritablePathFails) {
  EXPECT_FALSE(ps1::metrics::dumpJsonTo("/nonexistent_dir_ps1_test/m.json"));
}

TEST_F(MetricsTest, AnEmptyCollectorDumpsEmptySections) {
  const std::string path = uniqueJsonPath("empty");

  ASSERT_TRUE(ps1::metrics::dumpJsonTo(path.c_str()));

  const std::string body = slurp(path);
  EXPECT_NE(body.find("\"counters\": {  }"), std::string::npos) << body;
  EXPECT_NE(body.find("\"state\": {  }"), std::string::npos) << body;
  std::remove(path.c_str());
}

TEST_F(MetricsTest, SeveralEntriesAreCommaSeparatedInNameOrder) {
  ps1::metrics::count("b", 2);
  ps1::metrics::count("a", 1);
  ps1::metrics::setState("y", 4);
  ps1::metrics::setState("x", 3);
  const std::string path = uniqueJsonPath("many");

  ASSERT_TRUE(ps1::metrics::dumpJsonTo(path.c_str()));

  const std::string body = slurp(path);
  EXPECT_NE(body.find("\"a\": 1,\n    \"b\": 2\n"), std::string::npos) << body;
  EXPECT_NE(body.find("\"x\": 3,\n    \"y\": 4\n"), std::string::npos) << body;
  std::remove(path.c_str());
}

TEST_F(MetricsTest, DisabledCollectorKeepsNoState) {
  ps1::metrics::setEnabledForTesting(false);

  ps1::metrics::setState("ignored", 9);

  EXPECT_EQ(ps1::metrics::getState("ignored"), 0);
}

TEST_F(MetricsTest, UnknownStateReadsZero) {
  EXPECT_EQ(ps1::metrics::getState("never.set"), 0);
}

TEST_F(MetricsTest, DumpJsonDoesNothingWhileDisabled) {
  ps1::metrics::setEnabledForTesting(false);

  EXPECT_FALSE(ps1::metrics::dumpJson());
}

TEST_F(MetricsTest, DumpJsonWritesToThePathNamedByPs1Metrics) {
  const std::string path = uniqueJsonPath("env");
  ps1::metrics::count("frames", 3);
  ::setenv("PS1_METRICS", path.c_str(), 1);

  const bool ok = ps1::metrics::dumpJson();
  ::unsetenv("PS1_METRICS");

  EXPECT_TRUE(ok);
  EXPECT_NE(slurp(path).find("\"frames\": 3"), std::string::npos);
  std::remove(path.c_str());
}
