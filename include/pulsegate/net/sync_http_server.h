#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>
#include <cstddef>
#include <cstdint>

#include "pulsegate/net/endpoint.h"

namespace pulsegate::net {

// Chapter 6 baseline: one thread accepts and handles one connection at a time.
class SyncHttpServer {
   public:
    explicit SyncHttpServer(boost::asio::io_context& io_context, const ListenConfig& config = {});

    SyncHttpServer(const SyncHttpServer&) = delete;
    SyncHttpServer& operator=(const SyncHttpServer&) = delete;
    SyncHttpServer(SyncHttpServer&&) = delete;
    SyncHttpServer& operator=(SyncHttpServer&&) = delete;
    ~SyncHttpServer() = default;

    [[nodiscard]] boost::asio::ip::tcp::endpoint localEndpoint() const;
    [[nodiscard]] std::uint16_t port() const;

    // A value of zero keeps serving until accept returns an error. A finite value
    // is useful for deterministic integration tests.
    void run(std::size_t max_connections, boost::system::error_code& error);
    void run(boost::system::error_code& error) {
        run(0, error);
    }

   private:
    static void handleConnection(boost::asio::ip::tcp::socket& socket);

    boost::asio::ip::tcp::acceptor acceptor_;
};

}  // namespace pulsegate::net
