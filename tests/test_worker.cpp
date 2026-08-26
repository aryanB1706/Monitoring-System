#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

#include "detector/zscore_detector.hpp"
#include "producer/producer.hpp"
#include "queue/bounded_queue.hpp"
#include "sensor/reading.hpp"
#include "sensor/sensor_factory.hpp"
#include "sensor/temperature_sensor.hpp"
#include "worker/alert_sink.hpp"
#include "worker/worker.hpp"

using sensor_monitor::detector::Detection;
using sensor_monitor::detector::Severity;
using sensor_monitor::detector::ZScoreDetector;
using sensor_monitor::producer::SensorProducer;
using sensor_monitor::queue::BoundedQueue;
using sensor_monitor::sensor::SensorFactory;
using sensor_monitor::sensor::SensorReading;
using sensor_monitor::sensor::TemperatureSensor;
using sensor_monitor::worker::Alert;
using sensor_monitor::worker::ConsoleAlertSink;
using sensor_monitor::worker::NullAlertSink;
using sensor_monitor::worker::VectorAlertSink;
using sensor_monitor::worker::Worker;
using sensor_monitor::worker::WorkerPool;
using namespace std::chrono_literals;

namespace {

SensorReading makeReading(const std::string& id, double value) {
  SensorReading r;
  r.id = id;
  r.type = "temperature";
  r.value = value;
  r.timestamp = std::chrono::system_clock::now();
  return r;
}

}  // namespace

TEST(WorkerTest, ScaffoldConstructs) {
  Worker w;
  SUCCEED();
}

TEST(WorkerTest, DefaultIsFourWorkers) {
  EXPECT_EQ(WorkerPool::kDefaultWorkers, 4u);
  BoundedQueue<SensorReading> q(16);
  ZScoreDetector detector;
  NullAlertSink sink;
  WorkerPool pool(q, detector, sink);  // default arg
  EXPECT_EQ(pool.num_workers(), 4u);
}

TEST(WorkerTest, InvalidArgsThrow) {
  BoundedQueue<SensorReading> q(16);
  ZScoreDetector detector;
  NullAlertSink sink;
  EXPECT_THROW(WorkerPool(q, detector, sink, 0), std::invalid_argument);
  EXPECT_THROW(WorkerPool(q, detector, sink, 4, 0), std::invalid_argument);
}

TEST(WorkerTest, FourWorkersDrainFullyZeroDrop) {
  constexpr int kProducers = 4;
  constexpr int kPerProducer = 250;
  constexpr int kTotal = kProducers * kPerProducer;

  BoundedQueue<SensorReading> q(128);
  ZScoreDetector detector;
  NullAlertSink sink;
  WorkerPool pool(q, detector, sink, 4);
  pool.start();

  std::atomic<int> pushed{0};
  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p] {
      for (int i = 0; i < kPerProducer; ++i) {
        SensorReading r = makeReading("s-" + std::to_string(p),
                                      static_cast<double>(i));
        EXPECT_TRUE(q.push(std::move(r)));
        ++pushed;
      }
    });
  }
  for (auto& t : producers) t.join();

  // Graceful shutdown: no more pushes -> shutdown -> workers drain fully.
  q.shutdown();
  pool.stop();

  EXPECT_EQ(pushed.load(), kTotal);
  EXPECT_EQ(pool.consumed(), static_cast<std::uint64_t>(kTotal));
  EXPECT_TRUE(q.empty());
}

TEST(WorkerTest, AnomaliesRaiseSeverityTaggedAlerts) {
  BoundedQueue<SensorReading> q(64);
  ZScoreDetector detector;
  // Pre-load history: alternating 10/11 => mean 10.5, stddev 0.5.
  for (int i = 0; i < 50; ++i) {
    detector.detect(makeReading("s-0", (i % 2 == 0) ? 10.0 : 11.0));
  }
  VectorAlertSink sink;

  WorkerPool pool(q, detector, sink, 4);
  pool.start();
  // z = (13 - 10.5)/0.5 = 5.0 => CRITICAL.
  EXPECT_TRUE(q.push(makeReading("s-0", 13.0)));
  // Normal value: z ~ 0.2 => no alert.
  EXPECT_TRUE(q.push(makeReading("s-0", 10.6)));
  q.shutdown();
  pool.stop();

  EXPECT_EQ(pool.consumed(), 2u);
  EXPECT_EQ(pool.anomalies(), 1u);
  auto alerts = sink.snapshot();
  ASSERT_EQ(alerts.size(), 1u);
  EXPECT_EQ(alerts[0].sensor_id, "s-0");
  EXPECT_EQ(alerts[0].severity, Severity::kCritical);
  EXPECT_NEAR(alerts[0].z_score, 5.0, 1e-9);
}

TEST(WorkerTest, ProducersStopThenDrainZeroDrop) {
  BoundedQueue<SensorReading> q(256);
  auto sensors = SensorFactory::createDefaultSet();
  ASSERT_EQ(sensors.size(), 12u);
  ZScoreDetector detector;
  NullAlertSink sink;
  WorkerPool pool(q, detector, sink, 4);
  pool.start();

  std::vector<std::unique_ptr<SensorProducer>> producers;
  for (auto& s : sensors) {
    producers.push_back(
        std::make_unique<SensorProducer>(std::move(s), q, 0.0));
  }
  for (auto& p : producers) p->start();
  std::this_thread::sleep_for(200ms);
  // Stop-flag path: producers stop pushing first...
  for (auto& p : producers) p->stop();
  // ...then queue shutdown lets workers exit only after fully drained.
  q.shutdown();
  pool.stop();

  std::uint64_t produced = 0;
  for (const auto& p : producers) produced += p->produced();
  EXPECT_GT(produced, 0u);
  EXPECT_EQ(pool.consumed(), produced)
      << "zero-drop invariant violated: produced != consumed";
}

TEST(WorkerTest, MeasuringGateSkipsLatencySamples) {
  BoundedQueue<SensorReading> q(64);
  ZScoreDetector detector;
  NullAlertSink sink;
  WorkerPool pool(q, detector, sink, 2);
  pool.set_measuring(false);
  pool.start();
  for (int i = 0; i < 10; ++i) {
    EXPECT_TRUE(q.push(makeReading("s-0", 1.0)));
  }
  q.shutdown();
  pool.stop();
  EXPECT_EQ(pool.consumed(), 10u);
  EXPECT_EQ(pool.detect_stats().count(), 0u);
  EXPECT_EQ(pool.e2e_stats().count(), 0u);
}

TEST(WorkerTest, GracefulShutdownUnderLoadNoDrops) {
  // Small queue => constant backpressure while 12 unpaced producers race
  // 4 workers. Stop mid-flight, then drain: produced must equal consumed.
  BoundedQueue<SensorReading> q(32);
  auto sensors = SensorFactory::createDefaultSet();
  ASSERT_EQ(sensors.size(), 12u);
  ZScoreDetector detector;
  NullAlertSink sink;
  WorkerPool pool(q, detector, sink, 4);
  pool.start();

  std::vector<std::unique_ptr<SensorProducer>> producers;
  for (auto& s : sensors) {
    producers.push_back(
        std::make_unique<SensorProducer>(std::move(s), q, 0.0));
  }
  for (auto& p : producers) p->start();
  std::this_thread::sleep_for(500ms);
  for (auto& p : producers) p->stop();
  q.shutdown();
  pool.stop();

  std::uint64_t produced = 0;
  for (const auto& p : producers) produced += p->produced();
  EXPECT_GT(produced, 0u);
  EXPECT_EQ(pool.consumed(), produced);
  EXPECT_TRUE(q.empty());
  // Every consumed reading recorded a latency sample (measuring on).
  EXPECT_EQ(pool.detect_stats().count(), produced);
  EXPECT_EQ(pool.e2e_stats().count(), produced);
}

TEST(WorkerTest, StopIsIdempotentAndRunningFlag) {
  BoundedQueue<SensorReading> q(16);
  ZScoreDetector detector;
  NullAlertSink sink;
  WorkerPool pool(q, detector, sink, 4);
  EXPECT_FALSE(pool.running());
  pool.start();
  EXPECT_TRUE(pool.running());
  pool.start();  // second start is a no-op, must not spawn extra threads
  EXPECT_TRUE(pool.running());
  q.shutdown();
  pool.stop();
  EXPECT_FALSE(pool.running());
  const auto consumed = pool.consumed();
  pool.stop();  // second stop must not hang or change counts
  EXPECT_FALSE(pool.running());
  EXPECT_EQ(pool.consumed(), consumed);
}

TEST(WorkerTest, SingleWorkerAlsoZeroDrop) {
  BoundedQueue<SensorReading> q(64);
  ZScoreDetector detector;
  NullAlertSink sink;
  WorkerPool pool(q, detector, sink, 1);
  pool.start();
  constexpr int kN = 300;
  for (int i = 0; i < kN; ++i) {
    EXPECT_TRUE(q.push(makeReading("s-0", static_cast<double>(i))));
  }
  q.shutdown();
  pool.stop();
  EXPECT_EQ(pool.consumed(), static_cast<std::uint64_t>(kN));
}

TEST(WorkerTest, BatchSizeOneStillZeroDrop) {
  BoundedQueue<SensorReading> q(64);
  ZScoreDetector detector;
  NullAlertSink sink;
  WorkerPool pool(q, detector, sink, 4, 1);  // no batching at all
  pool.start();
  constexpr int kN = 200;
  for (int i = 0; i < kN; ++i) {
    EXPECT_TRUE(q.push(makeReading("s-0", static_cast<double>(i))));
  }
  q.shutdown();
  pool.stop();
  EXPECT_EQ(pool.consumed(), static_cast<std::uint64_t>(kN));
}

TEST(WorkerTest, VectorSinkIsThreadSafeUnderConcurrentEmit) {
  VectorAlertSink sink;
  constexpr int kThreads = 4;
  constexpr int kPerThread = 250;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) {
        Detection det;
        det.sensor_id = "s-" + std::to_string(t);
        det.sensor_type = "temperature";
        det.value = 13.0;
        det.z_score = 5.0;
        det.severity = Severity::kCritical;
        sink.emit(Alert::fromDetection(det));
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(sink.size(), static_cast<std::size_t>(kThreads * kPerThread));
  auto alerts = sink.snapshot();
  ASSERT_EQ(alerts.size(), static_cast<std::size_t>(kThreads * kPerThread));
  for (const auto& a : alerts) {
    EXPECT_EQ(a.severity, Severity::kCritical);
  }
}

TEST(WorkerTest, ConsoleSinkWritesSeverityTaggedLine) {
  std::ostringstream oss;
  ConsoleAlertSink sink(oss);
  Detection det;
  det.sensor_id = "s-9";
  det.sensor_type = "pressure";
  det.value = 0.17;
  det.z_score = -5.0;
  det.severity = Severity::kCritical;
  sink.emit(Alert::fromDetection(det));
  const std::string out = oss.str();
  EXPECT_NE(out.find("CRITICAL"), std::string::npos) << out;
  EXPECT_NE(out.find("s-9"), std::string::npos) << out;
}
