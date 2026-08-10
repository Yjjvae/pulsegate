#pragma once

#include <boost/asio/strand.hpp>
#include <memory>

#include "pulsegate/http/upstream_endpoint.h"
#include "pulsegate/net/asio_types.h"

namespace pulsegate::http {

// Owns all mutable transport objects for one reusable upstream TCP connection.
// Every socket operation must bind its completion handler to executor().
class UpstreamConnection : public std::enable_shared_from_this<UpstreamConnection> {
   public:
    UpstreamConnection(net::asio::any_io_executor executor, UpstreamEndpoint endpoint);

    [[nodiscard]] const UpstreamEndpoint& endpoint() const noexcept;
    [[nodiscard]] net::asio::any_io_executor executor() const noexcept;
    [[nodiscard]] net::tcp::resolver& resolver() noexcept;
    [[nodiscard]] net::tcp::socket& socket() noexcept;
    [[nodiscard]] bool isOpen() const noexcept;
    void cancelAndClose();

   private:
    void cancelAndCloseInStrand();

    UpstreamEndpoint endpoint_;
    net::asio::strand<net::asio::any_io_executor> strand_;
    net::tcp::resolver resolver_;
    net::tcp::socket socket_;
};

}  // namespace pulsegate::http
