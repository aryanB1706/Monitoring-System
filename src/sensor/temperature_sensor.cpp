#include "sensor/temperature_sensor.hpp"

#include <utility>

namespace sensor_monitor::sensor {

TemperatureSensor::TemperatureSensor(std::string id, unsigned int seed)
    : Sensor(std::move(id)),
      rng_(seed),
      normal_(60.0, 12.0),
      spike_(110.0, 150.0),
      spike_chance_(0.05) {}

double TemperatureSensor::readValue() {
  if (spike_chance_(rng_)) {
    return spike_(rng_);  // Injected outlier for anomaly-detection testing.
  }
  return normal_(rng_);
}

std::string TemperatureSensor::type() const { return "temperature"; }

std::string TemperatureSensor::id() const { return id_; }

}  // namespace sensor_monitor::sensor
