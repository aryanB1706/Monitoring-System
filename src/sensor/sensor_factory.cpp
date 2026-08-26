#include "sensor/sensor_factory.hpp"

#include <string>

#include "sensor/pressure_sensor.hpp"
#include "sensor/temperature_sensor.hpp"
#include "sensor/vibration_sensor.hpp"

namespace sensor_monitor::sensor {

std::vector<std::unique_ptr<Sensor>> SensorFactory::createDefaultSet() {
  return create(12, 42);
}

std::vector<std::unique_ptr<Sensor>> SensorFactory::create(std::size_t count,
                                                           unsigned int seed) {
  std::vector<std::unique_ptr<Sensor>> sensors;
  sensors.reserve(count);

  std::size_t temp_n = 0;
  std::size_t press_n = 0;
  std::size_t vib_n = 0;

  for (std::size_t i = 0; i < count; ++i) {
    const unsigned int sensor_seed = seed + static_cast<unsigned int>(i);
    switch (i % 3) {
      case 0:
        sensors.push_back(std::make_unique<TemperatureSensor>(
            "temp-" + std::to_string(temp_n++), sensor_seed));
        break;
      case 1:
        sensors.push_back(std::make_unique<PressureSensor>(
            "press-" + std::to_string(press_n++), sensor_seed));
        break;
      default:
        sensors.push_back(std::make_unique<VibrationSensor>(
            "vib-" + std::to_string(vib_n++), sensor_seed));
        break;
    }
  }
  return sensors;
}

}  // namespace sensor_monitor::sensor
