#include "pulsegate/net/listener.h"

#include <boost/asio/dispatch.hpp>
#include <boost/asio/socket_base.hpp>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>

namespace pulsegate::net {
namespace {

[[noreturn]] void throwSystemError(const ErrorCode& error, const std::string& operation) {
    throw boost::system::system_error(error, operation);
}

}  // namespace

Listener::Listener(asio::io_context& context, tcp::endpoint endpoint, int backlog,
                   SessionFactory session_factory, runtime::CoroutineErrorHandler on_error)
    : context_(context),
      strand_(asio::make_strand(context)),
      acceptor_(strand_),
      retry_timer_(strand_),
      session_factory_(std::move(session_factory)),
      on_error_(std::move(on_error)) {
    if (backlog <= 0 || !session_factory_) {
        throw std::invalid_argument("listener requires a positive backlog and session factory");
    }

    ErrorCode error;
    acceptor_.open(endpoint.protocol(), error);
    if (error) {
        throwSystemError(error, "open asynchronous acceptor");
    }
    acceptor_.set_option(asio::socket_base::reuse_address(true), error);
    if (error) {
        throwSystemError(error, "set SO_REUSEADDR");
    }
    acceptor_.bind(endpoint, error);
    if (error) {
        throwSystemError(error, "bind asynchronous acceptor");
    }
    acceptor_.listen(backlog, error);
    if (error) {
        throwSystemError(error, "listen on asynchronous acceptor");
    }
}

void Listener::start() {
    const auto self = shared_from_this();
    asio::dispatch(strand_, [self] {
        if (self->started_ || self->stopping_) {
            return;
        }
        self->started_ = true;
        runtime::spawnGuarded(
            self->strand_, "listener.accept_loop",
            [self]() -> Awaitable<void> { co_await self->acceptLoop(); },
            [self](std::string_view operation, std::exception_ptr error) {
                self->stopInExecutor();
                if (self->on_error_) {
                    self->on_error_(operation, error);
                }
            });
    });
}

void Listener::stop() {
    const auto self = shared_from_this();
    asio::dispatch(strand_, [self] { self->stopInExecutor(); });
}

tcp::endpoint Listener::localEndpoint() const {
    ErrorCode error;
    const auto endpoint = acceptor_.local_endpoint(error);
    if (error) {
        throwSystemError(error, "read asynchronous listener endpoint");
    }
    return endpoint;
}

Awaitable<void> Listener::acceptLoop() {
    while (!stopping_) {
        tcp::socket socket(asio::make_strand(context_));
        ErrorCode error;
        co_await acceptor_.async_accept(socket, asio::redirect_error(use_awaitable, error));

        if (error == asio::error::operation_aborted && stopping_) {
            co_return;
        }
        if (error) {
            retry_timer_.expires_after(std::chrono::milliseconds(50));
            co_await retry_timer_.async_wait(asio::redirect_error(use_awaitable, error));
            if (stopping_) {
                co_return;
            }
            continue;
        }

        try {
            session_factory_(std::move(socket));
        } catch (...) {
            if (on_error_) {
                on_error_("listener.session_factory", std::current_exception());
            }
        }
    }
}

void Listener::stopInExecutor() {
    if (stopping_) {
        return;
    }
    stopping_ = true;
    ErrorCode ignored;
    retry_timer_.cancel();
    acceptor_.cancel(ignored);
    acceptor_.close(ignored);
}

}  // namespace pulsegate::net
