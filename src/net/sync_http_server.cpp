#include "pulsegate/net/sync_http_server.h"

#include <algorithm>
#include <array>
#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/system_error.hpp>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace pulsegate::net {
namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

constexpr std::size_t kReadBufferBytes = 4096;
constexpr std::size_t kMaxHeaderBytes = 16 * 1024;
constexpr std::string_view kHeaderTerminator = "\r\n\r\n";

enum class RequestStatus : std::uint8_t { ok, bad_request, method_not_allowed };

[[noreturn]] void throwSystemError(const boost::system::error_code& error,
                                   std::string_view operation) {
    throw boost::system::system_error(error, std::string(operation));
}

RequestStatus validateRequestLine(std::string_view request) {
    const auto line_end = request.find("\r\n");
    if (line_end == std::string_view::npos) {
        return RequestStatus::bad_request;
    }

    const auto line = request.substr(0, line_end);
    const auto first_space = line.find(' ');
    if (first_space == std::string_view::npos) {
        return RequestStatus::bad_request;
    }

    const auto second_space = line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos ||
        line.find(' ', second_space + 1) != std::string_view::npos) {
        return RequestStatus::bad_request;
    }

    const auto method = line.substr(0, first_space);
    const auto target = line.substr(first_space + 1, second_space - first_space - 1);
    const auto version = line.substr(second_space + 1);
    if (target.empty() || target.front() != '/' ||
        (version != "HTTP/1.1" && version != "HTTP/1.0")) {
        return RequestStatus::bad_request;
    }

    return method == "GET" ? RequestStatus::ok : RequestStatus::method_not_allowed;
}

std::string makeResponse(std::string_view status, std::string_view body,
                         std::string_view extra_headers = {}) {
    std::string response;
    response.reserve(96 + body.size() + extra_headers.size());
    response.append("HTTP/1.1 ");
    response.append(status);
    response.append("\r\nContent-Type: text/plain\r\nContent-Length: ");
    response.append(std::to_string(body.size()));
    response.append("\r\nConnection: close\r\n");
    response.append(extra_headers);
    response.append("\r\n");
    response.append(body);
    return response;
}

void writeResponse(tcp::socket& socket, const std::string& response) {
    boost::system::error_code error;
    asio::write(socket, asio::buffer(response), error);
    // A peer is allowed to disconnect before reading the response. That is a
    // connection-local event, so the sequential accept loop keeps running.
}

}  // namespace

SyncHttpServer::SyncHttpServer(asio::io_context& io_context, const ListenConfig& config)
    : acceptor_(io_context) {
    if (config.backlog <= 0) {
        throw std::invalid_argument("listen backlog must be greater than zero");
    }

    boost::system::error_code error;
    const auto endpoint = makeEndpoint(config, error);
    if (error) {
        throwSystemError(error, "parse listen address " + config.host);
    }

    acceptor_.open(endpoint.protocol(), error);
    if (error) {
        throwSystemError(error, "open acceptor");
    }

    acceptor_.set_option(asio::socket_base::reuse_address(true), error);
    if (error) {
        throwSystemError(error, "set SO_REUSEADDR");
    }

    acceptor_.bind(endpoint, error);
    if (error) {
        throwSystemError(error, "bind " + config.host + ":" + std::to_string(config.port));
    }

    acceptor_.listen(config.backlog, error);
    if (error) {
        throwSystemError(error, "listen");
    }
}

tcp::endpoint SyncHttpServer::localEndpoint() const {
    boost::system::error_code error;
    const auto endpoint = acceptor_.local_endpoint(error);
    if (error) {
        throwSystemError(error, "read local endpoint");
    }
    return endpoint;
}

std::uint16_t SyncHttpServer::port() const {
    return localEndpoint().port();
}

void SyncHttpServer::run(std::size_t max_connections, boost::system::error_code& error) {
    error.clear();
    std::size_t accepted_connections = 0;

    while (max_connections == 0 || accepted_connections < max_connections) {
        tcp::socket socket(acceptor_.get_executor());
        acceptor_.accept(socket, error);
        if (error) {
            return;
        }

        ++accepted_connections;
        handleConnection(socket);
    }
}

void SyncHttpServer::handleConnection(tcp::socket& socket) {
    std::array<char, kReadBufferBytes> buffer{};
    std::string request;
    request.reserve(kReadBufferBytes);

    while (request.find(kHeaderTerminator) == std::string::npos) {
        if (request.size() >= kMaxHeaderBytes) {
            writeResponse(socket, makeResponse("431 Request Header Fields Too Large",
                                               "request header fields too large\n"));
            return;
        }

        const auto remaining = kMaxHeaderBytes - request.size();
        const auto bytes_to_read = std::min(buffer.size(), remaining);
        boost::system::error_code error;
        const auto bytes_read = socket.read_some(asio::buffer(buffer.data(), bytes_to_read), error);
        if (error) {
            if (error == asio::error::eof || error == asio::error::connection_reset) {
                return;
            }
            return;
        }

        request.append(buffer.data(), bytes_read);
    }

    switch (validateRequestLine(request)) {
        case RequestStatus::ok:
            writeResponse(socket, makeResponse("200 OK", "hello world\n"));
            break;
        case RequestStatus::method_not_allowed:
            writeResponse(socket, makeResponse("405 Method Not Allowed", "method not allowed\n",
                                               "Allow: GET\r\n"));
            break;
        case RequestStatus::bad_request:
            writeResponse(socket, makeResponse("400 Bad Request", "bad request\n"));
            break;
    }
}

}  // namespace pulsegate::net
