#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>

#include "detector/detector.hpp"
#include "detector/latency.hpp"
#include "detector/zscore_detector.hpp"
#include "sensor/reading.hpp"

using sensor_monitor::detector::AnomalyDetector;
using sensor_monitor::detector::Detection;
using sensor_monitor::detector::DetectorConfig;
using sensor_monitor::detector::LatencyStats;
using sensor_monitor::detector::Severity;
using sensor_monitor::detector::ZScoreDetector;
using sensor_monitor::sensor::SensorReading;

namespace {

SensorReading makeReading(const std::string& id, double value) {
  SensorReading r;
  r.id = id;
  r.type = "temperature";
  r.value = value;
  r.timestamp = std::chrono::system_clock::now();
  return r;
}

// Feed an alternating 10/11 history: mean 10.5, stddev 0.5.
void feedHistory(ZScoreDetector& d, const std::string& id, int n = 50) {
  for (int i = 0; i < n; ++i) {
    d.detect(makeReading(id, (i % 2 == 0) ? 10.0 : 11.0));
  }
}

}  // namespace

TEST(DetectorTest, StrategyIsPolymorphic) {
  std::unique_ptr<AnomalyDetector> d =
      std::make_unique<ZScoreDetector>();
  Detection det = d->detect(makeReading("s-0", 42.0));
  EXPECT_FALSE(det.is_anomaly);  // still warming up
}

TEST(DetectorTest, WarmupSuppressesAnomalies) {
  DetectorConfig cfg;
  cfg.min_samples = 10;
  ZScoreDetector d(cfg);
  for (int i = 0; i < 9; ++i) {
    Detection det = d.detect(makeReading("s-0", 1000.0 + i));
    EXPECT_FALSE(det.is_anomaly);
  }
}

TEST(DetectorTest, SpikeIsCriticalAnomaly) {
  ZScoreDetector d;
  feedHistory(d, "s-0");
  Detection det = d.detect(makeReading("s-0", 13.0));  // z = +5.0
  EXPECT_TRUE(det.is_anomaly);
  EXPECT_EQ(det.severity, Severity::kCritical);
  EXPECT_NEAR(det.z_score, 5.0, 1e-9);
  EXPECT_NEAR(det.mean, 10.5, 1e-9);
  EXPECT_NEAR(det.stddev, 0.5, 1e-9);
}

TEST(DetectorTest, ModerateDeviationIsWarning) {
  ZScoreDetector d;
  feedHistory(d, "s-0");
  Detection det = d.detect(makeReading("s-0", 12.0));  // z = +3.0
  EXPECT_TRUE(det.is_anomaly);
  EXPECT_EQ(det.severity, Severity::kWarning);
  EXPECT_NEAR(det.z_score, 3.0, 1e-9);
}

TEST(DetectorTest, NormalValueIsNotAnomaly) {
  ZScoreDetector d;
  feedHistory(d, "s-0");
  Detection det = d.detect(makeReading("s-0", 10.6));
  EXPECT_FALSE(det.is_anomaly);
  EXPECT_EQ(det.severity, Severity::kOk);
}

TEST(DetectorTest, WindowsArePerSensorAndCapped) {
  DetectorConfig cfg;
  cfg.window_size = 5;
  cfg.min_samples = 2;
  ZScoreDetector d(cfg);
  for (int i = 0; i < 20; ++i) {
    d.detect(makeReading("a", 10.0 + (i % 2)));
    d.detect(makeReading("b", 50.0));
  }
  EXPECT_EQ(d.trackedSensors(), 2u);
  EXPECT_EQ(d.windowSizeFor("a"), 5u);
  EXPECT_EQ(d.windowSizeFor("b"), 5u);
  EXPECT_EQ(d.windowSizeFor("missing"), 0u);
}

TEST(DetectorTest, DefaultWindowHoldsLast100) {
  ZScoreDetector d;
  for (int i = 0; i < 250; ++i) {
    d.detect(makeReading("s-0", 10.0 + (i % 2)));
  }
  EXPECT_EQ(d.windowSizeFor("s-0"), 100u);
}

TEST(DetectorTest, CustomThresholdsAreHonored) {
  DetectorConfig cfg;
  cfg.anomaly_threshold = 2.0;
  cfg.warning_threshold = 2.0;
  cfg.critical_threshold = 3.0;
  ZScoreDetector d(cfg);
  feedHistory(d, "s-0");
  Detection warn = d.detect(makeReading("s-0", 11.75));  // z = 2.5
  EXPECT_TRUE(warn.is_anomaly);
  EXPECT_EQ(warn.severity, Severity::kWarning);
}

TEST(DetectorTest, InvalidConfigThrows) {
  DetectorConfig bad;
  bad.window_size = 0;
  // NOTE: braces + named variable — bare `ZScoreDetector(bad)` parses as a
  // shadowing declaration, not a throwing construction.
  EXPECT_THROW({ ZScoreDetector d(bad); }, std::invalid_argument);

  DetectorConfig bad2;
  bad2.critical_threshold = 2.0;  // < warning 3.0
  EXPECT_THROW({ ZScoreDetector d(bad2); }, std::invalid_argument);
}

TEST(DetectorTest, ConfigLoadsFromFile) {
  const std::string path = "/tmp/sensor_monitor_detector_test.conf";
  {
    std::ofstream out(path);
    out << "# test config\n";
    out << "window_size = 50\n";
    out << "anomaly_threshold = 2.5\n";
    out << "warning_threshold = 2.5\n";
    out << "critical_threshold = 5.0\n";
    out << "min_samples = 5\n";
  }
  DetectorConfig cfg = DetectorConfig::loadFromFile(path);
  EXPECT_EQ(cfg.window_size, 50u);
  EXPECT_DOUBLE_EQ(cfg.anomaly_threshold, 2.5);
  EXPECT_DOUBLE_EQ(cfg.warning_threshold, 2.5);
  EXPECT_DOUBLE_EQ(cfg.critical_threshold, 5.0);
  EXPECT_EQ(cfg.min_samples, 5u);
  std::remove(path.c_str());

  EXPECT_THROW(DetectorConfig::loadFromFile("/tmp/does-not-exist-xyz.conf"),
               std::runtime_error);
}

TEST(DetectorTest, DetectLatencyIsUnder5ms) {
  ZScoreDetector d;
  feedHistory(d, "s-0");
  Detection det = d.detect(makeReading("s-0", 10.5));
  EXPECT_LT(det.detect_latency, std::chrono::milliseconds(5));
  // End-to-end for a fresh reading must also be well under 5ms.
  EXPECT_LT(det.e2e_latency, std::chrono::milliseconds(5));
}

TEST(DetectorTest, LatencyStatsPercentiles) {
  LatencyStats s;
  EXPECT_EQ(s.count(), 0u);
  EXPECT_EQ(s.percentile(50), 0);
  for (int i = 1; i <= 100; ++i) {
    s.add(std::chrono::nanoseconds(i));
  }
  EXPECT_EQ(s.count(), 100u);
  EXPECT_EQ(s.percentile(50), 51);  // rank = 0.5*99 = 49.5 -> idx 50 -> 51
  EXPECT_EQ(s.percentile(0), 1);
  EXPECT_EQ(s.percentile(100), 100);
  EXPECT_GT(s.mean(), 0.0);
}

TEST(DetectorTest, RollingWindowEvictsOldest) {
  // window=5: after [10,10,20,20,20,20] the window must be [10,20,20,20,20]
  // (mean 18, stddev 4) — not the un-evicted mean of 70/6 ≈ 16.7.
  DetectorConfig cfg;
  cfg.window_size = 5;
  cfg.min_samples = 2;
  ZScoreDetector d(cfg);
  for (int i = 0; i < 2; ++i) d.detect(makeReading("s-0", 10.0));
  for (int i = 0; i < 4; ++i) d.detect(makeReading("s-0", 20.0));
  EXPECT_EQ(d.windowSizeFor("s-0"), 5u);
  Detection probe = d.detect(makeReading("s-0", 30.0));  // z=(30-18)/4=3
  EXPECT_NEAR(probe.mean, 18.0, 1e-9);
  EXPECT_NEAR(probe.stddev, 4.0, 1e-9);
  EXPECT_TRUE(probe.is_anomaly);
  EXPECT_EQ(probe.severity, Severity::kWarning);
}

TEST(DetectorTest, ZeroVarianceNeverSpikes) {
  // Constant history => stddev 0 => z is forced to 0 (no div-by-zero, no
  // false anomaly even for a far-away value).
  ZScoreDetector d;
  for (int i = 0; i < 50; ++i) d.detect(makeReading("s-0", 10.0));
  Detection same = d.detect(makeReading("s-0", 10.0));
  EXPECT_FALSE(same.is_anomaly);
  Detection far = d.detect(makeReading("s-0", 500.0));
  EXPECT_FALSE(far.is_anomaly);
  EXPECT_DOUBLE_EQ(far.z_score, 0.0);
}

TEST(DetectorTest, MinSamplesBoundaryIsExact) {
  DetectorConfig cfg;
  cfg.min_samples = 10;
  ZScoreDetector d(cfg);
  // Varying history (stddev 0.5) so the probe scores a huge |z|.
  for (int i = 0; i < 10; ++i) {
    d.detect(makeReading("s-0", 1000.0 + (i % 2)));  // builds window to 10
  }
  // 11th reading judged against 10 warm samples: huge deviation => anomaly.
  Detection det = d.detect(makeReading("s-0", 10.0));
  EXPECT_TRUE(det.is_anomaly);
}

TEST(DetectorTest, NegativeSpikeIsCritical) {
  ZScoreDetector d;
  feedHistory(d, "s-0");
  Detection det = d.detect(makeReading("s-0", 8.0));  // z = -5.0
  EXPECT_TRUE(det.is_anomaly);
  EXPECT_EQ(det.severity, Severity::kCritical);
  EXPECT_NEAR(det.z_score, -5.0, 1e-9);
}

TEST(DetectorTest, SeverityBoundaries) {
  // Each probe uses a fresh history: scoring inserts into the window, so
  // reusing one detector across probes would shift the mean/stddev.
  {
    ZScoreDetector d;
    feedHistory(d, "s-0");
    // z = (11.5-10.5)/0.5 = 2.0 < 3.0 => OK.
    Detection ok = d.detect(makeReading("s-0", 11.5));
    EXPECT_FALSE(ok.is_anomaly);
    EXPECT_EQ(ok.severity, Severity::kOk);
  }
  {
    ZScoreDetector d;
    feedHistory(d, "s-0");
    // z = 3.0 is the anomaly/warning edge (inclusive).
    Detection warn = d.detect(makeReading("s-0", 12.0));
    EXPECT_TRUE(warn.is_anomaly);
    EXPECT_EQ(warn.severity, Severity::kWarning);
  }
  {
    ZScoreDetector d;
    feedHistory(d, "s-0");
    // z = (12.5-10.5)/0.5 = 4.0 is the critical edge (inclusive).
    Detection crit = d.detect(makeReading("s-0", 12.5));
    EXPECT_TRUE(crit.is_anomaly);
    EXPECT_EQ(crit.severity, Severity::kCritical);
  }
}

TEST(DetectorTest, ResetClearsState) {
  ZScoreDetector d;
  feedHistory(d, "s-0");
  EXPECT_EQ(d.trackedSensors(), 1u);
  d.reset();
  EXPECT_EQ(d.trackedSensors(), 0u);
  EXPECT_EQ(d.windowSizeFor("s-0"), 0u);
  // After reset the detector is warming up again: spike suppressed.
  Detection det = d.detect(makeReading("s-0", 13.0));
  EXPECT_FALSE(det.is_anomaly);
}

TEST(DetectorTest, PerSensorIsolation) {
  ZScoreDetector d;
  feedHistory(d, "a");  // only "a" has history
  // "b" is still warming up: same spike value must NOT flag.
  Detection other = d.detect(makeReading("b", 13.0));
  EXPECT_FALSE(other.is_anomaly);
  EXPECT_EQ(other.severity, Severity::kOk);
  // "a" with real history flags the identical value.
  Detection self = d.detect(makeReading("a", 13.0));
  EXPECT_TRUE(self.is_anomaly);
}

TEST(DetectorTest, DetectsWithFewerThan100Samples) {
  // Default window is 100 but min_samples is 10: 20 samples is enough.
  ZScoreDetector d;
  feedHistory(d, "s-0", 20);
  EXPECT_EQ(d.windowSizeFor("s-0"), 20u);
  Detection det = d.detect(makeReading("s-0", 13.0));
  EXPECT_TRUE(det.is_anomaly);
  EXPECT_EQ(det.severity, Severity::kCritical);
}

TEST(DetectorTest, SeverityToString) {
  EXPECT_EQ(toString(Severity::kOk), "OK");
  EXPECT_EQ(toString(Severity::kWarning), "WARNING");
  EXPECT_EQ(toString(Severity::kCritical), "CRITICAL");
}

TEST(DetectorTest, ConcurrentDetectIsThreadSafe) {
  ZScoreDetector d;
  constexpr int kThreads = 4;
  constexpr int kPerThread = 50;
  std::atomic<bool> ok{true};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      const std::string id = "s-" + std::to_string(t);
      for (int i = 0; i < kPerThread; ++i) {
        SensorReading r = makeReading(id, 10.0 + (i % 2));
        Detection det = d.detect(r);
        if (det.detect_latency.count() < 0) ok.store(false);
        if (det.e2e_latency.count() < 0) ok.store(false);
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_TRUE(ok.load());
  EXPECT_EQ(d.trackedSensors(), static_cast<std::size_t>(kThreads));
  for (int t = 0; t < kThreads; ++t) {
    EXPECT_EQ(d.windowSizeFor("s-" + std::to_string(t)),
              static_cast<std::size_t>(kPerThread));
  }
}
