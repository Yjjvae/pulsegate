#include <algorithm>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pulsegate/core/version.h"
#include "pulsegate/http/config.h"
#include "pulsegate/http/health_checker.h"
#include "pulsegate/http/http_server.h"
#include "pulsegate/http/reverse_proxy.h"
#include "pulsegate/http/static_file_handler.h"
#include "pulsegate/runtime/asio_runtime.h"

namespace {

pulsegate::net::ListenConfig parseListenAddress(std::string_view value) {
    pulsegate::net::ListenConfig config;

    std::string_view host;
    std::string_view port_text;
    if (value.starts_with('[')) {
        const auto closing_bracket = value.find(']');
        if (closing_bracket == std::string_view::npos || closing_bracket + 1 >= value.size() ||
            value[closing_bracket + 1] != ':') {
            throw std::invalid_argument("IPv6 listen addresses must look like [::1]:8080");
        }
        host = value.substr(1, closing_bracket - 1);
        port_text = value.substr(closing_bracket + 2);
    } else {
        const auto colon = value.rfind(':');
        if (colon == std::string_view::npos) {
            throw std::invalid_argument("listen address must look like HOST:PORT");
        }
        host = value.substr(0, colon);
        port_text = value.substr(colon + 1);
    }

    if (host.empty() || port_text.empty()) {
        throw std::invalid_argument("listen host and port must not be empty");
    }

    std::uint32_t port = 0;
    const auto [end, error] =
        std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (error != std::errc{} || end != port_text.data() + port_text.size() || port > 65535) {
        throw std::invalid_argument("listen port must be between 0 and 65535");
    }

    config.host = std::string(host);
    config.port = static_cast<std::uint16_t>(port);
    return config;
}

pulsegate::http::UpstreamEndpoint parseUpstreamEndpoint(std::string_view value) {
    std::string_view host;
    std::string_view service;
    if (value.starts_with('[')) {
        const auto closing_bracket = value.find(']');
        if (closing_bracket == std::string_view::npos || closing_bracket + 1 >= value.size() ||
            value[closing_bracket + 1] != ':') {
            throw std::invalid_argument("IPv6 upstream addresses must look like [::1]:8081");
        }
        host = value.substr(1, closing_bracket - 1);
        service = value.substr(closing_bracket + 2);
    } else {
        const auto colon = value.rfind(':');
        if (colon == std::string_view::npos) {
            throw std::invalid_argument("upstream address must look like HOST:PORT");
        }
        host = value.substr(0, colon);
        service = value.substr(colon + 1);
    }
    if (host.empty() || service.empty()) {
        throw std::invalid_argument("upstream host and port must not be empty");
    }
    return {.host = std::string(host), .service = std::string(service)};
}

std::size_t parseThreadCount(std::string_view value) {
    std::uint32_t count = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), count);
    if (value.empty() || error != std::errc{} || end != value.data() + value.size() || count == 0) {
        throw std::invalid_argument("thread count must be a positive integer");
    }
    return count;
}

std::size_t parsePositiveSize(std::string_view value, std::string_view option) {
    std::uint64_t parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (value.empty() || error != std::errc{} || end != value.data() + value.size() ||
        parsed == 0 || parsed > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument(std::string(option) + " must be a positive integer");
    }
    return static_cast<std::size_t>(parsed);
}

std::size_t defaultThreadCount() {
    const auto detected = std::thread::hardware_concurrency();
    return detected == 0 ? 1U : static_cast<std::size_t>(detected);
}

double parsePositiveDouble(std::string_view value, std::string_view option) {
    std::size_t consumed = 0;
    double parsed = 0.0;
    try {
        parsed = std::stod(std::string(value), &consumed);
    } catch (const std::exception&) {
        throw std::invalid_argument(std::string(option) + " must be a positive number");
    }
    if (consumed != value.size() || !std::isfinite(parsed) || parsed <= 0.0) {
        throw std::invalid_argument(std::string(option) + " must be a positive number");
    }
    return parsed;
}

void printUsage(std::ostream& output) {
    output << "PulseGate " << pulsegate::core::version() << '\n'
           << "Usage: pulsegate [--config FILE | --listen HOST:PORT] [--threads N] "
              "[--document-root PATH] "
              "[--proxy-upstream HOST:PORT] "
              "[--rate-limit RPS --rate-burst N [--rate-per-client]] "
              "[--proxy-rate-limit RPS --proxy-rate-burst N [--proxy-rate-per-client]] "
              "[--proxy-cache-ttl-ms N --proxy-cache-max-bytes N "
              "[--proxy-cache-entry-max-bytes N] [--proxy-cache-shards N]] "
              "[--help | --version]\n\n"
           << "Multi-threaded Boost.Asio coroutine HTTP server.\n"
           << "Example: pulsegate --listen 127.0.0.1:8080 --threads 4\n";
}

pulsegate::http::SessionLimits sessionLimitsFromConfig(
    const pulsegate::http::ServerConfig& config) {
    pulsegate::http::SessionLimits limits;
    limits.parser.max_header_bytes = config.max_header_bytes;
    limits.parser.max_body_bytes = config.max_body_bytes;
    limits.max_buffer_bytes =
        std::max(config.max_body_bytes + limits.parser.max_header_bytes, limits.read_chunk_bytes);
    limits.header_timeout = config.header_timeout;
    limits.body_timeout = config.body_timeout;
    limits.idle_timeout = config.idle_timeout;
    limits.max_connections = config.max_connections;
    return limits;
}

int runFromConfig(const std::filesystem::path& path) {
    pulsegate::http::ConfigLoader loader;
    const auto loaded = loader.loadFromFile(path);
    auto initial =
        std::make_shared<const pulsegate::http::ConfigSnapshot>(pulsegate::http::ConfigSnapshot{
            .value = loaded, .source_path = path, .loaded_at = std::chrono::system_clock::now()});
    pulsegate::runtime::AsioRuntime runtime(loaded.server.io_threads);
    auto manager = std::make_shared<pulsegate::http::ConfigManager>(
        runtime.context().get_executor(), path, initial);
    auto router = pulsegate::http::HttpServer::makeDefaultRouter();
    std::vector<std::shared_ptr<pulsegate::http::HealthChecker>> health_checkers;
    std::vector<pulsegate::http::ReverseProxy> reverse_proxies;
    std::unordered_map<std::string, pulsegate::http::ReverseProxy> proxies;
    for (const auto& upstream : loaded.upstreams) {
        pulsegate::http::ReverseProxy proxy(upstream.endpoints, upstream.proxy_limits);
        auto checker = std::make_shared<pulsegate::http::HealthChecker>(
            runtime.context().get_executor(), pulsegate::http::HealthCheckConfig{},
            upstream.endpoints, proxy.health());
        checker->start();
        health_checkers.push_back(std::move(checker));
        reverse_proxies.push_back(proxy);
        proxies.emplace(upstream.name, std::move(proxy));
    }
    for (const auto& route : loaded.routes) {
        const auto found = proxies.find(route.upstream);
        if (found == proxies.end()) {
            throw std::logic_error("validated route references missing upstream");
        }
        const auto add = [&router, &route,
                          proxy = found->second](pulsegate::http::HttpMethod method) {
            router->add(
                pulsegate::http::Route{
                    .method = method,
                    .pattern = route.path_prefix,
                    .name = "config:" + route.path_prefix,
                    .prefix_match = true,
                    .handler = [proxy](pulsegate::http::RequestContext& context,
                                       pulsegate::http::HttpRequest request)
                        -> pulsegate::net::Awaitable<pulsegate::http::HttpResponse> {
                        co_return co_await proxy(context, std::move(request));
                    }},
                route.rate_limit, route.cache);
        };
        add(pulsegate::http::HttpMethod::Get);
        add(pulsegate::http::HttpMethod::Head);
        add(pulsegate::http::HttpMethod::Post);
        add(pulsegate::http::HttpMethod::Put);
        add(pulsegate::http::HttpMethod::Delete);
    }
    const pulsegate::net::ListenConfig listen{
        .host = loaded.server.listen_host,
        .port = static_cast<std::uint16_t>(loaded.server.listen_port)};
    auto observability = std::make_shared<pulsegate::http::Observability>(
        pulsegate::http::LoggerConfig{.level = pulsegate::http::parseLogLevel(loaded.logging.level),
                                      .json = loaded.logging.format == "json"});
    pulsegate::http::HttpServer server(runtime.context(), listen,
                                       pulsegate::http::RouterConfig{std::move(router)},
                                       sessionLimitsFromConfig(loaded.server), observability);
    const auto endpoint = server.localEndpoint();
    const auto graceful_shutdown = loaded.server.graceful_shutdown;
    std::cout << "PulseGate listening on " << endpoint.address().to_string() << ':'
              << endpoint.port() << " (config " << path.string() << ")\n";

    auto signals =
        std::make_shared<boost::asio::signal_set>(runtime.context(), SIGINT, SIGTERM, SIGHUP);
    auto wait_signal = std::make_shared<std::function<void()>>();
    *wait_signal = [signals, wait_signal, manager, observability, graceful_shutdown, &server,
                    &runtime, &health_checkers, &reverse_proxies] {
        signals->async_wait([signals, wait_signal, manager, observability, graceful_shutdown,
                             &server, &runtime, &health_checkers,
                             &reverse_proxies](const boost::system::error_code& error, int signal) {
            if (error) return;
            if (signal == SIGHUP) {
                manager->requestReload([manager,
                                        observability](pulsegate::http::ConfigReloadResult result) {
                    if (result.published) {
                        const auto logging = manager->snapshot()->value.logging;
                        observability->setLogging(pulsegate::http::parseLogLevel(logging.level),
                                                  logging.format == "json");
                        std::cerr << "PulseGate configuration snapshot reloaded\n";
                    } else if (!result.restart_required.empty()) {
                        std::cerr << "PulseGate configuration change requires restart:";
                        for (const auto& field : result.restart_required) std::cerr << ' ' << field;
                        std::cerr << '\n';
                    } else {
                        for (const auto& issue : result.errors) {
                            std::cerr << "PulseGate configuration reload failed: " << issue.format()
                                      << '\n';
                        }
                    }
                });
                (*wait_signal)();
                return;
            }
            server.beginDrain(graceful_shutdown,
                              [&runtime, &health_checkers, &reverse_proxies](bool) {
                                  for (const auto& checker : health_checkers) checker->stop();
                                  for (const auto& proxy : reverse_proxies) proxy.stop();
                                  runtime.requestStop();
                              });
        });
    };
    (*wait_signal)();
    server.start();
    runtime.start();
    runtime.join();
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        pulsegate::net::ListenConfig config;
        std::size_t thread_count = defaultThreadCount();
        std::optional<std::filesystem::path> document_root;
        std::optional<std::filesystem::path> config_path;
        std::vector<pulsegate::http::UpstreamEndpoint> proxy_upstreams;
        std::optional<double> rate_limit;
        std::optional<double> rate_burst;
        bool rate_per_client = false;
        std::optional<double> proxy_rate_limit;
        std::optional<double> proxy_rate_burst;
        bool proxy_rate_per_client = false;
        std::optional<std::size_t> proxy_cache_ttl_ms;
        std::optional<std::size_t> proxy_cache_max_bytes;
        std::optional<std::size_t> proxy_cache_entry_max_bytes;
        std::optional<std::size_t> proxy_cache_shards;
        std::vector<std::shared_ptr<pulsegate::http::HealthChecker>> health_checkers;
        std::vector<pulsegate::http::ReverseProxy> reverse_proxies;
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument(argv[index]);
            if (argument == "--version") {
                if (argc != 2) {
                    throw std::invalid_argument(
                        "--version cannot be combined with other arguments");
                }
                std::cout << pulsegate::core::version() << '\n';
                return 0;
            }
            if (argument == "--help" || argument == "-h") {
                printUsage(std::cout);
                return 0;
            }
            if (argument == "--listen" && index + 1 < argc) {
                config = parseListenAddress(argv[++index]);
                continue;
            }
            if (argument == "--config" && index + 1 < argc) {
                config_path = argv[++index];
                continue;
            }
            if (argument == "--threads" && index + 1 < argc) {
                thread_count = parseThreadCount(argv[++index]);
                continue;
            }
            if (argument == "--document-root" && index + 1 < argc) {
                document_root = argv[++index];
                continue;
            }
            if (argument == "--proxy-upstream" && index + 1 < argc) {
                proxy_upstreams.push_back(parseUpstreamEndpoint(argv[++index]));
                continue;
            }
            if (argument == "--rate-limit" && index + 1 < argc) {
                rate_limit = parsePositiveDouble(argv[++index], "--rate-limit");
                continue;
            }
            if (argument == "--rate-burst" && index + 1 < argc) {
                rate_burst = parsePositiveDouble(argv[++index], "--rate-burst");
                continue;
            }
            if (argument == "--rate-per-client") {
                rate_per_client = true;
                continue;
            }
            if (argument == "--proxy-rate-limit" && index + 1 < argc) {
                proxy_rate_limit = parsePositiveDouble(argv[++index], "--proxy-rate-limit");
                continue;
            }
            if (argument == "--proxy-rate-burst" && index + 1 < argc) {
                proxy_rate_burst = parsePositiveDouble(argv[++index], "--proxy-rate-burst");
                continue;
            }
            if (argument == "--proxy-rate-per-client") {
                proxy_rate_per_client = true;
                continue;
            }
            if (argument == "--proxy-cache-ttl-ms" && index + 1 < argc) {
                proxy_cache_ttl_ms = parsePositiveSize(argv[++index], "--proxy-cache-ttl-ms");
                continue;
            }
            if (argument == "--proxy-cache-max-bytes" && index + 1 < argc) {
                proxy_cache_max_bytes = parsePositiveSize(argv[++index], "--proxy-cache-max-bytes");
                continue;
            }
            if (argument == "--proxy-cache-entry-max-bytes" && index + 1 < argc) {
                proxy_cache_entry_max_bytes =
                    parsePositiveSize(argv[++index], "--proxy-cache-entry-max-bytes");
                continue;
            }
            if (argument == "--proxy-cache-shards" && index + 1 < argc) {
                proxy_cache_shards = parsePositiveSize(argv[++index], "--proxy-cache-shards");
                continue;
            }
            throw std::invalid_argument("unknown or incomplete argument: " + std::string(argument));
        }

        if (config_path) {
            if (argc != 3) {
                throw std::invalid_argument("--config cannot be combined with other options");
            }
            return runFromConfig(*config_path);
        }

        if (rate_limit.has_value() != rate_burst.has_value()) {
            throw std::invalid_argument("--rate-limit and --rate-burst must be used together");
        }
        if (proxy_rate_limit.has_value() != proxy_rate_burst.has_value()) {
            throw std::invalid_argument(
                "--proxy-rate-limit and --proxy-rate-burst must be used together");
        }
        if (proxy_cache_ttl_ms.has_value() != proxy_cache_max_bytes.has_value()) {
            throw std::invalid_argument(
                "--proxy-cache-ttl-ms and --proxy-cache-max-bytes must be used together");
        }
        if ((proxy_cache_entry_max_bytes || proxy_cache_shards) && !proxy_cache_ttl_ms) {
            throw std::invalid_argument(
                "proxy cache entry and shard options require --proxy-cache-ttl-ms and "
                "--proxy-cache-max-bytes");
        }
        std::optional<pulsegate::http::RateLimitConfig> global_rate_limit;
        if (rate_limit) {
            global_rate_limit = pulsegate::http::RateLimitConfig{.requests_per_second = *rate_limit,
                                                                 .burst = *rate_burst,
                                                                 .per_client = rate_per_client};
        }
        std::optional<pulsegate::http::RateLimitConfig> proxy_route_limit;
        if (proxy_rate_limit) {
            proxy_route_limit =
                pulsegate::http::RateLimitConfig{.requests_per_second = *proxy_rate_limit,
                                                 .burst = *proxy_rate_burst,
                                                 .per_client = proxy_rate_per_client};
        }
        std::optional<pulsegate::http::ResponseCacheConfig> proxy_cache;
        if (proxy_cache_ttl_ms) {
            const auto max_bytes = *proxy_cache_max_bytes;
            const auto max_entry_bytes =
                proxy_cache_entry_max_bytes.value_or(std::min<std::size_t>(256 * 1024, max_bytes));
            const auto shards =
                proxy_cache_shards.value_or(std::min<std::size_t>(16, max_bytes / max_entry_bytes));
            if (max_entry_bytes > max_bytes) {
                throw std::invalid_argument(
                    "--proxy-cache-entry-max-bytes cannot exceed cache size");
            }
            if (max_entry_bytes > max_bytes / shards) {
                throw std::invalid_argument(
                    "--proxy-cache-entry-max-bytes is too large for the selected shard count");
            }
            proxy_cache = pulsegate::http::ResponseCacheConfig{
                .ttl = std::chrono::milliseconds(*proxy_cache_ttl_ms),
                .max_entry_bytes = max_entry_bytes,
                .max_bytes = max_bytes,
                .shard_count = shards,
                .vary_headers = {}};
        }

        pulsegate::runtime::AsioRuntime runtime(thread_count);
        auto router = pulsegate::http::HttpServer::makeDefaultRouter(global_rate_limit);
        if (document_root) {
            auto files = std::make_shared<pulsegate::http::BoundedFileService>(*document_root);
            pulsegate::http::StaticFileHandler static_files(*document_root, files);
            const auto add_static = [&router, static_files](pulsegate::http::HttpMethod method) {
                router->add(pulsegate::http::Route{
                    .method = method,
                    .pattern = "/static/",
                    .name = "static_files",
                    .prefix_match = true,
                    .handler = [static_files](pulsegate::http::RequestContext& context,
                                              pulsegate::http::HttpRequest request)
                        -> pulsegate::net::Awaitable<pulsegate::http::HttpResponse> {
                        co_return co_await static_files(context, std::move(request));
                    }});
            };
            add_static(pulsegate::http::HttpMethod::Get);
            add_static(pulsegate::http::HttpMethod::Head);
        }
        if (!proxy_upstreams.empty()) {
            auto upstreams = std::move(proxy_upstreams);
            pulsegate::http::ReverseProxy proxy(upstreams);
            auto checker = std::make_shared<pulsegate::http::HealthChecker>(
                runtime.context().get_executor(), pulsegate::http::HealthCheckConfig{}, upstreams,
                proxy.health());
            checker->start();
            health_checkers.push_back(std::move(checker));
            reverse_proxies.push_back(proxy);
            const auto add_proxy = [&router, proxy, proxy_route_limit,
                                    proxy_cache](pulsegate::http::HttpMethod method) {
                router->add(
                    pulsegate::http::Route{
                        .method = method,
                        .pattern = "/proxy/",
                        .name = "reverse_proxy",
                        .prefix_match = true,
                        .handler = [proxy](pulsegate::http::RequestContext& context,
                                           pulsegate::http::HttpRequest request)
                            -> pulsegate::net::Awaitable<pulsegate::http::HttpResponse> {
                            co_return co_await proxy(context, std::move(request));
                        }},
                    proxy_route_limit, proxy_cache);
            };
            add_proxy(pulsegate::http::HttpMethod::Get);
            add_proxy(pulsegate::http::HttpMethod::Head);
            add_proxy(pulsegate::http::HttpMethod::Post);
            add_proxy(pulsegate::http::HttpMethod::Put);
            add_proxy(pulsegate::http::HttpMethod::Delete);
        }
        pulsegate::http::HttpServer server(runtime.context(), config,
                                           pulsegate::http::RouterConfig{std::move(router)});
        const auto endpoint = server.localEndpoint();
        std::cout << "PulseGate listening on " << endpoint.address().to_string() << ':'
                  << endpoint.port() << '\n';

        boost::asio::signal_set signals(runtime.context(), SIGINT, SIGTERM);
        signals.async_wait([&server, &runtime, &health_checkers, &reverse_proxies](
                               const boost::system::error_code&, int) {
            server.beginDrain(std::chrono::seconds(15),
                              [&runtime, &health_checkers, &reverse_proxies](bool) {
                                  for (const auto& checker : health_checkers) checker->stop();
                                  for (const auto& proxy : reverse_proxies) proxy.stop();
                                  runtime.requestStop();
                              });
        });
        server.start();
        runtime.start();
        runtime.join();
    } catch (const std::exception& error) {
        std::cerr << "PulseGate failed: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
