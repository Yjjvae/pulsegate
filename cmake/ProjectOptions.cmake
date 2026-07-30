add_library(pulsegate_project_options INTERFACE)
add_library(PulseGate::project_options ALIAS pulsegate_project_options)

target_compile_features(pulsegate_project_options INTERFACE cxx_std_20)

target_compile_options(
  pulsegate_project_options
  INTERFACE
    "$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow;-Wformat=2>"
    "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W4;/permissive->"
)

if(PULSEGATE_WARNINGS_AS_ERRORS)
  target_compile_options(
    pulsegate_project_options
    INTERFACE
      "$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Werror>"
      "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/WX>"
  )
endif()
