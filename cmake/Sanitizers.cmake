add_library(pulsegate_sanitizers INTERFACE)
add_library(PulseGate::sanitizers ALIAS pulsegate_sanitizers)

if(PULSEGATE_ENABLE_TSAN AND
   (PULSEGATE_ENABLE_ASAN OR PULSEGATE_ENABLE_UBSAN))
  message(
    FATAL_ERROR
    "TSan must use a separate build from ASan/UBSan. Select only one preset."
  )
endif()

if(PULSEGATE_ENABLE_ASAN OR
   PULSEGATE_ENABLE_UBSAN OR
   PULSEGATE_ENABLE_TSAN)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "The sanitizer presets currently support GCC and Clang")
  endif()

  set(_pulsegate_sanitize_flags)

  if(PULSEGATE_ENABLE_ASAN AND PULSEGATE_ENABLE_UBSAN)
    list(APPEND _pulsegate_sanitize_flags -fsanitize=address,undefined)
  elseif(PULSEGATE_ENABLE_ASAN)
    list(APPEND _pulsegate_sanitize_flags -fsanitize=address)
  elseif(PULSEGATE_ENABLE_UBSAN)
    list(APPEND _pulsegate_sanitize_flags -fsanitize=undefined)
  elseif(PULSEGATE_ENABLE_TSAN)
    list(APPEND _pulsegate_sanitize_flags -fsanitize=thread)
  endif()

  target_compile_options(
    pulsegate_sanitizers
    INTERFACE
      ${_pulsegate_sanitize_flags}
      -fno-omit-frame-pointer
      -fno-sanitize-recover=all
  )
  target_link_options(
    pulsegate_sanitizers
    INTERFACE
      ${_pulsegate_sanitize_flags}
      -fno-omit-frame-pointer
      -fno-sanitize-recover=all
  )

  if(PULSEGATE_ENABLE_TSAN)
    # GCC warns for the atomic fences used internally by Boost.Asio even
    # though the instrumented program remains valid. Keep project builds
    # warning-clean without weakening diagnostics in non-TSan presets.
    target_compile_options(
      pulsegate_sanitizers
      INTERFACE
        "$<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wno-tsan>"
    )
  endif()

  unset(_pulsegate_sanitize_flags)
endif()
