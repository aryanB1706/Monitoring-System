# sensor-monitor

Real-time sensor monitoring pipeline in C++17: 12 simulated sensors push
timestamped readings through a bounded blocking queue into a 4-worker
consumer pool that runs z-score anomaly detection and raises
severity-tagged alerts. Graceful shutdown guarantees zero dropped readings
(`produced == consumed` is asserted on every run).

## Architecture

```
                        +------------------ BoundedQueue (blocking, backpressure)
                        |                   capacity 1024 normal / 128 benchmark
                        v
  +---------+   +---------------+   +-----------------+   +------------+   +--------------+
  | Sensors |-->| SensorProducer|-->| BoundedQueue    |-->| WorkerPool |-->| AlertSink    |
  | x12     |   | x12 threads   |   | SensorReading   |   | x4 workers |   | (pluggable)  |
  +---------+   +---------------+   +-----------------+   +-----+------+   +--------------+
   temp/press/    paced (100/s)         push blocks          |  bulk drain_to(64)   Console (normal)
   vib + factory  or as-fast-as-        when full;           |  per worker          Null (benchmark)
   seeds          possible (bench)      pop blocks           v                      Vector (tests)
                                               when empty  +-----------------+
                                                           | ZScoreDetector  |
                                                           | per-sensor      |
                                                           | rolling window  |
                                                           +-----------------+
                                                                     |
                                                                     v
                                                              +--------------+
                                                              | Alert        |
                                                              | OK/WARNING/  |
                                                              | CRITICAL     |
                                                              +--------------+
```

### Components

| Module | Files | Role |
|---|---|---|
| `sensor` | `include/sensor/`, `src/sensor/` | `Sensor` interface + `TemperatureSensor`, `PressureSensor`, `VibrationSensor` (seeded RNG, 5% injected outliers) + `SensorFactory` (12 mixed sensors, unique IDs) |
| `producer` | `include/producer/`, `src/producer/` | `SensorProducer`: one thread per sensor, pushes timestamped `SensorReading`s; paced (`100/s`) or as-fast-as-possible (`rate <= 0`) |
| `queue` | `include/queue/bounded_queue.hpp` | `BoundedQueue<T>`: blocking push/pop with backpressure, bulk `drain_to()`, `shutdown()` wakes waiters but still drains remaining items |
| `detector` | `include/detector/`, `src/detector/` | `AnomalyDetector` interface + thread-safe `ZScoreDetector` (per-sensor rolling window, z-score severity tagging) + `LatencyStats` (p50/p99/max/mean) |
| `worker` | `include/worker/`, `src/worker/` | `WorkerPool` (default 4 workers): bulk-drain → `detect()` → emit `Alert` to an `AlertSink`; `ConsoleAlertSink` / `NullAlertSink` / `VectorAlertSink` |
| `main` | `src/main.cpp` | CLI, SIGINT/SIGTERM handling, normal + benchmark modes, zero-drop assertion, 5ms p99 e2e budget check |

### Shutdown protocol (zero-drop)

1. Producers are stopped first — no new readings enter the queue.
2. `queue.shutdown()` wakes blocked workers; remaining items stay drainable.
3. Workers exit only when `drain_to()` returns 0 (queue empty **and** shut down).
4. `produced == consumed` is asserted; mismatch prints `ERROR` and exits 1.
   `SIGINT`/`SIGTERM` triggers the same path early (run prints `(interrupted)`).

## Build

Requires CMake >= 3.14 and a C++17 compiler. GoogleTest is fetched
automatically via `FetchContent`.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Run

```sh
# Normal demo: 12 paced producers (100/s each) + 4 workers, console alerts
./build/sensor-monitor --seconds 2

# Benchmark: 12 unpaced producers + 4 workers, throughput + latency report
./build/sensor-monitor --benchmark --seconds 3

# Custom worker count / queue / detector config
./build/sensor-monitor --benchmark --seconds 3 --consumers 4 --queue-size 128 \
  --detector-config ./detector.conf
```

| Flag | Default | Meaning |
|---|---|---|
| `--benchmark` | off | Unpaced producers, null alert sink (console I/O would dominate throughput) |
| `--seconds N` | `3` | Run duration |
| `--rate R` | `100` | Per-sensor readings/sec in normal mode (`<= 0` = unlimited) |
| `--queue-size Q` | `1024` (`128` benchmark) | Queue capacity (backpressure point) |
| `--consumers C` | `4` | WorkerPool threads |
| `--detector-config PATH` | — | `key=value` detector thresholds file |

## Benchmark results (measured)

Environment: 4-core Linux sandbox, `g++ 13.3`, `Release` build,
`--benchmark --seconds 3` (12 unpaced producers, 4 workers, queue 128).
Numbers vary by machine — rerun the command above to reproduce.

| Run | Throughput | e2e p50 | e2e p99 | detect p99 | produced | consumed | Exit |
|---|---|---|---|---|---|---|---|
| 1 | 332,641/s | 0.47ms | 6.67ms | 0.0022ms | 1,107,719 | 1,107,719 | 3 (budget) |
| 2 | 343,929/s | 0.45ms | 6.08ms | 0.0024ms | 1,146,983 | 1,146,983 | 3 (budget) |
| 3 | 349,569/s | 0.49ms | 6.39ms | ~0.002ms | 1,165,391 | 1,165,391 | 3 (budget) |

Normal mode (`--seconds 2`, paced 100/s, queue 1024, console alerts):

| Run | produced | consumed | anomalies | detect p99 | e2e p99 | Exit |
|---|---|---|---|---|---|---|
| 1 | 2,357 | 2,357 | 88 | 0.0046ms | 1.60ms | 0 |
| 2 | 2,389 | 2,389 | 89 | ~0.004ms | 1.54ms | 0 |

Definitions: `detect` = compute time inside `detect()`; `e2e` = reading
timestamp (push time) to alert-raised time, **including queueing delay**.
The 5ms budget check applies to e2e p99 (`kLatencyBudget` in `main.cpp`).

Known limit (documented honestly): in benchmark mode the producers are
unpaced, so the 128-deep queue stays full and queueing delay alone pushes
e2e p99 to ~6–6.7ms, over the 5ms budget — the binary reports
`WARNING: p99 end-to-end latency ... exceeds 5ms budget` and exits 3.
Detection itself is microseconds (p99 ~0.002ms). In paced normal operation
e2e p99 is ~1.5–1.6ms, inside budget. Throughput (~330–350K/s) clears the
50K/s target ~7x.

## Tests

```sh
ctest --test-dir build --output-on-failure
```

68 GoogleTest tests, all passing, across 6 suites:
`SensorTest` + `SensorFactoryTest` (value ranges, determinism, factory),
`QueueTest` (concurrency, backpressure, shutdown), `ProducerTest`,
`DetectorTest` (rolling-window correctness, thresholds/severity, edge cases
such as zero variance and `<100` samples), `WorkerTest` (zero drops,
graceful shutdown under load, sink thread-safety).

CI (`.github/workflows/ci.yml`): `ctest` runs on every push/PR and fails the
build on any test failure, plus a separate `-fsanitize=thread` job that runs
the same suite under ThreadSanitizer.

## Anomaly thresholds

`DetectorConfig` (`include/detector/detector.hpp`) defaults:

| Key | Default | Meaning |
|---|---|---|
| `window_size` | `100` | Per-sensor rolling window (oldest evicted first) |
| `anomaly_threshold` | `3.0` | `|z|` at/above this = anomaly |
| `warning_threshold` | `3.0` | `|z|` in `[warning, critical)` → `WARNING` |
| `critical_threshold` | `4.0` | `|z|` at/above this → `CRITICAL` |
| `min_samples` | `10` | Warm-up readings per sensor before any detection |

Each reading is scored against its sensor's window **before** being inserted
(`z = (value - mean) / stddev`; zero stddev yields `z = 0`, never an anomaly).
Override via `--detector-config PATH` with a `key=value` file (`#` comments,
unknown keys ignored, missing keys keep defaults):

```
# detector.conf
window_size = 50
anomaly_threshold = 2.5
warning_threshold = 2.5
critical_threshold = 5.0
min_samples = 5
```

Invalid combinations (e.g. `critical_threshold < warning_threshold`,
`window_size = 0`) throw `std::invalid_argument` at startup.
