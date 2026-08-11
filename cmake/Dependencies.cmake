include(FetchContent)

find_package(Threads REQUIRED)

# Boost.System has been header-only since Boost 1.69. Boost 1.89 removed its
# compatibility binary, so requiring the "system" component breaks on valid
# modern installations that intentionally provide no boost_system package.
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()
find_package(Boost 1.83 REQUIRED)

# Configuration is parsed through one project-owned boundary. Pinning a commit
# avoids silently changing YAML semantics when a moving dependency tag changes.
# yaml-cpp 0.8.0 still declares compatibility with CMake 3.5; CMake 4 requires
# this explicit policy floor when configuring that third-party project.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  yaml_cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG f7320141120f720aecc4c32be25586e7da9eb978 # yaml-cpp 0.8.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(yaml_cpp)

if(PULSEGATE_BUILD_TESTS)
  find_package(GTest CONFIG QUIET)

  if(NOT TARGET GTest::gtest_main)
    message(STATUS "System GoogleTest not found; fetching pinned GoogleTest 1.17.0")

    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
      googletest
      URL
        https://github.com/google/googletest/archive/52eb8108c5bdec04579160ae17225d66034bd723.tar.gz
      URL_HASH
        SHA256=745c55415660044610f7fcd3af7a6420d5de16a7dbb9ebfe2e131275676232be
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(googletest)
  endif()
endif()
