cmake_minimum_required(VERSION 3.24)

project(
  PulseGate
  VERSION 0.6.0
  DESCRIPTION "A Boost.Asio based HTTP gateway"
  LANGUAGES CXX
)

option(PULSEGATE_BUILD_TESTS "Build PulseGate tests" ON)
option(PULSEGATE_WARNINGS_AS_ERRORS "Treat project warnings as errors" OFF)
option(PULSEGATE_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(PULSEGATE_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(PULSEGATE_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(ProjectOptions)
include(Sanitizers)
include(Dependencies)

add_subdirectory(src)
add_subdirectory(app)

if(PULSEGATE_BUILD_TESTS)
  include(CTest)
  enable_testing()
  add_subdirectory(tests)
endif()
