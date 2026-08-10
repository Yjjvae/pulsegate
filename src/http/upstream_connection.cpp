#include "pulsegate/http/upstream_connection.h"

#include <boost/asio/dispatch.hpp>
#include <utility>

namespace pulsegate::http {

UpstreamConnection::UpstreamConnection(net::asio::any_io_executor executor,
                                       UpstreamEndpoint endpoint)
    : endpoint_(std::move(endpoint)),
      strand_(net::asio::make_strand(std::move(executor))),
      resolver_(strand_),
      socket_(strand_) {}

const UpstreamEndpoint& UpstreamConnection::endpoint() const noexcept {
    return endpoint_;
}

net::asio::any_io_executor UpstreamConnection::executor() const noexcept {
    return strand_;
}

net::tcp::resolver& UpstreamConnection::resolver() noexcept {
    return resolver_;
}

net::tcp::socket& UpstreamConnection::socket() noexcept {
    return socket_;
}

bool UpstreamConnection::isOpen() const noexcept {
    return socket_.is_open();
}

void UpstreamConnection::cancelAndClose() {
    const auto self = shared_from_this();
    net::asio::dispatch(strand_, [self] { self->cancelAndCloseInStrand(); });
}

void UpstreamConnection::cancelAndCloseInStrand() {
    resolver_.cancel();
    net::ErrorCode ignored;
    socket_.cancel(ignored);
    socket_.close(ignored);
}

}  // namespace pulsegate::http
