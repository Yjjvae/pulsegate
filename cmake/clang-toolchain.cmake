# Use the active GNU toolchain's C++ runtime with Clang. Some Linux hosts keep
# a newer GCC runtime directory without the matching libstdc++ development
# library; asking Clang to use the active g++ runtime keeps Clang builds
# reproducible without hard-coding a GCC version.
find_program(PULSEGATE_CLANGXX NAMES clang++-21 clang++ REQUIRED)
find_program(PULSEGATE_GXX NAMES g++ c++ REQUIRED)

execute_process(
  COMMAND "${PULSEGATE_GXX}" -print-libgcc-file-name
  OUTPUT_VARIABLE PULSEGATE_LIBGCC
  OUTPUT_STRIP_TRAILING_WHITESPACE
  COMMAND_ERROR_IS_FATAL ANY
)
get_filename_component(PULSEGATE_GCC_INSTALL_DIR "${PULSEGATE_LIBGCC}" DIRECTORY)

set(CMAKE_CXX_COMPILER "${PULSEGATE_CLANGXX}" CACHE FILEPATH "" FORCE)
set(
  CMAKE_CXX_COMPILER_ARG1
  "--gcc-install-dir=${PULSEGATE_GCC_INSTALL_DIR}"
  CACHE STRING ""
  FORCE
)
