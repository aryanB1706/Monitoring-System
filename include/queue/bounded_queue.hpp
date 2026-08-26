#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>
#include <stdexcept>

namespace sensor_monitor::queue {

// Bounded blocking queue with backpressure + graceful shutdown.
//
// - push() blocks while full, returns false if shutdown.
// - pop() blocks while empty, returns false if shutdown AND empty
//   (remaining items can still be drained after shutdown).
// - shutdown() wakes all waiters; subsequent push() fails, pop() drains
//   then fails.
// - size() is thread-safe.
template <typename T>
class BoundedQueue {
 public:
  explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
    if (capacity_ == 0) {
      throw std::invalid_argument("BoundedQueue capacity must be > 0");
    }
  }

  BoundedQueue(const BoundedQueue&) = delete;
  BoundedQueue& operator=(const BoundedQueue&) = delete;

  ~BoundedQueue() { shutdown(); }

  // Blocking push (copy). Returns false if shutdown.
  bool push(const T& item) {
    std::unique_lock<std::mutex> lock(mutex_);
    not_full_.wait(lock,
                   [this] { return shutdown_ || queue_.size() < capacity_; });
    if (shutdown_) {
      return false;
    }
    queue_.push(item);
    lock.unlock();
    not_empty_.notify_one();
    return true;
  }

  // Blocking push (move). Returns false if shutdown.
  bool push(T&& item) {
    std::unique_lock<std::mutex> lock(mutex_);
    not_full_.wait(lock,
                   [this] { return shutdown_ || queue_.size() < capacity_; });
    if (shutdown_) {
      return false;
    }
    queue_.push(std::move(item));
    lock.unlock();
    not_empty_.notify_one();
    return true;
  }

  // Blocking pop. Returns false if shutdown AND empty.
  bool pop(T& out) {
    std::unique_lock<std::mutex> lock(mutex_);
    not_empty_.wait(lock, [this] { return shutdown_ || !queue_.empty(); });
    if (queue_.empty()) {
      return false;  // shutdown_ must be true here.
    }
    out = std::move(queue_.front());
    queue_.pop();
    lock.unlock();
    not_full_.notify_one();
    return true;
  }

  // Bulk drain: blocks until >= 1 item is available or shutdown.
  // Moves up to max_items into `out` under a single lock acquisition,
  // which amortizes mutex/futex cost for high-throughput consumers.
  // Returns items moved; 0 means shutdown AND empty.
  // Wakes all producers blocked in push() (queue has free space after).
  template <typename Container>
  std::size_t drain_to(Container& out, std::size_t max_items) {
    std::unique_lock<std::mutex> lock(mutex_);
    not_empty_.wait(lock, [this] { return shutdown_ || !queue_.empty(); });
    if (queue_.empty()) {
      return 0;  // shutdown_ must be true here.
    }
    std::size_t n = 0;
    while (!queue_.empty() && n < max_items) {
      out.push_back(std::move(queue_.front()));
      queue_.pop();
      ++n;
    }
    lock.unlock();
    not_full_.notify_all();
    return n;
  }

  // Wakes all blocked push()/pop() calls. Idempotent.
  void shutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (shutdown_) {
        return;
      }
      shutdown_ = true;
    }
    not_full_.notify_all();
    not_empty_.notify_all();
  }

  std::size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

  std::size_t capacity() const noexcept { return capacity_; }

  bool empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
  }

  bool is_shutdown() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return shutdown_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable not_full_;
  std::condition_variable not_empty_;
  std::queue<T> queue_;
  std::size_t capacity_;
  bool shutdown_ = false;
};

}  // namespace sensor_monitor::queue
