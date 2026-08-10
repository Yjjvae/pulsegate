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

add_library(pulsegate_net STATIC)
add_library(PulseGate::net ALIAS pulsegate_net)

target_sources(
  pulsegate_net
  PRIVATE
    net/buffer.cpp
    net/endpoint.cpp
    net/listener.cpp
    net/sync_http_server.cpp
)
target_include_directories(
  pulsegate_net
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(
  pulsegate_net
  PUBLIC
    PulseGate::runtime
)

add_library(pulsegate_http STATIC)
add_library(PulseGate::http ALIAS pulsegate_http)

target_sources(
  pulsegate_http
  PRIVATE
    http/headers.cpp
    http/http_parser.cpp
    http/http_request.cpp
    http/http_response.cpp
    http/http_server.cpp
)
target_include_directories(
  pulsegate_http
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(
  pulsegate_http
  PUBLIC
    PulseGate::net
)
