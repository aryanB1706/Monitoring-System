#pragma once

#include <chrono>
#include <string>

namespace sensor_monitor::sensor {

// Timestamped reading produced by a Sensor.
struct SensorReading {
  std::string id;
  std::string type;
  double value = 0.0;
  std::chrono::system_clock::time_point timestamp{};
};

}  // namespace sensor_monitor::sensor
