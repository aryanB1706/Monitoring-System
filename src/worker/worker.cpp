#include "worker/worker.hpp"

#include <stdexcept>

namespace sensor_monitor::worker {

Worker::Worker() = default;
Worker::~Worker() = default;

WorkerPool::WorkerPool(
    queue::BoundedQueue<sensor::SensorReading>& queue,
    detector::AnomalyDetector& detector, AlertSink& sink,
    std::size_t num_workers, std::size_t batch_size)
    : queue_(queue),
      detector_(detector),
      sink_(sink),
      num_workers_(num_workers),
      batch_size_(batch_size) {
  if (num_workers_ == 0) {
    throw std::invalid_argument("WorkerPool: num_workers must be > 0");
  }
  if (batch_size_ == 0) {
    throw std::invalid_argument("WorkerPool: batch_size must be > 0");
  }
}

WorkerPool::~WorkerPool() { stop(); }

void WorkerPool::start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return;  // already running
  }
  threads_.reserve(num_workers_);
  for (std::size_t i = 0; i < num_workers_; ++i) {
    threads_.emplace_back(&WorkerPool::run, this);
  }
}

void WorkerPool::stop() {
  // Join all threads. Workers exit only when drain_to() returns 0, i.e.
  // queue.shutdown() was called AND the queue is fully drained.
  for (auto& t : threads_) {
    if (t.joinable()) {
      t.join();
    }
  }
  threads_.clear();
  running_.store(false);
}

bool WorkerPool::running() const noexcept { return running_.load(); }

std::uint64_t WorkerPool::consumed() const noexcept {
  return consumed_.load();
}

std::uint64_t WorkerPool::anomalies() const noexcept {
  return anomalies_.load();
}

const detector::LatencyStats& WorkerPool::detect_stats() const noexcept {
  return detect_stats_;
}

const detector::LatencyStats& WorkerPool::e2e_stats() const noexcept {
  return e2e_stats_;
}

void WorkerPool::set_measuring(bool v) noexcept {
  measuring_.store(v, std::memory_order_relaxed);
}

std::size_t WorkerPool::num_workers() const noexcept { return num_workers_; }

void WorkerPool::run() {
  // Bulk drain amortizes queue lock cost; detect runs on local batch.
  std::vector<sensor::SensorReading> batch;
  batch.reserve(batch_size_);
  while (queue_.drain_to(batch, batch_size_) > 0) {
    for (auto& r : batch) {
      detector::Detection res = detector_.detect(r);
      if (measuring_.load(std::memory_order_relaxed)) {
        detect_stats_.add(res.detect_latency);
        e2e_stats_.add(res.e2e_latency);
      }
      ++consumed_;
      if (res.is_anomaly) {
        ++anomalies_;
        sink_.emit(Alert::fromDetection(res));
      }
    }
    batch.clear();
  }
}

}  // namespace sensor_monitor::worker
