#pragma once

#include <random>
#include <string>

#include "sensor/sensor.hpp"

namespace sensor_monitor::sensor {

// Simulated vibration sensor (mm/s nominal, occasional outliers).
class VibrationSensor : public Sensor {
 public:
  explicit VibrationSensor(std::string id,
                           unsigned int seed = std::random_device{}());

  double readValue() override;
  std::string type() const override;
  std::string id() const override;

 private:
  std::mt19937 rng_;
  std::normal_distribution<double> normal_;    // mean 3.5 mm/s, stddev 1.0
  std::uniform_real_distribution<double> spike_;  // outlier 10-18 mm/s
  std::bernoulli_distribution spike_chance_;  // ~5% outliers
};

}  // namespace sensor_monitor::sensor
