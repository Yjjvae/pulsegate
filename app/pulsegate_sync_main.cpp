#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>
#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "pulsegate/net/sync_http_server.h"

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

void printUsage(std::string_view program) {
    std::cout << "Usage: " << program << " [--listen HOST:PORT]\n"
              << "Example: " << program << " --listen 127.0.0.1:8080\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        pulsegate::net::ListenConfig config;
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument(argv[index]);
            if (argument == "--help" || argument == "-h") {
                printUsage(argv[0]);
                return 0;
            }
            if (argument == "--listen" && index + 1 < argc) {
                config = parseListenAddress(argv[++index]);
                continue;
            }
            throw std::invalid_argument("unknown or incomplete argument: " + std::string(argument));
        }

        boost::asio::io_context io_context;
        pulsegate::net::SyncHttpServer server(io_context, config);
        const auto endpoint = server.localEndpoint();
        std::cout << "PulseGate synchronous baseline listening on "
                  << endpoint.address().to_string() << ':' << endpoint.port() << '\n';

        boost::system::error_code error;
        server.run(error);
        if (error) {
            std::cerr << "Accept loop stopped: " << error.message() << '\n';
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << "Failed to start PulseGate: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
