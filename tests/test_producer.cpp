#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include "producer/producer.hpp"
#include "queue/bounded_queue.hpp"
#include "sensor/reading.hpp"
#include "sensor/sensor_factory.hpp"
#include "sensor/temperature_sensor.hpp"

using sensor_monitor::producer::SensorProducer;
using sensor_monitor::queue::BoundedQueue;
using sensor_monitor::sensor::SensorFactory;
using sensor_monitor::sensor::SensorReading;
using sensor_monitor::sensor::TemperatureSensor;
using namespace std::chrono_literals;

TEST(ProducerTest, PushesTimestampedReadings) {
  BoundedQueue<SensorReading> q(64);
  SensorProducer p(
      std::make_unique<TemperatureSensor>("temp-0", 1), q, 0.0);

  // Consumer must drain continuously: an unpaced producer would otherwise
  // fill the queue and block in push() forever, deadlocking p.stop().
  std::vector<SensorReading> received;
  std::mutex received_mutex;
  std::thread consumer([&] {
    SensorReading r;
    while (q.pop(r)) {
      std::lock_guard<std::mutex> lock(received_mutex);
      received.push_back(r);
    }
  });

  p.start();
  // Wait for at least one reading (with timeout to avoid hanging forever).
  for (int i = 0; i < 200; ++i) {
    {
      std::lock_guard<std::mutex> lock(received_mutex);
      if (!received.empty()) break;
    }
    std::this_thread::sleep_for(10ms);
  }
  p.stop();  // consumer still draining, so push() can't stay blocked
  q.shutdown();
  consumer.join();

  ASSERT_FALSE(received.empty());
  const SensorReading& got = received.front();
  EXPECT_EQ(got.id, "temp-0");
  EXPECT_EQ(got.type, "temperature");
  EXPECT_TRUE(std::isfinite(got.value));
  EXPECT_NE(got.timestamp, SensorReading{}.timestamp);
  EXPECT_GE(p.produced(), 1u);
}

TEST(ProducerTest, ConfigurableRateIsHonored) {
  BoundedQueue<SensorReading> q(256);
  // 50 readings/sec for ~0.4s => ~20 readings.
  SensorProducer p(
      std::make_unique<TemperatureSensor>("temp-0", 2), q, 50.0);

  std::atomic<std::uint64_t> consumed{0};
  std::thread consumer([&] {
    SensorReading r;
    while (q.pop(r)) ++consumed;
  });

  p.start();
  std::this_thread::sleep_for(400ms);
  p.stop();
  q.shutdown();
  consumer.join();

  const auto n = p.produced();
  EXPECT_GE(n, 5u);
  EXPECT_LE(n, 60u);
  EXPECT_EQ(consumed.load(), n);
}

TEST(ProducerTest, TwelveFactorySensorsShareOneQueue) {
  BoundedQueue<SensorReading> q(256);
  auto sensors = SensorFactory::createDefaultSet();
  ASSERT_EQ(sensors.size(), 12u);

  std::vector<std::unique_ptr<SensorProducer>> producers;
  for (auto& s : sensors) {
    producers.push_back(
        std::make_unique<SensorProducer>(std::move(s), q, 0.0));
  }

  std::atomic<std::uint64_t> consumed{0};
  std::unordered_set<std::string> seen_ids;
  std::mutex seen_mutex;
  std::thread consumer([&] {
    SensorReading r;
    while (q.pop(r)) {
      ++consumed;
      std::lock_guard<std::mutex> lock(seen_mutex);
      seen_ids.insert(r.id);
    }
  });

  for (auto& p : producers) p->start();
  std::this_thread::sleep_for(200ms);
  for (auto& p : producers) p->stop();
  q.shutdown();
  consumer.join();

  EXPECT_GT(consumed.load(), 0u);
  EXPECT_EQ(seen_ids.size(), 12u);  // all 12 unique IDs observed
}
