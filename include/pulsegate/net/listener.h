#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <functional>
#include <memory>

#include "pulsegate/net/asio_types.h"
#include "pulsegate/runtime/coroutine_guard.h"

namespace pulsegate::net {

class Listener : public std::enable_shared_from_this<Listener> {
   public:
    using SessionFactory = std::function<void(tcp::socket)>;

    Listener(asio::io_context& context, tcp::endpoint endpoint, int backlog,
             SessionFactory session_factory, runtime::CoroutineErrorHandler on_error = {});

    void start();
    void stop();
    [[nodiscard]] tcp::endpoint localEndpoint() const;

   private:
    Awaitable<void> acceptLoop();
    void stopInExecutor();

    asio::io_context& context_;
    asio::strand<asio::io_context::executor_type> strand_;
    tcp::acceptor acceptor_;
    asio::steady_timer retry_timer_;
    SessionFactory session_factory_;
    runtime::CoroutineErrorHandler on_error_;
    bool started_{false};
    bool stopping_{false};
};

}  // namespace pulsegate::net
