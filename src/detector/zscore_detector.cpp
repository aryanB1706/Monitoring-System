#include "detector/zscore_detector.hpp"

#include <chrono>
#include <cmath>

namespace sensor_monitor::detector {

ZScoreDetector::ZScoreDetector(DetectorConfig config)
    : config_(config) {
  config_.validate();
}

Detection ZScoreDetector::detect(const sensor::SensorReading& reading) {
  const auto t_start = std::chrono::steady_clock::now();

  Detection out;
  out.sensor_id = reading.id;
  out.sensor_type = reading.type;
  out.value = reading.value;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    Window& w = windows_[reading.id];

    if (w.values.size() >= config_.min_samples && w.values.size() >= 2) {
      const double n = static_cast<double>(w.values.size());
      const double mean = w.sum / n;
      double var = (w.sumsq - w.sum * w.sum / n) / n;
      if (var < 0.0) var = 0.0;  // guard against float rounding
      const double stddev = std::sqrt(var);

      out.mean = mean;
      out.stddev = stddev;

      double z = 0.0;
      if (stddev > 1e-9) {
        z = (reading.value - mean) / stddev;
      }
      out.z_score = z;

      const double az = std::fabs(z);
      if (az >= config_.anomaly_threshold) {
        out.is_anomaly = true;
        out.severity = (az >= config_.critical_threshold)
                           ? Severity::kCritical
                           : Severity::kWarning;
      }
    }

    // Insert AFTER scoring so a reading is judged against history.
    w.values.push_back(reading.value);
    w.sum += reading.value;
    w.sumsq += reading.value * reading.value;
    if (w.values.size() > config_.window_size) {
      const double oldest = w.values.front();
      w.values.pop_front();
      w.sum -= oldest;
      w.sumsq -= oldest * oldest;
    }
  }

  const auto t_end = std::chrono::steady_clock::now();
  out.detect_latency =
      std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start);

  // End-to-end: pushed (producer timestamp) -> alert raised (now).
  const auto now_sys = std::chrono::system_clock::now();
  auto e2e = now_sys - reading.timestamp;
  if (e2e.count() < 0) e2e = decltype(e2e)::zero();
  out.e2e_latency =
      std::chrono::duration_cast<std::chrono::nanoseconds>(e2e);

  return out;
}

void ZScoreDetector::reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  windows_.clear();
}

std::size_t ZScoreDetector::trackedSensors() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return windows_.size();
}

std::size_t ZScoreDetector::windowSizeFor(const std::string& sensor_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = windows_.find(sensor_id);
  return it == windows_.end() ? 0 : it->second.values.size();
}

}  // namespace sensor_monitor::detector
