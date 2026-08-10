#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "pulsegate/core/version.h"
#include "pulsegate/http/http_server.h"

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

void printUsage(std::ostream& output) {
    output << "PulseGate " << pulsegate::core::version() << '\n'
           << "Usage: pulsegate [--listen HOST:PORT] [--help | --version]\n\n"
           << "Single-threaded coroutine HTTP server.\n"
           << "Example: pulsegate --listen 127.0.0.1:8080\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        pulsegate::net::ListenConfig config;
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
            throw std::invalid_argument("unknown or incomplete argument: " + std::string(argument));
        }

        boost::asio::io_context context(1);
        pulsegate::http::HttpServer server(context, config);
        const auto endpoint = server.localEndpoint();
        std::cout << "PulseGate listening on " << endpoint.address().to_string() << ':'
                  << endpoint.port() << '\n';

        boost::asio::signal_set signals(context, SIGINT, SIGTERM);
        signals.async_wait([&server](const boost::system::error_code&, int) { server.stop(); });
        server.start();
        context.run();
    } catch (const std::exception& error) {
        std::cerr << "PulseGate failed: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
