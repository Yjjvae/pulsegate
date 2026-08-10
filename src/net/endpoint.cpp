#include "pulsegate/net/endpoint.h"

#include <boost/asio/ip/address.hpp>

namespace pulsegate::net {

boost::asio::ip::tcp::endpoint makeEndpoint(const ListenConfig& config,
                                            boost::system::error_code& error) {
    const auto address = boost::asio::ip::make_address(config.host, error);
    if (error) {
        return {};
    }

    return {address, config.port};
}

}  // namespace pulsegate::net
