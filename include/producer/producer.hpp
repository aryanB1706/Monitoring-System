#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include "queue/bounded_queue.hpp"
#include "sensor/reading.hpp"
#include "sensor/sensor.hpp"

namespace sensor_monitor::producer {

// One producer thread owning a single Sensor, pushing timestamped
// SensorReadings into a shared BoundedQueue.
//
// rate_per_sec <= 0 means as-fast-as-possible (no sleep, for benchmarks).
// Otherwise the producer sleeps 1/rate_per_sec between readings.
//
// NOTE: stop() only sets a flag; a producer blocked in push() still needs
// the consumer to drain (or queue shutdown) to unblock. Teardown order:
// keep the consumer running -> stop producers -> shutdown queue.
class SensorProducer {
 public:
  SensorProducer(std::unique_ptr<sensor::Sensor> sensor,
                 queue::BoundedQueue<sensor::SensorReading>& queue,
                 double rate_per_sec = 100.0);

  ~SensorProducer();

  SensorProducer(const SensorProducer&) = delete;
  SensorProducer& operator=(const SensorProducer&) = delete;

  void start();
  void stop();

  std::uint64_t produced() const noexcept;
  bool running() const noexcept;

 private:
  void run();

  std::unique_ptr<sensor::Sensor> sensor_;
  queue::BoundedQueue<sensor::SensorReading>& queue_;
  double rate_per_sec_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> running_{false};
  std::atomic<std::uint64_t> produced_{0};
};

}  // namespace sensor_monitor::producer
