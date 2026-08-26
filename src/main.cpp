#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "detector/detector.hpp"
#include "detector/latency.hpp"
#include "detector/zscore_detector.hpp"
#include "producer/producer.hpp"
#include "queue/bounded_queue.hpp"
#include "sensor/reading.hpp"
#include "sensor/sensor_factory.hpp"
#include "worker/alert_sink.hpp"
#include "worker/worker.hpp"

namespace {

// Graceful-shutdown stop flag. Set on SIGINT/SIGTERM so the run loop ends
// early but still goes through the orderly teardown below.
std::atomic<bool> g_stop_requested{false};

void on_signal(int) { g_stop_requested.store(true); }

void install_signal_handlers() {
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
}

bool stop_requested() { return g_stop_requested.load(); }

// Sleep in small slices so SIGINT ends the run promptly instead of waiting
// out the full duration.
void sleep_interruptible(double seconds) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
  while (std::chrono::steady_clock::now() < deadline) {
    if (stop_requested()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

constexpr std::chrono::nanoseconds kLatencyBudget{5'000'000};  // 5ms

struct Options {
  bool benchmark = false;
  double seconds = 3.0;         // run duration
  double rate_per_sec = 100.0;  // per-sensor rate (normal mode only)
  std::size_t queue_size = 0;   // 0 = auto (1024 normal, 128 benchmark)
  int consumers = 0;            // 0 = auto (4 workers)
  std::string detector_config;  // optional key=value config file path
};

void print_usage(const char* prog) {
  std::cout << "Usage: " << prog << " [--benchmark] [--seconds N] "
            << "[--rate R] [--queue-size Q] [--consumers C] "
               "[--detector-config PATH]\n"
            << "  --benchmark    run 12 producers as-fast-as-possible for N "
               "seconds\n"
            << "  --seconds N    run duration in seconds (default 3)\n"
            << "  --rate R       per-sensor readings/sec in normal mode "
               "(default 100, <=0 = unlimited)\n"
            << "  --queue-size Q queue capacity (default 1024, 128 for "
               "--benchmark)\n"
            << "  --consumers C  detector consumer threads (default 4)\n"
            << "  --detector-config PATH  key=value detector thresholds file\n"
            << "  SIGINT/SIGTERM triggers graceful shutdown (drain + "
               "zero-drop check).\n";
}

Options parse_args(int argc, char* argv[]) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--benchmark") {
      opt.benchmark = true;
    } else if (a == "--seconds" && i + 1 < argc) {
      opt.seconds = std::stod(argv[++i]);
    } else if (a == "--rate" && i + 1 < argc) {
      opt.rate_per_sec = std::stod(argv[++i]);
    } else if (a == "--queue-size" && i + 1 < argc) {
      opt.queue_size = static_cast<std::size_t>(std::stoul(argv[++i]));
    } else if (a == "--consumers" && i + 1 < argc) {
      opt.consumers = std::stoi(argv[++i]);
    } else if (a == "--detector-config" && i + 1 < argc) {
      opt.detector_config = argv[++i];
    } else if (a == "--help" || a == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    } else {
      std::cerr << "Unknown arg: " << a << "\n";
      print_usage(argv[0]);
      std::exit(1);
    }
  }
  if (opt.queue_size == 0) {
    opt.queue_size = opt.benchmark ? 128 : 1024;
  }
  if (opt.consumers <= 0) {
    opt.consumers = static_cast<int>(
        sensor_monitor::worker::WorkerPool::kDefaultWorkers);
  }
  if (opt.consumers < 1) opt.consumers = 1;
  return opt;
}

sensor_monitor::detector::DetectorConfig loadConfig(const Options& opt) {
  using sensor_monitor::detector::DetectorConfig;
  if (!opt.detector_config.empty()) {
    return DetectorConfig::loadFromFile(opt.detector_config);
  }
  return DetectorConfig{};
}

void printLatency(const char* name,
                  const sensor_monitor::detector::LatencyStats& stats) {
  using sensor_monitor::detector::toMillis;
  std::cout << name << ": count=" << stats.count()
            << " p50=" << toMillis(std::chrono::nanoseconds(stats.percentile(50))) << "ms"
            << " p99=" << toMillis(std::chrono::nanoseconds(stats.percentile(99))) << "ms"
            << " max=" << toMillis(std::chrono::nanoseconds(stats.max())) << "ms"
            << " mean=" << toMillis(std::chrono::nanoseconds(
                             static_cast<std::int64_t>(stats.mean()))) << "ms\n";
}

// Returns non-zero if the p99 e2e budget is violated.
int checkBudget(const sensor_monitor::detector::LatencyStats& e2e) {
  if (e2e.count() == 0) return 0;
  const auto p99 = std::chrono::nanoseconds(e2e.percentile(99));
  if (p99 > kLatencyBudget) {
    std::cerr << "WARNING: p99 end-to-end latency "
              << sensor_monitor::detector::toMillis(p99)
              << "ms exceeds 5ms budget\n";
    return 3;
  }
  return 0;
}

// Zero-drop assertion: every pushed reading must be consumed exactly once.
// Returns 0 on success, 1 on mismatch (and logs to stderr).
int verifyNoDrop(std::uint64_t produced, std::uint64_t consumed) {
  if (produced != consumed) {
    std::cerr << "ERROR: dropped readings: produced=" << produced
              << " consumed=" << consumed
              << " lost=" << (produced - consumed) << "\n";
    return 1;
  }
  std::cout << "zero-drop check OK: produced == consumed == " << produced
            << "\n";
  return 0;
}

// Normal demo: 12 paced producers + 4-worker consumer pool with console
// alert sink.
int run_normal(const Options& opt) {
  using sensor_monitor::detector::ZScoreDetector;
  using sensor_monitor::producer::SensorProducer;
  using sensor_monitor::queue::BoundedQueue;
  using sensor_monitor::sensor::SensorFactory;
  using sensor_monitor::sensor::SensorReading;
  using sensor_monitor::worker::ConsoleAlertSink;
  using sensor_monitor::worker::WorkerPool;

  g_stop_requested.store(false);

  BoundedQueue<SensorReading> queue(opt.queue_size);
  auto sensors = SensorFactory::createDefaultSet();
  ZScoreDetector detector(loadConfig(opt));
  ConsoleAlertSink sink;  // pluggable: swap for file/network sink later

  std::vector<std::unique_ptr<SensorProducer>> producers;
  producers.reserve(sensors.size());
  for (auto& s : sensors) {
    producers.push_back(std::make_unique<SensorProducer>(
        std::move(s), queue, opt.rate_per_sec));
  }

  WorkerPool pool(queue, detector, sink,
                  static_cast<std::size_t>(opt.consumers));
  pool.start();

  for (auto& p : producers) p->start();
  sleep_interruptible(opt.seconds);  // returns early on SIGINT/SIGTERM
  // Graceful shutdown: stop producers first (no new pushes), then shut the
  // queue down so workers drain everything and exit only on empty+shutdown.
  for (auto& p : producers) p->stop();
  queue.shutdown();
  pool.stop();

  std::uint64_t produced = 0;
  for (const auto& p : producers) produced += p->produced();
  const std::uint64_t consumed = pool.consumed();

  std::cout << "producers=12 workers=" << opt.consumers
            << " queue_size=" << opt.queue_size
            << " rate_per_sensor=" << opt.rate_per_sec << "/s"
            << " seconds=" << opt.seconds
            << (stop_requested() ? " (interrupted)" : "") << "\n"
            << "produced=" << produced << " consumed=" << consumed
            << " anomalies=" << pool.anomalies() << "\n";
  printLatency("detect", pool.detect_stats());
  printLatency("e2e   ", pool.e2e_stats());
  int rc = verifyNoDrop(produced, consumed);
  if (checkBudget(pool.e2e_stats()) != 0) rc = 3;
  return rc;
}

// Benchmark: 12 unpaced producers, 4-worker pool, report throughput.
// Uses a null alert sink so console I/O doesn't dominate the throughput
// measurement (swap in ConsoleAlertSink to debug alerts).
int run_benchmark(const Options& opt) {
  using sensor_monitor::detector::ZScoreDetector;
  using sensor_monitor::producer::SensorProducer;
  using sensor_monitor::queue::BoundedQueue;
  using sensor_monitor::sensor::SensorFactory;
  using sensor_monitor::sensor::SensorReading;
  using sensor_monitor::worker::NullAlertSink;
  using sensor_monitor::worker::WorkerPool;

  g_stop_requested.store(false);

  BoundedQueue<SensorReading> queue(opt.queue_size);
  auto sensors = SensorFactory::createDefaultSet();
  ZScoreDetector detector(loadConfig(opt));
  NullAlertSink sink;

  std::vector<std::unique_ptr<SensorProducer>> producers;
  producers.reserve(sensors.size());
  for (auto& s : sensors) {
    // rate <= 0 => as-fast-as-possible, no sleep.
    producers.push_back(
        std::make_unique<SensorProducer>(std::move(s), queue, 0.0));
  }

  WorkerPool pool(queue, detector, sink,
                  static_cast<std::size_t>(opt.consumers));
  // Steady-state gate: skip spawn/teardown transients so p50/p99 reflect
  // the pipeline, not thread-creation storms.
  pool.set_measuring(false);
  pool.start();

  const auto t0 = std::chrono::steady_clock::now();
  for (auto& p : producers) p->start();
  // Warmup excluded from latency stats.
  sleep_interruptible(0.3);
  if (!stop_requested()) pool.set_measuring(true);
  sleep_interruptible(opt.seconds);
  pool.set_measuring(false);
  for (auto& p : producers) p->stop();
  const auto t1 = std::chrono::steady_clock::now();
  queue.shutdown();
  pool.stop();

  std::uint64_t produced = 0;
  for (const auto& p : producers) produced += p->produced();
  const std::uint64_t consumed = pool.consumed();

  const double elapsed =
      std::chrono::duration<double>(t1 - t0).count();
  const double produced_per_sec = produced / elapsed;
  const double consumed_per_sec = consumed / elapsed;

  std::cout << "benchmark: producers=12 consumers=" << opt.consumers
            << " queue_size=" << opt.queue_size
            << " seconds=" << elapsed
            << (stop_requested() ? " (interrupted)" : "") << "\n"
            << "produced=" << produced << " (" << produced_per_sec
            << " readings/sec)\n"
            << "consumed=" << consumed << " (" << consumed_per_sec
            << " readings/sec)\n"
            << "anomalies=" << pool.anomalies() << "\n";
  printLatency("detect", pool.detect_stats());
  printLatency("e2e   ", pool.e2e_stats());
  int rc = verifyNoDrop(produced, consumed);
  if (produced_per_sec < 50000.0) {
    std::cerr << "WARNING: below 50K readings/sec target\n";
    rc = 2;
  }
  if (checkBudget(pool.e2e_stats()) != 0) rc = 3;
  return rc;
}

}  // namespace

int main(int argc, char* argv[]) {
  install_signal_handlers();
  Options opt = parse_args(argc, argv);
  if (opt.benchmark) {
    return run_benchmark(opt);
  }
  return run_normal(opt);
}
