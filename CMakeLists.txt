cmake_minimum_required(VERSION 3.24)

project(
  PulseGate
  VERSION 1.0.0
  DESCRIPTION "A Boost.Asio based HTTP gateway"
  LANGUAGES CXX
)

option(PULSEGATE_BUILD_TESTS "Build PulseGate tests" ON)
option(PULSEGATE_BUILD_FUZZERS "Build libFuzzer targets (Clang only)" OFF)
option(PULSEGATE_WARNINGS_AS_ERRORS "Treat project warnings as errors" OFF)
option(PULSEGATE_ENABLE_ASIO_HANDLER_TRACKING "Enable verbose Boost.Asio handler tracing" OFF)
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

if(PULSEGATE_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "PULSEGATE_BUILD_FUZZERS requires Clang and libFuzzer")
  endif()
  if(NOT (PULSEGATE_ENABLE_ASAN AND PULSEGATE_ENABLE_UBSAN))
    message(FATAL_ERROR "PULSEGATE_BUILD_FUZZERS requires ASan and UBSan")
  endif()
  execute_process(
    COMMAND "${CMAKE_CXX_COMPILER}" -print-resource-dir
    OUTPUT_VARIABLE PULSEGATE_CLANG_RESOURCE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
  )
  file(
    GLOB_RECURSE PULSEGATE_LIBFUZZER_RUNTIMES
    "${PULSEGATE_CLANG_RESOURCE_DIR}/lib*/libclang_rt.fuzzer-*.a"
  )
  if(NOT PULSEGATE_LIBFUZZER_RUNTIMES)
    message(
      FATAL_ERROR
      "Clang libFuzzer runtime is missing. Install the compiler-rt development package, for example: sudo apt install libclang-rt-21-dev"
    )
  endif()
  add_subdirectory(tests/fuzz)
endif()
