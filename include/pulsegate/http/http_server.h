#pragma once

#include <functional>
#include <memory>
#include <optional>

#include "pulsegate/http/http_parser.h"
#include "pulsegate/http/http_response.h"
#include "pulsegate/net/endpoint.h"
#include "pulsegate/net/listener.h"

namespace pulsegate::http {

using RequestHandler = std::function<HttpResponse(const HttpRequest&)>;

struct SessionLimits {
    HttpParserLimits parser{};
    std::size_t read_chunk_bytes{4096};
    std::size_t max_buffer_bytes{2 * 1024 * 1024};
};

class HttpSession : public std::enable_shared_from_this<HttpSession> {
   public:
    HttpSession(net::tcp::socket socket, RequestHandler handler, SessionLimits limits = {});

    void start();
    void stop();

   private:
    net::Awaitable<void> run();
    net::Awaitable<std::optional<HttpRequest>> readRequest();
    net::Awaitable<void> writeResponse(const HttpResponse& response, bool head_request);
    net::Awaitable<void> writeParserError(ParseResult result);
    void close();

    net::tcp::socket socket_;
    RequestHandler handler_;
    SessionLimits limits_;
    net::Buffer input_;
    HttpParser parser_;
    bool stopping_{false};
};

class HttpServer {
   public:
    HttpServer(net::asio::io_context& context, net::ListenConfig config,
               RequestHandler handler = {}, SessionLimits limits = {});

    void start();
    void stop();
    [[nodiscard]] net::tcp::endpoint localEndpoint() const;

   private:
    static HttpResponse defaultHandler(const HttpRequest& request);

    std::shared_ptr<net::Listener> listener_;
};

}  // namespace pulsegate::http
