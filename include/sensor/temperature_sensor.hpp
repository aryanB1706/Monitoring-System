#pragma once

#include <random>
#include <string>

#include "sensor/sensor.hpp"

namespace sensor_monitor::sensor {

// Simulated temperature sensor (~20-100 C nominal, occasional spikes).
class TemperatureSensor : public Sensor {
 public:
  explicit TemperatureSensor(std::string id,
                             unsigned int seed = std::random_device{}());

  double readValue() override;
  std::string type() const override;
  std::string id() const override;

 private:
  std::mt19937 rng_;
  std::normal_distribution<double> normal_;    // mean 60 C, stddev 12
  std::uniform_real_distribution<double> spike_;  // outlier 110-150 C
  std::bernoulli_distribution spike_chance_;  // ~5% outliers
};

}  // namespace sensor_monitor::sensor
