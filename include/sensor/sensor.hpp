#pragma once

#include <string>

namespace sensor_monitor::sensor {

// Abstract polymorphic sensor interface.
class Sensor {
 public:
  explicit Sensor(std::string id);
  virtual ~Sensor() = default;

  virtual double readValue() = 0;
  virtual std::string type() const = 0;
  virtual std::string id() const = 0;

 protected:
  std::string id_;
};

}  // namespace sensor_monitor::sensor
