add_executable(pulsegate)

target_sources(
  pulsegate
  PRIVATE
    pulsegate_main.cpp
)
target_link_libraries(
  pulsegate
  PRIVATE
    PulseGate::core
)

add_executable(pulsegate_sync_baseline)

target_sources(
  pulsegate_sync_baseline
  PRIVATE
    pulsegate_sync_main.cpp
)
target_link_libraries(
  pulsegate_sync_baseline
  PRIVATE
    PulseGate::net
)
