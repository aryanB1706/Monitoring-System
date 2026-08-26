#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

#include "detector/detector.hpp"

namespace sensor_monitor::detector {

// Z-score anomaly detector with a per-sensor rolling window.
//
// z = (value - rolling_mean) / rolling_stddev computed against the window
// *before* inserting the new value. Severity from DetectorConfig thresholds.
class ZScoreDetector : public AnomalyDetector {
 public:
  explicit ZScoreDetector(DetectorConfig config = DetectorConfig{});

  Detection detect(const sensor::SensorReading& reading) override;

  void reset();
  std::size_t trackedSensors() const;
  std::size_t windowSizeFor(const std::string& sensor_id) const;

 private:
  struct Window {
    std::deque<double> values;
    double sum = 0.0;
    double sumsq = 0.0;
  };

  DetectorConfig config_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, Window> windows_;
};

}  // namespace sensor_monitor::detector
