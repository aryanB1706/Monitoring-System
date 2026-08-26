#include "sensor/vibration_sensor.hpp"

#include <utility>

namespace sensor_monitor::sensor {

VibrationSensor::VibrationSensor(std::string id, unsigned int seed)
    : Sensor(std::move(id)),
      rng_(seed),
      normal_(3.5, 1.0),
      spike_(10.0, 18.0),
      spike_chance_(0.05) {}

double VibrationSensor::readValue() {
  if (spike_chance_(rng_)) {
    return spike_(rng_);  // Injected outlier for anomaly-detection testing.
  }
  double v = normal_(rng_);
  return v < 0.0 ? 0.0 : v;  // mm/s can't be negative.
}

std::string VibrationSensor::type() const { return "vibration"; }

std::string VibrationSensor::id() const { return id_; }

}  // namespace sensor_monitor::sensor
