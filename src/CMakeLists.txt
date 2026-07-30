add_library(pulsegate_core STATIC)
add_library(PulseGate::core ALIAS pulsegate_core)

target_sources(
  pulsegate_core
  PRIVATE
    core/version.cpp
)
target_include_directories(
  pulsegate_core
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(
  pulsegate_core
  PUBLIC
    PulseGate::project_options
    PulseGate::sanitizers
)

# Stage 0 owns only the dependency boundary. Concrete runtime sources arrive in
# later chapters, while users can already link against one stable project target.
add_library(pulsegate_runtime INTERFACE)
add_library(PulseGate::runtime ALIAS pulsegate_runtime)

target_include_directories(
  pulsegate_runtime
  INTERFACE
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(
  pulsegate_runtime
  INTERFACE
    Boost::headers
    Threads::Threads
    PulseGate::project_options
    PulseGate::sanitizers
)
target_compile_definitions(
  pulsegate_runtime
  INTERFACE
    BOOST_ASIO_NO_DEPRECATED
)
