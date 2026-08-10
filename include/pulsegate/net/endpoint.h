#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>
#include <cstdint>
#include <string>

namespace pulsegate::net {

struct ListenConfig {
    std::string host{"127.0.0.1"};
    std::uint16_t port{8080};
    int backlog{1024};
};

[[nodiscard]] boost::asio::ip::tcp::endpoint makeEndpoint(const ListenConfig& config,
                                                          boost::system::error_code& error);

}  // namespace pulsegate::net
