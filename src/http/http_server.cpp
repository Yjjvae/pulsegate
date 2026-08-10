#include "pulsegate/http/http_server.h"

#include <atomic>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/system_error.hpp>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#include "pulsegate/core/version.h"
#include "pulsegate/http/reverse_proxy.h"
#include "pulsegate/runtime/coroutine_guard.h"
#include "pulsegate/runtime/scope_exit.h"

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

bool isPositive(std::chrono::milliseconds value) {
    return value > std::chrono::milliseconds::zero();
}

}  // namespace

const char* toString(StopReason reason) noexcept {
    switch (reason) {
        case StopReason::None:
            return "none";
        case StopReason::PeerClosed:
            return "peer_closed";
        case StopReason::ProtocolError:
            return "protocol_error";
        case StopReason::HeaderTimeout:
            return "header_timeout";
        case StopReason::BodyTimeout:
            return "body_timeout";
        case StopReason::IdleTimeout:
            return "idle_timeout";
        case StopReason::ServerShutdown:
            return "server_shutdown";
        case StopReason::ResourceLimit:
            return "resource_limit";
        case StopReason::InternalError:
            return "internal_error";
    }
    return "unknown";
}

SessionRegistry::SessionRegistry(std::size_t maximum_sessions)
    : maximum_sessions_(maximum_sessions) {
    if (maximum_sessions_ == 0) {
        throw std::invalid_argument("session registry requires a positive connection limit");
    }
}

bool SessionRegistry::tryAdd(SessionId id, const std::shared_ptr<HttpSession>& session) {
    std::scoped_lock lock(mutex_);
    pruneExpiredLocked();
    if (draining_ || sessions_.size() >= maximum_sessions_) {
        return false;
    }
    return sessions_.emplace(id, session).second;
}

void SessionRegistry::recordRejected(StopReason reason) {
    std::scoped_lock lock(mutex_);
    ++closed_counts_[reason];
}

void SessionRegistry::remove(SessionId id, StopReason reason) {
    std::scoped_lock lock(mutex_);
    if (sessions_.erase(id) != 0) {
        ++closed_counts_[reason];
    }
}

void SessionRegistry::beginDrain() {
    {
        std::scoped_lock lock(mutex_);
        draining_ = true;
    }
    for (const auto& session : liveSessions()) {
        session->beginDrain();
    }
}

void SessionRegistry::forceCloseAll() {
    for (const auto& session : liveSessions()) {
        session->stop(StopReason::ServerShutdown);
    }
}

std::size_t SessionRegistry::size() const {
    std::scoped_lock lock(mutex_);
    const_cast<SessionRegistry*>(this)->pruneExpiredLocked();
    return sessions_.size();
}

std::size_t SessionRegistry::closedCount(StopReason reason) const {
    std::scoped_lock lock(mutex_);
    const auto iterator = closed_counts_.find(reason);
    return iterator == closed_counts_.end() ? 0 : iterator->second;
}

std::vector<std::shared_ptr<HttpSession>> SessionRegistry::liveSessions() {
    std::vector<std::shared_ptr<HttpSession>> result;
    std::scoped_lock lock(mutex_);
    for (auto iterator = sessions_.begin(); iterator != sessions_.end();) {
        if (auto session = iterator->second.lock()) {
            result.push_back(std::move(session));
            ++iterator;
        } else {
            iterator = sessions_.erase(iterator);
        }
    }
    return result;
}

void SessionRegistry::pruneExpiredLocked() {
    for (auto iterator = sessions_.begin(); iterator != sessions_.end();) {
        if (iterator->second.expired()) {
            iterator = sessions_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

HttpSession::HttpSession(net::tcp::socket socket, std::shared_ptr<Router> router,
                         std::shared_ptr<SessionRegistry> registry, SessionId id,
                         SessionLimits limits)
    : socket_(std::move(socket)),
      router_(std::move(router)),
      limits_(limits),
      input_(limits_.read_chunk_bytes, limits_.max_buffer_bytes),
      parser_(limits_.parser),
      registry_(std::move(registry)),
      id_(id),
      deadline_(std::make_shared<net::Deadline>(socket_.get_executor())) {
    if (!router_ || !registry_) {
        throw std::invalid_argument("HTTP session requires a router and session registry");
    }
    if (limits_.read_chunk_bytes == 0 || limits_.max_buffer_bytes < limits_.read_chunk_bytes ||
        limits_.max_buffer_bytes < limits_.parser.max_header_bytes ||
        limits_.max_buffer_bytes - limits_.parser.max_header_bytes <
            limits_.parser.max_body_bytes ||
        !isPositive(limits_.header_timeout) || !isPositive(limits_.body_timeout) ||
        !isPositive(limits_.idle_timeout)) {
        throw std::invalid_argument("HTTP session limits are outside safe bounds");
    }
}

void HttpSession::start() {
    const auto self = shared_from_this();
    runtime::spawnGuarded(
        socket_.get_executor(), "http_session",
        [self]() -> net::Awaitable<void> { co_await self->run(); },
        [self](std::string_view operation, std::exception_ptr error) {
            self->closeInExecutor(StopReason::InternalError);
            logCoroutineFailure(operation, error);
        });
}

void HttpSession::stop(StopReason reason) {
    const auto self = shared_from_this();
    net::asio::dispatch(socket_.get_executor(), [self, reason] { self->stopInExecutor(reason); });
}

void HttpSession::beginDrain() {
    const auto self = shared_from_this();
    net::asio::dispatch(socket_.get_executor(), [self] { self->beginDrainInExecutor(); });
}

SessionState HttpSession::state() const noexcept {
    return state_;
}

StopReason HttpSession::stopReason() const noexcept {
    return stop_reason_;
}

net::Awaitable<void> HttpSession::run() {
    // A member coroutine stores only `this`; keep the Session alive across its
    // suspension points rather than relying on the weak registry or timer.
    [[maybe_unused]] const auto self = shared_from_this();
    if (state_ == SessionState::Created) {
        state_ = SessionState::Running;
    }
    while (state_ == SessionState::Running || state_ == SessionState::Draining) {
        auto request = co_await readRequest();
        if (!request || state_ == SessionState::Closing || state_ == SessionState::Closed) {
            break;
        }

        const bool close_after_response = !request->keepAlive();
        const bool head_request = request->method == HttpMethod::Head;
        net::ErrorCode endpoint_error;
        const auto peer = socket_.remote_endpoint(endpoint_error);
        RequestContext context{
            .executor = socket_.get_executor(),
            .request_id = router_->nextRequestId(),
            .peer = endpoint_error ? net::tcp::endpoint{} : peer,
            .downstream = weak_from_this(),
            .set_current_proxy =
                [weak = weak_from_this()](std::weak_ptr<ProxySession> proxy) {
                    if (auto session = weak.lock()) {
                        session->current_proxy_ = std::move(proxy);
                    }
                },
            .write_downstream = [weak =
                                     weak_from_this()](std::string bytes) -> net::Awaitable<bool> {
                if (auto session = weak.lock()) {
                    co_return co_await session->writeDownstream(std::move(bytes));
                }
                co_return false;
            }};
        auto response = co_await router_->handle(context, std::move(*request));
        current_proxy_.reset();
        if (!response.already_written) {
            co_await writeResponse(response, head_request);
        }
        served_request_ = true;
        if (response.close_connection || close_after_response || state_ == SessionState::Draining ||
            state_ == SessionState::Closing || state_ == SessionState::Closed) {
            break;
        }
        parser_.reset();
    }

    if (state_ != SessionState::Closed) {
        closeInExecutor(stop_reason_ == StopReason::None ? StopReason::PeerClosed : stop_reason_);
    }
}

net::Awaitable<std::optional<HttpRequest>> HttpSession::readRequest() {
    for (;;) {
        const auto result = parser_.parse(input_);
        if (result == ParseResult::Complete) {
            co_return parser_.takeRequest();
        }
        if (result != ParseResult::NeedMore) {
            stop_reason_ = StopReason::ProtocolError;
            co_await writeParserError(result);
            co_return std::nullopt;
        }

        auto writable = input_.prepare(limits_.read_chunk_bytes);
        armReadDeadline();
        reading_ = true;
        const auto cleanup = runtime::makeScopeExit([this] {
            reading_ = false;
            deadline_->disarm();
        });

        net::ErrorCode error;
        const auto count =
            co_await socket_.async_read_some(net::asio::buffer(writable.data(), writable.size()),
                                             net::asio::redirect_error(net::use_awaitable, error));
        if (error) {
            if (stop_reason_ == StopReason::None) {
                stop_reason_ = StopReason::PeerClosed;
            }
            co_return std::nullopt;
        }
        if (count == 0) {
            if (stop_reason_ == StopReason::None) {
                stop_reason_ = StopReason::PeerClosed;
            }
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
    if (error && stop_reason_ == StopReason::None) {
        stop_reason_ = StopReason::PeerClosed;
    }
}

net::Awaitable<bool> HttpSession::writeDownstream(std::string bytes) {
    net::ErrorCode error;
    co_await net::asio::async_write(socket_, net::asio::buffer(bytes),
                                    net::asio::redirect_error(net::use_awaitable, error));
    if (error) {
        if (stop_reason_ == StopReason::None) {
            stop_reason_ = StopReason::PeerClosed;
        }
        co_return false;
    }
    co_return true;
}

net::Awaitable<void> HttpSession::writeParserError(ParseResult result) {
    co_await writeResponse(makeErrorResponse(result), false);
}

void HttpSession::armReadDeadline() {
    StopReason reason = StopReason::HeaderTimeout;
    auto timeout = limits_.header_timeout;
    if (parser_.state() == ParseState::Body) {
        reason = StopReason::BodyTimeout;
        timeout = limits_.body_timeout;
    } else if (served_request_ && parser_.state() == ParseState::RequestLine &&
               input_.readableBytes() == 0) {
        reason = StopReason::IdleTimeout;
        timeout = limits_.idle_timeout;
    }

    deadline_->arm(timeout, [weak = weak_from_this(), reason] {
        if (auto self = weak.lock()) {
            self->stopInExecutor(reason);
        }
    });
}

void HttpSession::stopInExecutor(StopReason reason) {
    closeInExecutor(reason);
}

void HttpSession::beginDrainInExecutor() {
    if (state_ == SessionState::Closing || state_ == SessionState::Closed) {
        return;
    }
    state_ = SessionState::Draining;
    // A slow client must not keep shutdown alive. An in-flight write is left
    // alone so an already accepted request can receive its response.
    if (reading_) {
        closeInExecutor(StopReason::ServerShutdown);
    }
}

void HttpSession::closeInExecutor(StopReason reason) {
    if (state_ == SessionState::Closed || state_ == SessionState::Closing) {
        return;
    }
    state_ = SessionState::Closing;
    if (stop_reason_ == StopReason::None) {
        stop_reason_ = reason;
    }
    deadline_->disarm();
    if (auto proxy = current_proxy_.lock()) {
        proxy->cancel(reason == StopReason::ServerShutdown ? ProxyStopReason::ServerShutdown
                                                           : ProxyStopReason::DownstreamClosed);
    }
    current_proxy_.reset();

    net::ErrorCode ignored;
    socket_.cancel(ignored);
    socket_.shutdown(net::tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);

    state_ = SessionState::Closed;
    if (!close_recorded_) {
        close_recorded_ = true;
        registry_->remove(id_, stop_reason_);
    }
}

HttpServer::HttpServer(net::asio::io_context& context, net::ListenConfig config,
                       RequestHandler handler, SessionLimits limits)
    : HttpServer(
          context, config, RouterConfig{[&handler] {
              if (!handler) {
                  return makeDefaultRouter();
              }
              auto router = std::make_shared<Router>();
              router->add(Route{
                  .method = HttpMethod::Unknown,
                  .pattern = "/",
                  .name = "legacy_handler",
                  .prefix_match = true,
                  .handler = [handler = std::move(handler)](RequestContext&, HttpRequest request)
                      -> net::Awaitable<HttpResponse> { co_return handler(request); }});
              return router;
          }()},
          limits) {}

HttpServer::HttpServer(net::asio::io_context& context, net::ListenConfig config,
                       RouterConfig router_config, SessionLimits limits)
    : registry_(std::make_shared<SessionRegistry>(limits.max_connections)) {
    auto router = std::move(router_config.router);
    if (!router) {
        throw std::invalid_argument("HTTP server requires a router");
    }
    net::ErrorCode error;
    const auto endpoint = net::makeEndpoint(config, error);
    if (error) {
        throw boost::system::system_error(error, "parse HTTP server listen address");
    }

    auto next_session_id = std::make_shared<std::atomic<SessionId>>(1);
    listener_ = std::make_shared<net::Listener>(
        context, endpoint, config.backlog,
        [router = std::move(router), limits, registry = registry_,
         next_session_id](net::tcp::socket socket) {
            const auto id = next_session_id->fetch_add(1, std::memory_order_relaxed);
            auto session =
                std::make_shared<HttpSession>(std::move(socket), router, registry, id, limits);
            if (registry->tryAdd(id, session)) {
                session->start();
            } else {
                net::ErrorCode ignored;
                socket.close(ignored);
                registry->recordRejected(StopReason::ResourceLimit);
            }
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
    registry_->beginDrain();
}

net::tcp::endpoint HttpServer::localEndpoint() const {
    return listener_->localEndpoint();
}

std::size_t HttpServer::connectionCount() const {
    return registry_->size();
}

std::size_t HttpServer::closedCount(StopReason reason) const {
    return registry_->closedCount(reason);
}

std::shared_ptr<Router> HttpServer::makeDefaultRouter() {
    auto router = std::make_shared<Router>();
    const auto text = [](std::string body) {
        return
            [body = std::move(body)](RequestContext&, HttpRequest) -> net::Awaitable<HttpResponse> {
                HttpResponse response;
                response.body = body;
                response.headers.add("Content-Type", "text/plain");
                co_return response;
            };
    };
    const auto add_get_and_head = [&router](std::string path, std::string name,
                                            HttpHandler handler) {
        router->add(
            Route{.method = HttpMethod::Get, .pattern = path, .name = name, .handler = handler});
        router->add(Route{.method = HttpMethod::Head,
                          .pattern = std::move(path),
                          .name = std::move(name),
                          .handler = std::move(handler)});
    };
    add_get_and_head("/healthz", "healthz", text("ok\n"));
    add_get_and_head("/livez", "livez", text("alive\n"));
    add_get_and_head("/readyz", "readyz", text("ready\n"));
    add_get_and_head("/metrics", "metrics", text("pulsegate_ready 1\n"));
    add_get_and_head("/api/version", "version",
                     text(std::string(pulsegate::core::version()) + "\n"));
    router->add(
        Route{.method = HttpMethod::Post,
              .pattern = "/echo",
              .name = "echo",
              .handler = [](RequestContext&, HttpRequest request) -> net::Awaitable<HttpResponse> {
                  HttpResponse response;
                  response.body = std::move(request.body);
                  response.headers.add("Content-Type", "application/octet-stream");
                  co_return response;
              }});
    return router;
}

}  // namespace pulsegate::http
