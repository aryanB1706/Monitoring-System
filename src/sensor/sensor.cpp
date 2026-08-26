#include "sensor/sensor.hpp"

#include <utility>

namespace sensor_monitor::sensor {

Sensor::Sensor(std::string id) : id_(std::move(id)) {}

}  // namespace sensor_monitor::sensor
