add_executable(pulsegate_unit_tests)

target_sources(
  pulsegate_unit_tests
  PRIVATE
    unit/smoke_test.cpp
)
target_link_libraries(
  pulsegate_unit_tests
  PRIVATE
    PulseGate::core
    PulseGate::runtime
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(
  pulsegate_unit_tests
  DISCOVERY_MODE PRE_TEST
)

add_executable(pulsegate_integration_tests)

target_sources(
  pulsegate_integration_tests
  PRIVATE
    integration/sync_http_server_test.cpp
)
target_link_libraries(
  pulsegate_integration_tests
  PRIVATE
    PulseGate::net
    GTest::gtest_main
)

gtest_discover_tests(
  pulsegate_integration_tests
  DISCOVERY_MODE PRE_TEST
)
