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
