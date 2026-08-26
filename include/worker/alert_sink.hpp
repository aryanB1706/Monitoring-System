#pragma once

#include <iostream>
#include <mutex>
#include <vector>

#include "worker/alert.hpp"

namespace sensor_monitor::worker {

// Pluggable sink for Alerts. Console logger is the default implementation;
// swap in another AlertSink (file, network, test double) without touching
// the consumer pool.
class AlertSink {
 public:
  virtual ~AlertSink() = default;
  virtual void emit(const Alert& alert) = 0;
};

// Thread-safe console logger. One line per alert on the wrapped stream.
class ConsoleAlertSink : public AlertSink {
 public:
  explicit ConsoleAlertSink(std::ostream& os = std::cout) : os_(os) {}

  void emit(const Alert& alert) override {
    std::lock_guard<std::mutex> lock(mutex_);
    os_ << alert.toString() << "\n";
  }

 private:
  std::ostream& os_;
  std::mutex mutex_;
};

// Discards all alerts. Useful for benchmarks where console I/O would
// dominate throughput, and as a null object in tests.
class NullAlertSink : public AlertSink {
 public:
  void emit(const Alert&) override {}
};

// In-memory sink for tests: records every alert for later assertions.
class VectorAlertSink : public AlertSink {
 public:
  void emit(const Alert& alert) override {
    std::lock_guard<std::mutex> lock(mutex_);
    alerts_.push_back(alert);
  }

  std::vector<Alert> snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return alerts_;
  }

  std::size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return alerts_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::vector<Alert> alerts_;
};

}  // namespace sensor_monitor::worker
