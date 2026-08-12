add_executable(pulsegate_http_parser_fuzz http_parser_fuzz.cpp)

target_link_libraries(
  pulsegate_http_parser_fuzz
  PRIVATE
    PulseGate::http
)

target_compile_options(
  pulsegate_http_parser_fuzz
  PRIVATE
    -fsanitize=fuzzer
)
target_link_options(
  pulsegate_http_parser_fuzz
  PRIVATE
    -fsanitize=fuzzer
)
