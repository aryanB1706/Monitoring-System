#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "detector/detector.hpp"
#include "detector/latency.hpp"
#include "queue/bounded_queue.hpp"
#include "sensor/reading.hpp"
#include "worker/alert.hpp"
#include "worker/alert_sink.hpp"

namespace sensor_monitor::worker {

// Legacy scaffold stub. Kept default-constructible for backwards
// compatibility; new code should use WorkerPool below.
class Worker {
 public:
  Worker();
  ~Worker();

  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;
};

// 4-worker (default) consumer thread pool.
//
// Each worker bulk-drains SensorReadings from a shared BoundedQueue, runs
// them through an AnomalyDetector, and raises severity-tagged Alerts to a
// pluggable AlertSink (console logger by default).
//
// Shutdown protocol (zero-drop):
//   1. Stop producers so no new readings are pushed.
//   2. Call queue.shutdown() — blocked drain_to()/pop() wake up, but
//      remaining items can still be drained.
//   3. Call pool.stop() — workers exit only after the queue is empty AND
//      shutdown() was called (drain_to returns 0), so every pushed reading
//      is consumed exactly once.
//   4. Assert produced == consumed (see main.cpp).
//
// Thread-safety: requires AnomalyDetector and AlertSink to be thread-safe
// (ZScoreDetector, LatencyStats and the bundled sinks are).
class WorkerPool {
 public:
  static constexpr std::size_t kDefaultWorkers = 4;

  WorkerPool(queue::BoundedQueue<sensor::SensorReading>& queue,
             detector::AnomalyDetector& detector, AlertSink& sink,
             std::size_t num_workers = kDefaultWorkers,
             std::size_t batch_size = 64);

  ~WorkerPool();

  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;

  // Launch worker threads. Idempotent.
  void start();
  // Join worker threads. Caller must have called queue.shutdown() first,
  // otherwise workers block forever in drain_to(). Idempotent.
  void stop();

  bool running() const noexcept;

  std::uint64_t consumed() const noexcept;
  std::uint64_t anomalies() const noexcept;

  const detector::LatencyStats& detect_stats() const noexcept;
  const detector::LatencyStats& e2e_stats() const noexcept;

  // Steady-state gate for benchmarks: when false, detect still runs and
  // counters/alerts still fire, but latency samples are skipped (excludes
  // spawn/teardown transients). Defaults to true.
  void set_measuring(bool v) noexcept;

  std::size_t num_workers() const noexcept;

 private:
  void run();

  queue::BoundedQueue<sensor::SensorReading>& queue_;
  detector::AnomalyDetector& detector_;
  AlertSink& sink_;
  std::size_t num_workers_;
  std::size_t batch_size_;

  std::vector<std::thread> threads_;
  std::atomic<bool> running_{false};
  std::atomic<bool> measuring_{true};
  std::atomic<std::uint64_t> consumed_{0};
  std::atomic<std::uint64_t> anomalies_{0};
  detector::LatencyStats detect_stats_;
  detector::LatencyStats e2e_stats_;
};

}  // namespace sensor_monitor::worker
