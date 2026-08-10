#include "pulsegate/http/http_server.h"

#include <boost/asio/dispatch.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/system_error.hpp>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#include "pulsegate/runtime/coroutine_guard.h"

namespace pulsegate::http {
namespace {

HttpResponse makeErrorResponse(ParseResult result) {
    HttpResponse response;
    response.close_connection = true;
    switch (result) {
        case ParseResult::BadRequest:
            response.status_code = 400;
            response.reason = "Bad Request";
            response.body = "bad request\n";
            break;
        case ParseResult::HeaderTooLarge:
            response.status_code = 431;
            response.reason = "Request Header Fields Too Large";
            response.body = "request header fields too large\n";
            break;
        case ParseResult::BodyTooLarge:
            response.status_code = 413;
            response.reason = "Content Too Large";
            response.body = "request body too large\n";
            break;
        case ParseResult::UnsupportedTransferEncoding:
            response.status_code = 501;
            response.reason = "Not Implemented";
            response.body = "transfer encoding is not supported\n";
            break;
        case ParseResult::NeedMore:
        case ParseResult::Complete:
            throw std::logic_error("cannot convert a non-error parse result to a response");
    }
    return response;
}

void logCoroutineFailure(std::string_view operation, std::exception_ptr error) {
    try {
        if (error) {
            std::rethrow_exception(error);
        }
    } catch (const std::exception& exception) {
        std::cerr << "Coroutine failure in " << operation << ": " << exception.what() << '\n';
    } catch (...) {
        std::cerr << "Coroutine failure in " << operation << ": unknown exception\n";
    }
}

}  // namespace

HttpSession::HttpSession(net::tcp::socket socket, RequestHandler handler, SessionLimits limits)
    : socket_(std::move(socket)),
      handler_(std::move(handler)),
      limits_(limits),
      input_(limits_.read_chunk_bytes, limits_.max_buffer_bytes),
      parser_(limits_.parser) {
    if (!handler_) {
        throw std::invalid_argument("HTTP session requires a request handler");
    }
    if (limits_.read_chunk_bytes == 0 || limits_.max_buffer_bytes < limits_.read_chunk_bytes ||
        limits_.max_buffer_bytes < limits_.parser.max_header_bytes ||
        limits_.max_buffer_bytes - limits_.parser.max_header_bytes <
            limits_.parser.max_body_bytes) {
        throw std::invalid_argument("session buffer cannot satisfy configured parser limits");
    }
}

void HttpSession::start() {
    const auto self = shared_from_this();
    runtime::spawnGuarded(
        socket_.get_executor(), "http_session",
        [self]() -> net::Awaitable<void> { co_await self->run(); },
        [self](std::string_view operation, std::exception_ptr error) {
            self->close();
            logCoroutineFailure(operation, error);
        });
}

void HttpSession::stop() {
    const auto self = shared_from_this();
    net::asio::dispatch(socket_.get_executor(), [self] {
        self->stopping_ = true;
        self->close();
    });
}

net::Awaitable<void> HttpSession::run() {
    while (!stopping_) {
        auto request = co_await readRequest();
        if (!request) {
            break;
        }

        auto response = handler_(*request);
        const bool close_after_response = response.close_connection || !request->keepAlive();
        co_await writeResponse(response, request->method == HttpMethod::Head);
        if (close_after_response) {
            break;
        }
        parser_.reset();
    }

    close();
}

net::Awaitable<std::optional<HttpRequest>> HttpSession::readRequest() {
    for (;;) {
        const auto result = parser_.parse(input_);
        if (result == ParseResult::Complete) {
            co_return parser_.takeRequest();
        }
        if (result != ParseResult::NeedMore) {
            co_await writeParserError(result);
            stopping_ = true;
            co_return std::nullopt;
        }

        auto writable = input_.prepare(limits_.read_chunk_bytes);
        net::ErrorCode error;
        const auto count =
            co_await socket_.async_read_some(net::asio::buffer(writable.data(), writable.size()),
                                             net::asio::redirect_error(net::use_awaitable, error));
        if (error) {
            co_return std::nullopt;
        }
        if (count == 0) {
            co_return std::nullopt;
        }
        input_.commit(count);
    }
}

net::Awaitable<void> HttpSession::writeResponse(const HttpResponse& response, bool head_request) {
    auto bytes = response.serialize(head_request);
    net::ErrorCode error;
    co_await net::asio::async_write(socket_, net::asio::buffer(bytes),
                                    net::asio::redirect_error(net::use_awaitable, error));
    if (error) {
        stopping_ = true;
    }
}

net::Awaitable<void> HttpSession::writeParserError(ParseResult result) {
    co_await writeResponse(makeErrorResponse(result), false);
}

void HttpSession::close() {
    if (!socket_.is_open()) {
        return;
    }
    net::ErrorCode ignored;
    socket_.shutdown(net::tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);
}

HttpServer::HttpServer(net::asio::io_context& context, net::ListenConfig config,
                       RequestHandler handler, SessionLimits limits) {
    if (!handler) {
        handler = defaultHandler;
    }

    net::ErrorCode error;
    const auto endpoint = net::makeEndpoint(config, error);
    if (error) {
        throw boost::system::system_error(error, "parse HTTP server listen address");
    }

    listener_ = std::make_shared<net::Listener>(
        context, endpoint, config.backlog,
        [handler = std::move(handler), limits](net::tcp::socket socket) {
            std::make_shared<HttpSession>(std::move(socket), handler, limits)->start();
        },
        [](std::string_view operation, std::exception_ptr exception) {
            logCoroutineFailure(operation, exception);
        });
}

void HttpServer::start() {
    listener_->start();
}

void HttpServer::stop() {
    listener_->stop();
}

net::tcp::endpoint HttpServer::localEndpoint() const {
    return listener_->localEndpoint();
}

HttpResponse HttpServer::defaultHandler(const HttpRequest& request) {
    HttpResponse response;
    response.headers.add("Content-Type", "text/plain");
    if (request.target != "/healthz") {
        response.status_code = 404;
        response.reason = "Not Found";
        response.body = "not found\n";
        return response;
    }
    if (request.method == HttpMethod::Get || request.method == HttpMethod::Head) {
        response.body = "ok\n";
        return response;
    }
    response.status_code = 405;
    response.reason = "Method Not Allowed";
    response.body = "method not allowed\n";
    response.headers.add("Allow", "GET, HEAD");
    return response;
}

}  // namespace pulsegate::http
