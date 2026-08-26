#pragma once

#include <chrono>
#include <string>

#include "detector/detector.hpp"

namespace sensor_monitor::worker {

// Severity-tagged alert raised when the detector flags a reading.
// Produced by the consumer pool, delivered to an AlertSink.
struct Alert {
  std::string sensor_id;
  std::string sensor_type;
  double value = 0.0;
  double z_score = 0.0;
  double mean = 0.0;
  double stddev = 0.0;
  detector::Severity severity = detector::Severity::kOk;
  std::chrono::system_clock::time_point timestamp{};
  std::chrono::nanoseconds e2e_latency{0};

  static Alert fromDetection(const detector::Detection& d) {
    Alert a;
    a.sensor_id = d.sensor_id;
    a.sensor_type = d.sensor_type;
    a.value = d.value;
    a.z_score = d.z_score;
    a.mean = d.mean;
    a.stddev = d.stddev;
    a.severity = d.severity;
    a.timestamp = std::chrono::system_clock::now();
    a.e2e_latency = d.e2e_latency;
    return a;
  }

  std::string toString() const {
    return "ALERT [" + detector::toString(severity) + "] sensor=" +
           sensor_id + " (" + sensor_type + ") value=" +
           std::to_string(value) + " z=" + std::to_string(z_score);
  }
};

}  // namespace sensor_monitor::worker
