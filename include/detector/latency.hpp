#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace sensor_monitor::detector {

// Thread-safe latency accumulator with p50/p99/max/mean reporting.
class LatencyStats {
 public:
  void add(std::chrono::nanoseconds sample) {
    std::lock_guard<std::mutex> lock(mutex_);
    samples_.push_back(sample.count());
  }

  std::size_t count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return samples_.size();
  }

  // Percentile in nanoseconds (p in [0,100]). Returns 0 if empty.
  std::int64_t percentile(double p) const {
    std::vector<std::int64_t> sorted;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (samples_.empty()) return 0;
      sorted = samples_;
    }
    std::sort(sorted.begin(), sorted.end());
    if (p <= 0.0) return sorted.front();
    if (p >= 100.0) return sorted.back();
    const double rank = (p / 100.0) * (sorted.size() - 1);
    return sorted[static_cast<std::size_t>(rank + 0.5)];
  }

  std::int64_t max() const { return percentile(100.0); }

  double mean() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (samples_.empty()) return 0.0;
    long double sum = 0;
    for (auto s : samples_) sum += s;
    return static_cast<double>(sum / samples_.size());
  }

 private:
  mutable std::mutex mutex_;
  std::vector<std::int64_t> samples_;
};

inline double toMillis(std::chrono::nanoseconds ns) {
  return std::chrono::duration<double, std::milli>(ns).count();
}

}  // namespace sensor_monitor::detector
