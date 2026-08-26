#include "sensor/pressure_sensor.hpp"

#include <utility>

namespace sensor_monitor::sensor {

PressureSensor::PressureSensor(std::string id, unsigned int seed)
    : Sensor(std::move(id)),
      rng_(seed),
      normal_(5.0, 0.8),
      high_(9.5, 12.0),
      low_(0.1, 0.8),
      outlier_chance_(0.05),
      high_or_low_(0.5) {}

double PressureSensor::readValue() {
  if (outlier_chance_(rng_)) {
    // Injected outlier for anomaly-detection testing.
    return high_or_low_(rng_) ? high_(rng_) : low_(rng_);
  }
  return normal_(rng_);
}

std::string PressureSensor::type() const { return "pressure"; }

std::string PressureSensor::id() const { return id_; }

}  // namespace sensor_monitor::sensor
