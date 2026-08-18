include(FetchContent)

find_package(Threads REQUIRED)

# Boost.System has been header-only since Boost 1.69. Boost 1.89 removed its
# compatibility binary, so requiring the "system" component breaks on valid
# modern installations that intentionally provide no boost_system package.
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()
find_package(Boost 1.83 REQUIRED)
message(STATUS "PulseGate dependency: Boost ${Boost_VERSION} from the system package")

# Dependency policy:
# - Boost is supplied by the pinned Ubuntu 24.04 base used in CI and Docker.
# - Source dependencies below are owned by this project and must use immutable
#   revisions. Do not add a second package-manager source for the same library.
set(PULSEGATE_YAML_CPP_VERSION "0.8.0")
set(PULSEGATE_YAML_CPP_REVISION "f7320141120f720aecc4c32be25586e7da9eb978")
set(PULSEGATE_GOOGLETEST_VERSION "1.17.0")
set(PULSEGATE_GOOGLETEST_REVISION "52eb8108c5bdec04579160ae17225d66034bd723")
set(PULSEGATE_GOOGLETEST_SHA256 "745c55415660044610f7fcd3af7a6420d5de16a7dbb9ebfe2e131275676232be")

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
  GIT_TAG ${PULSEGATE_YAML_CPP_REVISION} # yaml-cpp ${PULSEGATE_YAML_CPP_VERSION}
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(yaml_cpp)
# yaml-cpp 0.8.0 emits -Wshadow diagnostics with the Clang version on the
# Ubuntu 24.04 GitHub runner. It is a third-party interface, so expose its
# headers as SYSTEM to keep -Werror focused on PulseGate source files.
get_target_property(PULSEGATE_YAML_CPP_INCLUDE_DIRS yaml-cpp INTERFACE_INCLUDE_DIRECTORIES)
if(PULSEGATE_YAML_CPP_INCLUDE_DIRS)
  set_property(
    TARGET yaml-cpp
    APPEND
    PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
      "${PULSEGATE_YAML_CPP_INCLUDE_DIRS}"
  )
endif()
message(
  STATUS
  "PulseGate dependency: yaml-cpp ${PULSEGATE_YAML_CPP_VERSION} (${PULSEGATE_YAML_CPP_REVISION})"
)

if(PULSEGATE_BUILD_TESTS)
  # Always fetch this hash-verified archive. A system GTest may be convenient,
  # but silently selecting it would make test behavior vary across developers.
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)

  FetchContent_Declare(
    googletest
    URL
      https://github.com/google/googletest/archive/${PULSEGATE_GOOGLETEST_REVISION}.tar.gz
    URL_HASH SHA256=${PULSEGATE_GOOGLETEST_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(googletest)
  message(
    STATUS
    "PulseGate dependency: GoogleTest ${PULSEGATE_GOOGLETEST_VERSION} (${PULSEGATE_GOOGLETEST_REVISION})"
  )
endif()
