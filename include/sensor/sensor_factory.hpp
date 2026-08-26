#pragma once

#include <memory>
#include <vector>

#include "sensor/sensor.hpp"

namespace sensor_monitor::sensor {

// Factory that spins up a mixed set of sensors with unique IDs.
class SensorFactory {
 public:
  // Creates 12 sensors (4 of each type) with unique IDs.
  static std::vector<std::unique_ptr<Sensor>> createDefaultSet();

  // Creates `count` sensors, round-robin across the 3 types.
  // `seed` seeds per-sensor RNGs deterministically (seed + index).
  static std::vector<std::unique_ptr<Sensor>> create(std::size_t count = 12,
                                                     unsigned int seed = 42);
};

}  // namespace sensor_monitor::sensor
