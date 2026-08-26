add_executable(sensor-monitor-tests
  test_sensor.cpp
  test_queue.cpp
  test_producer.cpp
  test_detector.cpp
  test_worker.cpp
)

target_link_libraries(sensor-monitor-tests PRIVATE
  sensor_monitor_lib
  GTest::gtest_main
)

target_include_directories(sensor-monitor-tests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)

include(GoogleTest)
gtest_discover_tests(sensor-monitor-tests)
