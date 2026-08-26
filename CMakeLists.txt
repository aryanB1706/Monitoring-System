cmake_minimum_required(VERSION 3.14)
project(sensor-monitor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Library with all modules
add_library(sensor_monitor_lib
  src/sensor/sensor.cpp
  src/sensor/temperature_sensor.cpp
  src/sensor/pressure_sensor.cpp
  src/sensor/vibration_sensor.cpp
  src/sensor/sensor_factory.cpp
  src/producer/producer.cpp
  src/queue/queue.cpp
  src/detector/detector.cpp
  src/detector/zscore_detector.cpp
  src/worker/worker.cpp
)

target_include_directories(sensor_monitor_lib PUBLIC include)

find_package(Threads REQUIRED)
target_link_libraries(sensor_monitor_lib PUBLIC Threads::Threads)

# Main executable
add_executable(sensor-monitor src/main.cpp)
target_link_libraries(sensor-monitor PRIVATE sensor_monitor_lib)

# Warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(sensor_monitor_lib PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(sensor-monitor PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Testing
option(BUILD_TESTING "Build tests" ON)
if(BUILD_TESTING)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  )
  # For Windows: prevent overriding parent project's compiler/linker settings
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_subdirectory(tests)
endif()
