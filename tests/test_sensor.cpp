#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <set>
#include <string>

#include "sensor/pressure_sensor.hpp"
#include "sensor/sensor.hpp"
#include "sensor/sensor_factory.hpp"
#include "sensor/temperature_sensor.hpp"
#include "sensor/vibration_sensor.hpp"

using sensor_monitor::sensor::PressureSensor;
using sensor_monitor::sensor::Sensor;
using sensor_monitor::sensor::SensorFactory;
using sensor_monitor::sensor::TemperatureSensor;
using sensor_monitor::sensor::VibrationSensor;

TEST(SensorTest, IsAbstract) {
  EXPECT_FALSE(std::is_constructible<Sensor>::value);
  EXPECT_TRUE((std::is_abstract<Sensor>::value));
}

TEST(SensorTest, DerivedTypesReportCorrectType) {
  TemperatureSensor t("temp-0", 1);
  PressureSensor p("press-0", 1);
  VibrationSensor v("vib-0", 1);
  EXPECT_EQ(t.type(), "temperature");
  EXPECT_EQ(p.type(), "pressure");
  EXPECT_EQ(v.type(), "vibration");
}

TEST(SensorTest, IdsAreReturned) {
  TemperatureSensor t("temp-42", 1);
  EXPECT_EQ(t.id(), "temp-42");
}

TEST(SensorTest, ReadingsAreFinite) {
  TemperatureSensor t("temp-0", 123);
  PressureSensor p("press-0", 123);
  VibrationSensor v("vib-0", 123);
  for (int i = 0; i < 100; ++i) {
    EXPECT_TRUE(std::isfinite(t.readValue()));
    EXPECT_TRUE(std::isfinite(p.readValue()));
    EXPECT_TRUE(std::isfinite(v.readValue()));
  }
}

TEST(SensorTest, PolymorphicDispatch) {
  std::unique_ptr<Sensor> s =
      std::make_unique<TemperatureSensor>("temp-0", 7);
  EXPECT_EQ(s->type(), "temperature");
  EXPECT_EQ(s->id(), "temp-0");
  EXPECT_TRUE(std::isfinite(s->readValue()));
}

TEST(SensorFactoryTest, CreatesTwelveMixedSensorsWithUniqueIds) {
  auto sensors = SensorFactory::createDefaultSet();
  ASSERT_EQ(sensors.size(), 12u);

  std::set<std::string> ids;
  std::set<std::string> types;
  for (const auto& s : sensors) {
    ASSERT_NE(s, nullptr);
    EXPECT_TRUE(ids.insert(s->id()).second) << "Duplicate id: " << s->id();
    types.insert(s->type());
    EXPECT_TRUE(std::isfinite(s->readValue()));
  }
  EXPECT_EQ(types.size(), 3u);  // all 3 types present
}

TEST(SensorFactoryTest, CustomCountIsHonored) {
  auto sensors = SensorFactory::create(6, 99);
  EXPECT_EQ(sensors.size(), 6u);
}

TEST(SensorTest, TemperatureValuesStayInPlausibleRange) {
  // normal_(60, 12) + rare spikes around 110±150. With a fixed seed the
  // stream is deterministic; assert every sample is finite and within a
  // wide physical envelope (catches NaN/inf or unit mix-ups).
  TemperatureSensor t("temp-0", 42);
  double sum = 0.0;
  constexpr int kN = 2000;
  for (int i = 0; i < kN; ++i) {
    const double v = t.readValue();
    EXPECT_TRUE(std::isfinite(v)) << "i=" << i << " v=" << v;
    EXPECT_GT(v, -200.0) << "i=" << i;
    EXPECT_LT(v, 600.0) << "i=" << i;
    sum += v;
  }
  // Mean must sit near the 60 nominal band (spikes are 5% so pull is small).
  const double mean = sum / kN;
  EXPECT_GT(mean, 40.0) << "mean=" << mean;
  EXPECT_LT(mean, 90.0) << "mean=" << mean;
}

TEST(SensorTest, PressureValuesAreFinite) {
  PressureSensor p("press-0", 42);
  int finite = 0;
  for (int i = 0; i < 1000; ++i) {
    if (std::isfinite(p.readValue())) ++finite;
  }
  EXPECT_EQ(finite, 1000);
}

TEST(SensorTest, VibrationValuesAreNonNegativeAndFinite) {
  // Vibration clamps negatives to 0.0 (mm/s can't be negative).
  VibrationSensor v("vib-0", 7);
  for (int i = 0; i < 1000; ++i) {
    const double val = v.readValue();
    EXPECT_TRUE(std::isfinite(val));
    EXPECT_GE(val, 0.0);
  }
}

TEST(SensorTest, SameSeedGivesDeterministicSequence) {
  TemperatureSensor a("temp-0", 1234);
  TemperatureSensor b("temp-0", 1234);
  for (int i = 0; i < 50; ++i) {
    EXPECT_DOUBLE_EQ(a.readValue(), b.readValue()) << "i=" << i;
  }
}

TEST(SensorTest, DifferentSeedsDiverge) {
  TemperatureSensor a("temp-0", 1);
  TemperatureSensor b("temp-0", 2);
  bool any_different = false;
  for (int i = 0; i < 50; ++i) {
    if (a.readValue() != b.readValue()) {
      any_different = true;
      break;
    }
  }
  EXPECT_TRUE(any_different);
}

TEST(SensorFactoryTest, RoundRobinTypesAcrossCounts) {
  // create() cycles temp/press/vib in order.
  auto sensors = SensorFactory::create(6, 11);
  ASSERT_EQ(sensors.size(), 6u);
  EXPECT_EQ(sensors[0]->type(), "temperature");
  EXPECT_EQ(sensors[1]->type(), "pressure");
  EXPECT_EQ(sensors[2]->type(), "vibration");
  EXPECT_EQ(sensors[3]->type(), "temperature");
  EXPECT_EQ(sensors[4]->type(), "pressure");
  EXPECT_EQ(sensors[5]->type(), "vibration");
}

TEST(SensorFactoryTest, UniqueIdsAndDeterministicSeeds) {
  auto first = SensorFactory::create(9, 77);
  auto second = SensorFactory::create(9, 77);
  ASSERT_EQ(first.size(), 9u);
  ASSERT_EQ(second.size(), 9u);
  std::set<std::string> ids;
  for (std::size_t i = 0; i < first.size(); ++i) {
    ASSERT_NE(first[i], nullptr);
    EXPECT_TRUE(ids.insert(first[i]->id()).second);
    // Same seed => same ids in the same order.
    EXPECT_EQ(first[i]->id(), second[i]->id());
    EXPECT_EQ(first[i]->type(), second[i]->type());
  }
  EXPECT_EQ(ids.size(), 9u);
}
