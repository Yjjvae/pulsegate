#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include "pulsegate/http/http_parser.h"
#include "pulsegate/http/http_response.h"
#include "pulsegate/http/router.h"
#include "pulsegate/http/session_lifecycle.h"
#include "pulsegate/net/deadline.h"
#include "pulsegate/net/endpoint.h"
#include "pulsegate/net/listener.h"

namespace pulsegate::http {

using RequestHandler = std::function<HttpResponse(const HttpRequest&)>;

struct SessionLimits {
    HttpParserLimits parser{};
    std::size_t read_chunk_bytes{4096};
    std::size_t max_buffer_bytes{2 * 1024 * 1024};
    std::chrono::milliseconds header_timeout{std::chrono::seconds(10)};
    std::chrono::milliseconds body_timeout{std::chrono::seconds(30)};
    std::chrono::milliseconds idle_timeout{std::chrono::seconds(15)};
    std::size_t max_connections{1024};
};

struct RouterConfig {
    explicit RouterConfig(std::shared_ptr<Router> value) : router(std::move(value)) {}

    std::shared_ptr<Router> router;
};

class HttpSession : public std::enable_shared_from_this<HttpSession> {
   public:
    HttpSession(net::tcp::socket socket, std::shared_ptr<Router> router,
                std::shared_ptr<SessionRegistry> registry, SessionId id, SessionLimits limits = {});

    void start();
    void stop(StopReason reason = StopReason::ServerShutdown);
    void beginDrain();

    [[nodiscard]] SessionState state() const noexcept;
    [[nodiscard]] StopReason stopReason() const noexcept;

   private:
    net::Awaitable<void> run();
    net::Awaitable<std::optional<HttpRequest>> readRequest();
    net::Awaitable<void> writeResponse(const HttpResponse& response, bool head_request);
    net::Awaitable<bool> writeDownstream(std::string bytes);
    net::Awaitable<void> writeParserError(ParseResult result);
    void armReadDeadline();
    void stopInExecutor(StopReason reason);
    void beginDrainInExecutor();
    void closeInExecutor(StopReason reason);

    net::tcp::socket socket_;
    std::shared_ptr<Router> router_;
    SessionLimits limits_;
    net::Buffer input_;
    HttpParser parser_;
    std::shared_ptr<SessionRegistry> registry_;
    SessionId id_;
    std::shared_ptr<net::Deadline> deadline_;
    std::weak_ptr<ProxySession> current_proxy_;
    SessionState state_{SessionState::Created};
    StopReason stop_reason_{StopReason::None};
    bool reading_{false};
    bool served_request_{false};
    bool close_recorded_{false};
};

class HttpServer {
   public:
    HttpServer(net::asio::io_context& context, net::ListenConfig config,
               RequestHandler handler = {}, SessionLimits limits = {});
    HttpServer(net::asio::io_context& context, net::ListenConfig config, RouterConfig router,
               SessionLimits limits = {});

    void start();
    void stop();
    [[nodiscard]] net::tcp::endpoint localEndpoint() const;
    [[nodiscard]] std::size_t connectionCount() const;
    [[nodiscard]] std::size_t closedCount(StopReason reason) const;
    [[nodiscard]] static std::shared_ptr<Router> makeDefaultRouter();

   private:
    std::shared_ptr<net::Listener> listener_;
    std::shared_ptr<SessionRegistry> registry_;
};

}  // namespace pulsegate::http
