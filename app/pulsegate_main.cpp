#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <charconv>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "pulsegate/core/version.h"
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

std::size_t defaultThreadCount() {
    const auto detected = std::thread::hardware_concurrency();
    return detected == 0 ? 1U : static_cast<std::size_t>(detected);
}

void printUsage(std::ostream& output) {
    output << "PulseGate " << pulsegate::core::version() << '\n'
           << "Usage: pulsegate [--listen HOST:PORT] [--threads N] [--document-root PATH] "
              "[--proxy-upstream HOST:PORT] "
              "[--help | --version]\n\n"
           << "Multi-threaded Boost.Asio coroutine HTTP server.\n"
           << "Example: pulsegate --listen 127.0.0.1:8080 --threads 4\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        pulsegate::net::ListenConfig config;
        std::size_t thread_count = defaultThreadCount();
        std::optional<std::filesystem::path> document_root;
        std::vector<pulsegate::http::UpstreamEndpoint> proxy_upstreams;
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
            throw std::invalid_argument("unknown or incomplete argument: " + std::string(argument));
        }

        pulsegate::runtime::AsioRuntime runtime(thread_count);
        auto router = pulsegate::http::HttpServer::makeDefaultRouter();
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
            const auto add_proxy = [&router, proxy](pulsegate::http::HttpMethod method) {
                router->add(pulsegate::http::Route{
                    .method = method,
                    .pattern = "/proxy/",
                    .name = "reverse_proxy",
                    .prefix_match = true,
                    .handler = [proxy](pulsegate::http::RequestContext& context,
                                       pulsegate::http::HttpRequest request)
                        -> pulsegate::net::Awaitable<pulsegate::http::HttpResponse> {
                        co_return co_await proxy(context, std::move(request));
                    }});
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
            for (const auto& checker : health_checkers) {
                checker->stop();
            }
            for (const auto& proxy : reverse_proxies) {
                proxy.stop();
            }
            server.stop();
            runtime.requestStop();
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
