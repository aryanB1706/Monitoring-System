#include "producer/producer.hpp"

#include <chrono>

namespace sensor_monitor::producer {

SensorProducer::SensorProducer(
    std::unique_ptr<sensor::Sensor> sensor,
    queue::BoundedQueue<sensor::SensorReading>& queue, double rate_per_sec)
    : sensor_(std::move(sensor)),
      queue_(queue),
      rate_per_sec_(rate_per_sec) {}

SensorProducer::~SensorProducer() { stop(); }

void SensorProducer::start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return;  // already running
  }
  stop_.store(false);
  thread_ = std::thread(&SensorProducer::run, this);
}

void SensorProducer::stop() {
  stop_.store(true);
  if (thread_.joinable()) {
    thread_.join();
  }
  running_.store(false);
}

std::uint64_t SensorProducer::produced() const noexcept {
  return produced_.load();
}

bool SensorProducer::running() const noexcept { return running_.load(); }

void SensorProducer::run() {
  // Cache id/type once to avoid virtual calls + allocs per reading.
  const std::string id = sensor_->id();
  const std::string type = sensor_->type();

  std::chrono::nanoseconds interval{0};
  const bool paced = rate_per_sec_ > 0.0;
  if (paced) {
    interval = std::chrono::nanoseconds(
        static_cast<long long>(1e9 / rate_per_sec_));
  }

  while (!stop_.load(std::memory_order_relaxed)) {
    sensor::SensorReading reading;
    reading.id = id;
    reading.type = type;
    reading.value = sensor_->readValue();
    reading.timestamp = std::chrono::system_clock::now();

    if (!queue_.push(std::move(reading))) {
      break;  // queue shutdown
    }
    ++produced_;

    if (paced) {
      std::this_thread::sleep_for(interval);
    }
  }
  running_.store(false);
}

}  // namespace sensor_monitor::producer
