#pragma once

#include <chrono>
#include <cstddef>
#include <string>

#include "sensor/reading.hpp"

namespace sensor_monitor::detector {

enum class Severity { kOk, kWarning, kCritical };

std::string toString(Severity s);

struct DetectorConfig {
  std::size_t window_size = 100;     // per-sensor rolling window
  double anomaly_threshold = 3.0;    // |z| above this = anomaly
  double warning_threshold = 3.0;    // |z| in [warning, critical) = WARNING
  double critical_threshold = 4.0;   // |z| >= critical = CRITICAL
  std::size_t min_samples = 10;      // warm-up readings before detecting

  void validate() const;

  // Simple key=value file (lines: window_size, anomaly_threshold,
  // warning_threshold, critical_threshold, min_samples; '#' comments).
  // Missing keys keep defaults.
  static DetectorConfig loadFromFile(const std::string& path);
};

struct Detection {
  std::string sensor_id;
  std::string sensor_type;
  double value = 0.0;
  bool is_anomaly = false;
  double z_score = 0.0;
  double mean = 0.0;
  double stddev = 0.0;
  Severity severity = Severity::kOk;
  // Compute-only time inside detect().
  std::chrono::nanoseconds detect_latency{0};
  // End-to-end: reading.timestamp (push time) -> alert raised (post-detect).
  std::chrono::nanoseconds e2e_latency{0};
};

// Pluggable strategy interface: swap implementations without touching
// the consumer path.
class AnomalyDetector {
 public:
  virtual ~AnomalyDetector() = default;
  virtual Detection detect(const sensor::SensorReading& reading) = 0;
};

}  // namespace sensor_monitor::detector
