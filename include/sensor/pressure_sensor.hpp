#pragma once

#include <random>
#include <string>

#include "sensor/sensor.hpp"

namespace sensor_monitor::sensor {

// Simulated pressure sensor (bar nominal, occasional outliers).
class PressureSensor : public Sensor {
 public:
  explicit PressureSensor(std::string id,
                          unsigned int seed = std::random_device{}());

  double readValue() override;
  std::string type() const override;
  std::string id() const override;

 private:
  std::mt19937 rng_;
  std::normal_distribution<double> normal_;       // mean 5.0 bar, stddev 0.8
  std::uniform_real_distribution<double> high_;  // outlier high 9.5-12.0 bar
  std::uniform_real_distribution<double> low_;   // outlier low 0.1-0.8 bar
  std::bernoulli_distribution outlier_chance_;   // ~5% outliers
  std::bernoulli_distribution high_or_low_;      // 50/50 high vs low outlier
};

}  // namespace sensor_monitor::sensor
