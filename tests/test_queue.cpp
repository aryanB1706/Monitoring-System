#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include "queue/bounded_queue.hpp"

using sensor_monitor::queue::BoundedQueue;
using namespace std::chrono_literals;

TEST(QueueTest, SingleProducerConsumerFIFO) {
  BoundedQueue<int> q(10);
  constexpr int kCount = 100;

  std::thread producer([&] {
    for (int i = 0; i < kCount; ++i) {
      EXPECT_TRUE(q.push(i));
    }
  });

  std::vector<int> received;
  received.reserve(kCount);
  std::thread consumer([&] {
    for (int i = 0; i < kCount; ++i) {
      int v = -1;
      EXPECT_TRUE(q.pop(v));
      received.push_back(v);
    }
  });

  producer.join();
  consumer.join();

  ASSERT_EQ(received.size(), static_cast<std::size_t>(kCount));
  for (int i = 0; i < kCount; ++i) {
    EXPECT_EQ(received[i], i);
  }
  EXPECT_EQ(q.size(), 0u);
}

TEST(QueueTest, MultipleProducersConsumersNoLossOrDuplicates) {
  constexpr int kProducers = 4;
  constexpr int kConsumers = 4;
  constexpr int kPerProducer = 250;
  constexpr int kTotal = kProducers * kPerProducer;

  BoundedQueue<int> q(32);
  std::atomic<int> popped{0};
  std::vector<int> collected;
  std::mutex collected_mutex;

  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p] {
      for (int i = 0; i < kPerProducer; ++i) {
        int value = p * kPerProducer + i;
        EXPECT_TRUE(q.push(value));
      }
    });
  }

  std::vector<std::thread> consumers;
  for (int c = 0; c < kConsumers; ++c) {
    consumers.emplace_back([&] {
      while (popped.load() < kTotal) {
        int v = -1;
        if (q.pop(v)) {
          std::lock_guard<std::mutex> lock(collected_mutex);
          collected.push_back(v);
          ++popped;
        } else {
          break;  // shutdown + empty
        }
      }
    });
  }

  for (auto& t : producers) t.join();
  // All items produced; wait until consumed, then shut down to release
  // any consumers blocked in pop().
  while (popped.load() < kTotal) {
    std::this_thread::sleep_for(1ms);
  }
  q.shutdown();
  for (auto& t : consumers) t.join();

  EXPECT_EQ(collected.size(), static_cast<std::size_t>(kTotal));
  std::sort(collected.begin(), collected.end());
  for (int i = 0; i < kTotal; ++i) {
    EXPECT_EQ(collected[i], i);
  }
}

TEST(QueueTest, PushBlocksWhenFullBackpressure) {
  BoundedQueue<int> q(2);
  EXPECT_TRUE(q.push(1));
  EXPECT_TRUE(q.push(2));
  EXPECT_EQ(q.size(), 2u);

  // Queue is full — push must block.
  auto blocked_push =
      std::async(std::launch::async, [&] { return q.push(3); });
  EXPECT_EQ(blocked_push.wait_for(150ms), std::future_status::timeout);
  EXPECT_EQ(q.size(), 2u);

  // Free one slot; blocked push must complete.
  int v = -1;
  EXPECT_TRUE(q.pop(v));
  EXPECT_EQ(v, 1);
  EXPECT_EQ(blocked_push.wait_for(2s), std::future_status::ready);
  EXPECT_TRUE(blocked_push.get());
  EXPECT_EQ(q.size(), 2u);
}

TEST(QueueTest, PopBlocksWhenEmptyUntilItemArrives) {
  BoundedQueue<int> q(4);
  auto blocked_pop = std::async(std::launch::async, [&] {
    int v = -1;
    bool ok = q.pop(v);
    return std::make_pair(ok, v);
  });
  EXPECT_EQ(blocked_pop.wait_for(150ms), std::future_status::timeout);

  EXPECT_TRUE(q.push(42));
  EXPECT_EQ(blocked_pop.wait_for(2s), std::future_status::ready);
  auto [ok, v] = blocked_pop.get();
  EXPECT_TRUE(ok);
  EXPECT_EQ(v, 42);
}

TEST(QueueTest, ShutdownWakesBlockedPop) {
  BoundedQueue<int> q(4);
  auto blocked_pop = std::async(std::launch::async, [&] {
    int v = -1;
    return q.pop(v);  // blocks: empty
  });
  // Give it a moment to block, then shut down.
  EXPECT_EQ(blocked_pop.wait_for(150ms), std::future_status::timeout);
  q.shutdown();
  EXPECT_EQ(blocked_pop.wait_for(2s), std::future_status::ready);
  EXPECT_FALSE(blocked_pop.get());
  EXPECT_TRUE(q.is_shutdown());
}

TEST(QueueTest, ShutdownWakesBlockedPush) {
  BoundedQueue<int> q(1);
  EXPECT_TRUE(q.push(1));  // fill

  auto blocked_push =
      std::async(std::launch::async, [&] { return q.push(2); });  // blocks: full
  EXPECT_EQ(blocked_push.wait_for(150ms), std::future_status::timeout);
  q.shutdown();
  EXPECT_EQ(blocked_push.wait_for(2s), std::future_status::ready);
  EXPECT_FALSE(blocked_push.get());

  // After shutdown: push fails, pop drains then fails.
  EXPECT_FALSE(q.push(99));
  int v = -1;
  EXPECT_TRUE(q.pop(v));
  EXPECT_EQ(v, 1);
  EXPECT_FALSE(q.pop(v));
}

TEST(QueueTest, SizeIsThreadSafe) {
  BoundedQueue<int> q(16);
  EXPECT_EQ(q.size(), 0u);
  EXPECT_TRUE(q.push(1));
  EXPECT_TRUE(q.push(2));
  EXPECT_EQ(q.size(), 2u);
  int v = -1;
  EXPECT_TRUE(q.pop(v));
  EXPECT_EQ(q.size(), 1u);
}

TEST(QueueTest, ZeroCapacityThrows) {
  EXPECT_THROW(BoundedQueue<int>(0), std::invalid_argument);
}

TEST(QueueTest, DrainReturnsUpToMax) {
  BoundedQueue<int> q(16);
  for (int i = 0; i < 10; ++i) EXPECT_TRUE(q.push(i));
  std::vector<int> batch;
  EXPECT_EQ(q.drain_to(batch, 4), 4u);
  ASSERT_EQ(batch.size(), 4u);
  for (int i = 0; i < 4; ++i) EXPECT_EQ(batch[i], i);
  EXPECT_EQ(q.size(), 6u);
  // Drain remainder with a max larger than what's left.
  EXPECT_EQ(q.drain_to(batch, 100), 6u);
  EXPECT_EQ(batch.size(), 10u);
  EXPECT_TRUE(q.empty());
}

TEST(QueueTest, DrainBlocksUntilItemArrives) {
  BoundedQueue<int> q(8);
  std::vector<int> batch;
  auto blocked = std::async(std::launch::async, [&] {
    return q.drain_to(batch, 8);
  });
  EXPECT_EQ(blocked.wait_for(150ms), std::future_status::timeout);
  EXPECT_TRUE(q.push(7));
  EXPECT_EQ(blocked.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(blocked.get(), 1u);
  ASSERT_EQ(batch.size(), 1u);
  EXPECT_EQ(batch[0], 7);
}

TEST(QueueTest, DrainShutdownReturnsZero) {
  BoundedQueue<int> q(8);
  std::vector<int> batch;
  auto blocked = std::async(std::launch::async, [&] {
    return q.drain_to(batch, 8);
  });
  EXPECT_EQ(blocked.wait_for(150ms), std::future_status::timeout);
  q.shutdown();
  EXPECT_EQ(blocked.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(blocked.get(), 0u);
  EXPECT_TRUE(batch.empty());
}

TEST(QueueTest, ShutdownIsIdempotent) {
  BoundedQueue<int> q(4);
  EXPECT_TRUE(q.push(1));
  q.shutdown();
  q.shutdown();  // second call must be a harmless no-op
  EXPECT_TRUE(q.is_shutdown());
  int v = -1;
  EXPECT_TRUE(q.pop(v));  // remaining item still drainable
  EXPECT_EQ(v, 1);
  EXPECT_FALSE(q.pop(v));  // now empty + shutdown
}

TEST(QueueTest, PushAfterShutdownFailsImmediately) {
  BoundedQueue<int> q(4);
  q.shutdown();
  // Must not block: queue is already shut down.
  auto res = std::async(std::launch::async, [&] { return q.push(1); });
  EXPECT_EQ(res.wait_for(2s), std::future_status::ready);
  EXPECT_FALSE(res.get());
  EXPECT_FALSE(q.push(2));
}

TEST(QueueTest, CapacityOneStrictAlternationNoLoss) {
  // Smallest capacity exercises maximum backpressure: 200 items SPSC.
  BoundedQueue<int> q(1);
  constexpr int kCount = 200;
  std::atomic<bool> producer_ok{true};
  std::thread producer([&] {
    for (int i = 0; i < kCount; ++i) {
      if (!q.push(i)) producer_ok.store(false);
    }
  });
  std::vector<int> received;
  int v = -1;
  for (int i = 0; i < kCount; ++i) {
    ASSERT_TRUE(q.pop(v));
    received.push_back(v);
  }
  producer.join();
  EXPECT_TRUE(producer_ok.load());
  ASSERT_EQ(received.size(), static_cast<std::size_t>(kCount));
  for (int i = 0; i < kCount; ++i) EXPECT_EQ(received[i], i);
}

TEST(QueueTest, ConcurrentDrainNoLossUnderLoad) {
  // drain_to() consumers racing with multiple producers: every item must
  // arrive exactly once.
  constexpr int kProducers = 4;
  constexpr int kPerProducer = 250;
  constexpr int kTotal = kProducers * kPerProducer;
  BoundedQueue<int> q(32);
  std::atomic<int> consumed{0};
  std::atomic<bool> ok{true};
  std::vector<int> collected;
  std::mutex collected_mutex;

  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p] {
      for (int i = 0; i < kPerProducer; ++i) {
        if (!q.push(p * kPerProducer + i)) ok.store(false);
      }
    });
  }
  std::vector<std::thread> consumers;
  for (int c = 0; c < 4; ++c) {
    consumers.emplace_back([&] {
      std::vector<int> batch;
      batch.reserve(16);
      while (true) {
        batch.clear();
        std::size_t n = q.drain_to(batch, 16);
        if (n == 0) break;  // shutdown + empty
        std::lock_guard<std::mutex> lock(collected_mutex);
        for (int x : batch) collected.push_back(x);
        consumed.fetch_add(static_cast<int>(n));
      }
    });
  }
  for (auto& t : producers) t.join();
  // Wait for the full drain, then release blocked consumers.
  auto deadline = std::chrono::steady_clock::now() + 10s;
  while (consumed.load() < kTotal &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  q.shutdown();
  for (auto& t : consumers) t.join();

  EXPECT_TRUE(ok.load());
  EXPECT_EQ(consumed.load(), kTotal);
  ASSERT_EQ(collected.size(), static_cast<std::size_t>(kTotal));
  std::sort(collected.begin(), collected.end());
  for (int i = 0; i < kTotal; ++i) EXPECT_EQ(collected[i], i);
}

TEST(QueueTest, MovePushAndAccessors) {
  BoundedQueue<std::string> q(4);
  EXPECT_EQ(q.capacity(), 4u);
  EXPECT_TRUE(q.empty());
  std::string s = "hello";
  EXPECT_TRUE(q.push(std::move(s)));  // move overload
  EXPECT_EQ(q.size(), 1u);
  EXPECT_FALSE(q.empty());
  std::string out;
  EXPECT_TRUE(q.pop(out));
  EXPECT_EQ(out, "hello");
  EXPECT_TRUE(q.empty());
}
