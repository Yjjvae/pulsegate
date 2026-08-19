#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/system_error.hpp>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "pulsegate/http/http_server.h"
#include "pulsegate/http/reverse_proxy.h"
#include "pulsegate/runtime/asio_runtime.h"

namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using pulsegate::http::HttpServer;
using pulsegate::http::SessionLimits;
using pulsegate::http::StopReason;
using pulsegate::net::ListenConfig;

constexpr std::string_view kHealthRequest =
    "GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";

class RunningServer {
   public:
    explicit RunningServer(SessionLimits limits = {})
        : server_(context_, ListenConfig{.host = "127.0.0.1", .port = 0}, {}, limits) {
        server_.start();
        thread_ = std::jthread([this] { context_.run(); });
    }

    RunningServer(const RunningServer&) = delete;
    RunningServer& operator=(const RunningServer&) = delete;

    ~RunningServer() {
        server_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] tcp::endpoint endpoint() const {
        return server_.localEndpoint();
    }
    void stop() {
        server_.stop();
    }
    void stopAndJoin() {
        server_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    template <typename Handler>
    void beginDrain(std::chrono::milliseconds grace, Handler&& handler) {
        asio::post(context_, [this, grace, handler = std::forward<Handler>(handler)]() mutable {
            server_.beginDrain(grace, std::move(handler));
        });
    }
    [[nodiscard]] bool isDraining() const {
        return server_.isDraining();
    }
    [[nodiscard]] std::size_t connectionCount() const {
        return server_.connectionCount();
    }
    [[nodiscard]] std::size_t closedCount(StopReason reason) const {
        return server_.closedCount(reason);
    }

   private:
    asio::io_context context_{1};
    HttpServer server_;
    std::jthread thread_;
};

class MultiThreadedRunningServer {
   public:
    MultiThreadedRunningServer(std::size_t thread_count,
                               pulsegate::http::RequestHandler handler = {},
                               SessionLimits limits = {})
        : runtime_(thread_count),
          server_(runtime_.context(), ListenConfig{.host = "127.0.0.1", .port = 0},
                  std::move(handler), limits) {
        server_.start();
        runtime_.start();
    }

    MultiThreadedRunningServer(const MultiThreadedRunningServer&) = delete;
    MultiThreadedRunningServer& operator=(const MultiThreadedRunningServer&) = delete;

    ~MultiThreadedRunningServer() {
        stopAndJoin();
    }

    [[nodiscard]] tcp::endpoint endpoint() const {
        return server_.localEndpoint();
    }
    void stop() {
        server_.stop();
    }
    void stopAndJoin() {
        server_.stop();
        runtime_.requestStop();
        runtime_.join();
    }
    [[nodiscard]] std::size_t connectionCount() const {
        return server_.connectionCount();
    }
    [[nodiscard]] std::size_t closedCount(StopReason reason) const {
        return server_.closedCount(reason);
    }

   private:
    pulsegate::runtime::AsioRuntime runtime_;
    HttpServer server_;
};

class MockUpstream {
   public:
    explicit MockUpstream(std::vector<std::string> response_fragments,
                          std::size_t requests_to_serve = 1,
                          std::chrono::milliseconds response_delay = std::chrono::milliseconds(0))
        : acceptor_(context_, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)),
          response_fragments_(std::move(response_fragments)),
          requests_to_serve_(requests_to_serve),
          response_delay_(response_delay) {
        socket_ = std::make_shared<tcp::socket>(context_);
        thread_ = std::jthread([this] {
            try {
                acceptor_.accept(*socket_);
                connection_count_.fetch_add(1, std::memory_order_relaxed);
                std::array<char, 1024> storage{};
                for (std::size_t request_index = 0; request_index < requests_to_serve_;
                     ++request_index) {
                    std::string current_request;
                    std::size_t expected_body_bytes = 0;
                    bool headers_complete = false;
                    for (;;) {
                        const auto count = socket_->read_some(asio::buffer(storage));
                        current_request.append(storage.data(), count);
                        const auto header_end = current_request.find("\r\n\r\n");
                        if (header_end != std::string::npos && !headers_complete) {
                            const auto content_length = current_request.find("Content-Length: ");
                            if (content_length != std::string::npos &&
                                content_length < header_end) {
                                const auto begin =
                                    content_length + std::string_view("Content-Length: ").size();
                                const auto end = current_request.find("\r\n", begin);
                                const auto [parsed_end, error] = std::from_chars(
                                    current_request.data() + begin, current_request.data() + end,
                                    expected_body_bytes);
                                if (error != std::errc{} ||
                                    parsed_end != current_request.data() + end) {
                                    throw std::runtime_error(
                                        "mock upstream received an invalid Content-Length");
                                }
                            }
                            headers_complete = true;
                        }
                        if (headers_complete &&
                            current_request.size() >= header_end + 4 + expected_body_bytes) {
                            break;
                        }
                    }
                    request_.append(current_request);
                    request_started_.store(true, std::memory_order_release);
                    if (response_delay_ > std::chrono::milliseconds::zero()) {
                        std::this_thread::sleep_for(response_delay_);
                    }
                    for (const auto& fragment : response_fragments_) {
                        asio::write(*socket_, asio::buffer(fragment));
                    }
                }
                received_.store(true, std::memory_order_release);
            } catch (const boost::system::system_error&) {
            }
        });
    }

    MockUpstream(const MockUpstream&) = delete;
    MockUpstream& operator=(const MockUpstream&) = delete;

    ~MockUpstream() {
        boost::system::error_code ignored;
        acceptor_.close(ignored);
        socket_->close(ignored);
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] tcp::endpoint endpoint() const {
        return acceptor_.local_endpoint();
    }
    [[nodiscard]] const std::string& request() const {
        return request_;
    }
    [[nodiscard]] bool received() const {
        return received_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool requestStarted() const {
        return request_started_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t connectionCount() const {
        return connection_count_.load(std::memory_order_relaxed);
    }

   private:
    asio::io_context context_{1};
    tcp::acceptor acceptor_;
    std::vector<std::string> response_fragments_;
    std::size_t requests_to_serve_;
    std::chrono::milliseconds response_delay_;
    std::string request_;
    std::atomic_bool received_{false};
    std::atomic_bool request_started_{false};
    std::atomic_size_t connection_count_{0};
    std::shared_ptr<tcp::socket> socket_;
    std::jthread thread_;
};

class ProxyRunningServer {
   public:
    explicit ProxyRunningServer(
        std::vector<pulsegate::http::UpstreamEndpoint> upstreams,
        std::optional<pulsegate::http::RateLimitConfig> rate_limit = std::nullopt,
        std::optional<pulsegate::http::ResponseCacheConfig> cache = std::nullopt,
        std::optional<pulsegate::http::ProxyLimits> proxy_limits = std::nullopt)
        : router_(std::make_shared<pulsegate::http::Router>()),
          proxy_(std::move(upstreams),
                 [proxy_limits] {
                     pulsegate::http::ProxyLimits limits;
                     if (proxy_limits) limits = *proxy_limits;
                     if (!proxy_limits) {
                         limits.pool_limits.max_connections = 1;
                         limits.pool_limits.max_idle_connections = 1;
                     }
                     return limits;
                 }()),
          server_(context_, ListenConfig{.host = "127.0.0.1", .port = 0},
                  pulsegate::http::RouterConfig{router_}) {
        const auto add = [this, rate_limit, cache](pulsegate::http::HttpMethod method) {
            router_->add(
                pulsegate::http::Route{
                    .method = method,
                    .pattern = "/proxy/",
                    .name = "proxy",
                    .prefix_match = true,
                    .handler = [proxy = proxy_](pulsegate::http::RequestContext& request_context,
                                                pulsegate::http::HttpRequest request)
                        -> pulsegate::net::Awaitable<pulsegate::http::HttpResponse> {
                        co_return co_await proxy(request_context, std::move(request));
                    }},
                rate_limit, cache);
        };
        add(pulsegate::http::HttpMethod::Get);
        add(pulsegate::http::HttpMethod::Head);
        add(pulsegate::http::HttpMethod::Post);
        server_.start();
        thread_ = std::jthread([this] { context_.run(); });
    }

    ~ProxyRunningServer() {
        proxy_.stop();
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }
    [[nodiscard]] tcp::endpoint endpoint() const {
        return server_.localEndpoint();
    }
    template <typename Handler>
    void beginDrain(std::chrono::milliseconds grace, Handler&& handler) {
        asio::post(context_, [this, grace, handler = std::forward<Handler>(handler)]() mutable {
            server_.beginDrain(grace, std::move(handler));
        });
    }

   private:
    asio::io_context context_{1};
    std::shared_ptr<pulsegate::http::Router> router_;
    pulsegate::http::ReverseProxy proxy_;
    HttpServer server_;
    std::jthread thread_;
};

SessionLimits shortTimeoutLimits() {
    SessionLimits limits;
    limits.header_timeout = std::chrono::milliseconds(30);
    limits.body_timeout = std::chrono::milliseconds(30);
    limits.idle_timeout = std::chrono::milliseconds(30);
    return limits;
}

boost::system::error_code waitForClose(tcp::socket& socket) {
    std::array<char, 64> storage{};
    boost::system::error_code error;
    [[maybe_unused]] const auto count = socket.read_some(asio::buffer(storage), error);
    return error;
}

template <typename Predicate>
bool waitUntil(Predicate&& predicate) {
    for (int attempt = 0; attempt < 1000; ++attempt) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

class ResponseReader {
   public:
    explicit ResponseReader(tcp::socket& socket) : socket_(socket) {}

    std::string readResponse() {
        const auto header_end = waitForHeader();
        if (pending_.find("Transfer-Encoding: chunked\r\n") < header_end) {
            for (;;) {
                const auto marker = pending_.find("0\r\n\r\n", header_end + 4);
                if (marker != std::string::npos) {
                    auto response = pending_.substr(0, marker + 5);
                    pending_.erase(0, marker + 5);
                    return response;
                }
                readMore();
            }
        }
        const auto content_length = contentLength(header_end);
        const auto response_size = header_end + 4 + content_length;
        while (pending_.size() < response_size) {
            readMore();
        }

        auto response = pending_.substr(0, response_size);
        pending_.erase(0, response_size);
        return response;
    }

   private:
    std::size_t waitForHeader() {
        for (;;) {
            const auto header_end = pending_.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                return header_end;
            }
            readMore();
        }
    }

    std::size_t contentLength(std::size_t header_end) const {
        constexpr std::string_view kContentLength = "Content-Length: ";
        const auto position = pending_.find(kContentLength);
        if (position == std::string::npos || position >= header_end) {
            throw std::runtime_error("HTTP response is missing Content-Length");
        }
        const auto value_begin = position + kContentLength.size();
        const auto value_end = pending_.find("\r\n", value_begin);
        if (value_end == std::string::npos || value_end > header_end) {
            throw std::runtime_error("HTTP response has an invalid Content-Length line");
        }

        std::size_t length = 0;
        const auto [end, error] =
            std::from_chars(pending_.data() + value_begin, pending_.data() + value_end, length);
        if (error != std::errc{} || end != pending_.data() + value_end) {
            throw std::runtime_error("HTTP response has a non-numeric Content-Length");
        }
        return length;
    }

    void readMore() {
        std::array<char, 1024> storage{};
        boost::system::error_code error;
        const auto count = socket_.read_some(asio::buffer(storage), error);
        pending_.append(storage.data(), count);
        if (error) {
            throw boost::system::system_error(error, "read asynchronous HTTP response");
        }
    }

    tcp::socket& socket_;
    std::string pending_;
};

template <typename Server>
std::string exchange(Server& server, const std::vector<std::string_view>& fragments) {
    asio::io_context client_context;
    tcp::socket client(client_context);
    client.connect(server.endpoint());
    for (const auto fragment : fragments) {
        asio::write(client, asio::buffer(fragment));
    }

    ResponseReader reader(client);
    return reader.readResponse();
}

void updateMaximum(std::atomic_int& maximum, int value) {
    auto observed = maximum.load(std::memory_order_relaxed);
    while (observed < value &&
           !maximum.compare_exchange_weak(observed, value, std::memory_order_relaxed)) {
    }
}

TEST(AsyncHttpServerTest, ServesHealthEndpoint) {
    RunningServer server;

    const auto response = exchange(server, {kHealthRequest});

    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(response.find("Content-Length: 3\r\n"), std::string::npos);
    EXPECT_TRUE(response.ends_with("\r\n\r\nok\n"));
}

TEST(AsyncHttpServerTest, AcceptsFragmentedRequest) {
    RunningServer server;

    const auto response = exchange(server, {"GET /healthz HTTP/1.1\r\nHo", "st: local",
                                            "host\r\nConnection: close\r\n", "\r\n"});

    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
}

TEST(AsyncHttpServerTest, MapsParserFailuresToClosingHttpErrors) {
    RunningServer server;

    const auto response = exchange(server, {"GET /healthz HTTP/1.1\r\n\r\n"});

    EXPECT_NE(response.find("HTTP/1.1 400 Bad Request\r\n"), std::string::npos);
    EXPECT_NE(response.find("Connection: close\r\n"), std::string::npos);
}

TEST(AsyncHttpServerTest, ReturnsMethodNotAllowedForUnsupportedRouteMethod) {
    RunningServer server;

    const auto response = exchange(
        server, {"POST /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"});

    EXPECT_NE(response.find("HTTP/1.1 405 Method Not Allowed\r\n"), std::string::npos);
    EXPECT_NE(response.find("allow: GET, HEAD\r\n"), std::string::npos);
}

TEST(AsyncHttpServerTest, ProcessesPipelinedKeepAliveRequestsInOrder) {
    RunningServer server;
    asio::io_context client_context;
    tcp::socket client(client_context);
    client.connect(server.endpoint());
    asio::write(client, asio::buffer("GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n"
                                     "GET /healthz HTTP/1.1\r\nHost: localhost\r\n"
                                     "Connection: close\r\n\r\n"));

    ResponseReader reader(client);
    const auto first = reader.readResponse();
    const auto second = reader.readResponse();

    EXPECT_NE(first.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(second.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_TRUE(first.ends_with("\r\n\r\nok\n"));
    EXPECT_TRUE(second.ends_with("\r\n\r\nok\n"));
}

TEST(AsyncHttpServerTest, SlowClientDoesNotBlockAnotherConnection) {
    RunningServer server;
    asio::io_context slow_context;
    tcp::socket slow_client(slow_context);
    slow_client.connect(server.endpoint());
    asio::write(slow_client, asio::buffer("G"));

    const auto response = exchange(server, {kHealthRequest});
    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);

    slow_client.close();
}

TEST(AsyncHttpServerTest, HandlesOneHundredConnectionsOnOneEventLoopThread) {
    constexpr std::size_t kConnectionCount = 100;
    RunningServer server;
    asio::io_context client_context;
    std::vector<tcp::socket> clients;
    clients.reserve(kConnectionCount);

    for (std::size_t index = 0; index < kConnectionCount; ++index) {
        clients.emplace_back(client_context);
        clients.back().connect(server.endpoint());
        asio::write(clients.back(), asio::buffer(kHealthRequest));
    }

    for (auto& client : clients) {
        ResponseReader reader(client);
        const auto response = reader.readResponse();
        EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    }
}

TEST(AsyncHttpServerTest, ToleratesClientClosingBeforeResponseRead) {
    RunningServer server;
    asio::io_context client_context;
    tcp::socket client(client_context);
    client.connect(server.endpoint());
    asio::write(client, asio::buffer(kHealthRequest));
    client.set_option(asio::socket_base::linger(true, 0));
    client.close();

    const auto response = exchange(server, {kHealthRequest});
    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
}

TEST(AsyncHttpServerTest, StopCancelsPendingAccept) {
    RunningServer server;
    const auto endpoint = server.endpoint();
    server.stopAndJoin();

    asio::io_context client_context;
    tcp::socket client(client_context);
    boost::system::error_code error;
    client.connect(endpoint, error);

    EXPECT_TRUE(error);
}

TEST(AsyncHttpServerTest, OperationsWaitUntilIoContextRuns) {
    asio::io_context server_context(1);
    HttpServer server(server_context, ListenConfig{.host = "127.0.0.1", .port = 0});
    const auto endpoint = server.localEndpoint();
    server.start();

    asio::io_context client_context;
    tcp::socket client(client_context);
    client.connect(endpoint);
    asio::write(client, asio::buffer(kHealthRequest));
    client.non_blocking(true);
    std::array<char, 1> byte{};
    boost::system::error_code read_error;
    client.read_some(asio::buffer(byte), read_error);
    EXPECT_EQ(read_error, asio::error::would_block);

    std::jthread server_thread([&server_context] { server_context.run(); });
    client.non_blocking(false);
    ResponseReader reader(client);
    const auto response = reader.readResponse();
    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);

    server.stop();
    server_thread.join();
}

TEST(AsyncHttpServerTest, ReclaimsSlowIncompleteHeaderWithHeaderTimeout) {
    RunningServer server(shortTimeoutLimits());
    asio::io_context client_context;
    tcp::socket client(client_context);
    client.connect(server.endpoint());
    asio::write(client, asio::buffer("GET /healthz HTTP/1.1\r\nHo"));

    EXPECT_TRUE(waitForClose(client));
    EXPECT_TRUE(
        waitUntil([&server] { return server.closedCount(StopReason::HeaderTimeout) == 1U; }));
    EXPECT_EQ(server.connectionCount(), 0U);
}

TEST(AsyncHttpServerTest, ReclaimsIncompleteBodyWithBodyTimeout) {
    RunningServer server(shortTimeoutLimits());
    asio::io_context client_context;
    tcp::socket client(client_context);
    client.connect(server.endpoint());
    asio::write(client, asio::buffer("POST /healthz HTTP/1.1\r\nHost: localhost\r\n"
                                     "Content-Length: 4\r\n\r\nx"));

    EXPECT_TRUE(waitForClose(client));
    EXPECT_TRUE(waitUntil([&server] { return server.closedCount(StopReason::BodyTimeout) == 1U; }));
    EXPECT_EQ(server.connectionCount(), 0U);
}

TEST(AsyncHttpServerTest, ReclaimsKeepAliveConnectionWithIdleTimeout) {
    RunningServer server(shortTimeoutLimits());
    asio::io_context client_context;
    tcp::socket client(client_context);
    client.connect(server.endpoint());
    constexpr std::string_view request = "GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n";
    asio::write(client, asio::buffer(request));

    ResponseReader reader(client);
    EXPECT_NE(reader.readResponse().find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_TRUE(waitForClose(client));
    EXPECT_TRUE(waitUntil([&server] { return server.closedCount(StopReason::IdleTimeout) == 1U; }));
    EXPECT_EQ(server.connectionCount(), 0U);
}

TEST(AsyncHttpServerTest, StopWinsOverPendingTimeoutAndClosesOnlyOnce) {
    RunningServer server(shortTimeoutLimits());
    asio::io_context client_context;
    tcp::socket client(client_context);
    client.connect(server.endpoint());
    asio::write(client, asio::buffer("GET /healthz HTTP/1.1\r\nHo"));
    ASSERT_TRUE(waitUntil([&server] { return server.connectionCount() == 1U; }));

    server.stop();
    EXPECT_TRUE(waitForClose(client));
    EXPECT_TRUE(
        waitUntil([&server] { return server.closedCount(StopReason::ServerShutdown) == 1U; }));
    EXPECT_EQ(server.connectionCount(), 0U);
    EXPECT_EQ(server.closedCount(StopReason::HeaderTimeout), 0U);
}

TEST(AsyncHttpServerTest, EnforcesConnectionLimitAndRecordsResourceRejection) {
    auto limits = shortTimeoutLimits();
    limits.max_connections = 1;
    RunningServer server(limits);
    asio::io_context client_context;
    tcp::socket first(client_context);
    tcp::socket second(client_context);
    first.connect(server.endpoint());
    asio::write(first, asio::buffer("G"));
    ASSERT_TRUE(waitUntil([&server] { return server.connectionCount() == 1U; }));
    second.connect(server.endpoint());

    EXPECT_TRUE(waitForClose(second));
    EXPECT_EQ(server.connectionCount(), 1U);
    EXPECT_EQ(server.closedCount(StopReason::ResourceLimit), 1U);
    first.close();
}

TEST(AsyncHttpServerTest, ServesDefaultAsyncRoutesAndSplitEchoBody) {
    RunningServer server;

    const auto live =
        exchange(server, {"GET /livez HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"});
    EXPECT_TRUE(live.ends_with("\r\n\r\nalive\n"));
    EXPECT_NE(live.find("x-request-id: "), std::string::npos);

    const auto version = exchange(
        server, {"GET /api/version HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"});
    EXPECT_NE(version.find("1.0.0-rc.1\n"), std::string::npos);

    const auto metrics =
        exchange(server, {"GET /metrics HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"});
    EXPECT_NE(metrics.find("\r\n\r\npulsegate_ready 1\n"), std::string::npos);
    EXPECT_NE(metrics.find("# TYPE pulsegate_rate_limit_requests_total counter\n"),
              std::string::npos);
    EXPECT_NE(metrics.find("# TYPE pulsegate_http_requests_total counter\n"), std::string::npos);
    EXPECT_NE(metrics.find("pulsegate_accepted_connections_total "), std::string::npos);
    EXPECT_NE(metrics.find("pulsegate_logs_dropped_total 0\n"), std::string::npos);
    EXPECT_NE(metrics.find("content-type: text/plain; version=0.0.4\r\n"), std::string::npos);

    const auto echo =
        exchange(server, {"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 11\r\n",
                          "Connection: close\r\n\r\nhello ", "world"});
    EXPECT_TRUE(echo.ends_with("\r\n\r\nhello world"));
}

TEST(AsyncHttpServerTest, ProxiesFragmentedChunkedResponseAndRewritesHeaders) {
    MockUpstream upstream(
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: keep-alive\r\n",
         "X-Upstream: yes\r\n\r\n5\r\nhello\r\n1\r\n!\r\n0\r\n\r\n"});
    const auto endpoint = upstream.endpoint();
    ProxyRunningServer gateway({pulsegate::http::UpstreamEndpoint{
        .host = endpoint.address().to_string(), .service = std::to_string(endpoint.port())}});

    const auto response = exchange(
        gateway, {"GET /proxy/resource HTTP/1.1\r\nHost: ignored\r\nConnection: close, X-Remove\r\n"
                  "X-Remove: should-not-pass\r\n\r\n"});

    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(response.find("Transfer-Encoding: chunked\r\n"), std::string::npos);
    EXPECT_NE(response.find("x-upstream: yes\r\n"), std::string::npos);
    EXPECT_EQ(response.find("transfer-encoding:"), std::string::npos);
    EXPECT_TRUE(response.ends_with("\r\n\r\n6\r\nhello!\r\n0\r\n\r\n"));
    EXPECT_TRUE(waitUntil([&upstream] { return upstream.received(); }));
    EXPECT_NE(upstream.request().find("X-Request-Id: "), std::string::npos);
    EXPECT_NE(upstream.request().find("X-Forwarded-For: 127.0.0.1"), std::string::npos);
    EXPECT_EQ(upstream.request().find("x-remove:"), std::string::npos);
}

TEST(AsyncHttpServerTest, ProxiesCompletePostBodyAndRoundRobinsUpstreams) {
    MockUpstream first({"HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nA"});
    MockUpstream second({"HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nB"});
    const auto first_endpoint = first.endpoint();
    const auto second_endpoint = second.endpoint();
    ProxyRunningServer gateway({{.host = first_endpoint.address().to_string(),
                                 .service = std::to_string(first_endpoint.port())},
                                {.host = second_endpoint.address().to_string(),
                                 .service = std::to_string(second_endpoint.port())}});

    const auto first_response =
        exchange(gateway, {"POST /proxy/orders HTTP/1.1\r\nHost: demo\r\n"
                           "Content-Length: 11\r\nConnection: close\r\n\r\nhello ",
                           "world"});
    const auto second_response = exchange(
        gateway, {"GET /proxy/orders HTTP/1.1\r\nHost: demo\r\nConnection: close\r\n\r\n"});

    EXPECT_TRUE(first_response.ends_with("\r\n\r\n1\r\nA\r\n0\r\n\r\n"));
    EXPECT_TRUE(second_response.ends_with("\r\n\r\n1\r\nB\r\n0\r\n\r\n"));
    EXPECT_TRUE(waitUntil([&first] { return first.received(); }));
    EXPECT_TRUE(waitUntil([&second] { return second.received(); }));
    EXPECT_TRUE(first.request().ends_with("\r\n\r\nhello world"));
    EXPECT_TRUE(second.request().starts_with("GET /proxy/orders HTTP/1.1\r\n"));
}

TEST(AsyncHttpServerTest, ReusesAnEligibleUpstreamConnectionAcrossTransactions) {
    MockUpstream upstream(
        {"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: keep-alive\r\n\r\nOK"}, 2);
    const auto endpoint = upstream.endpoint();
    ProxyRunningServer gateway(
        {{.host = endpoint.address().to_string(), .service = std::to_string(endpoint.port())}});

    const auto first =
        exchange(gateway, {"GET /proxy/first HTTP/1.1\r\nHost: demo\r\nConnection: close\r\n\r\n"});
    const auto second = exchange(
        gateway, {"GET /proxy/second HTTP/1.1\r\nHost: demo\r\nConnection: close\r\n\r\n"});

    EXPECT_TRUE(first.ends_with("\r\n\r\n2\r\nOK\r\n0\r\n\r\n"));
    EXPECT_TRUE(second.ends_with("\r\n\r\n2\r\nOK\r\n0\r\n\r\n"));
    EXPECT_TRUE(waitUntil([&upstream] { return upstream.received(); }));
    EXPECT_EQ(upstream.connectionCount(), 1U);
}

TEST(AsyncHttpServerTest, RateLimitRejectsBeforeASecondProxyRequestReachesUpstream) {
    MockUpstream upstream({"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK"});
    const auto endpoint = upstream.endpoint();
    ProxyRunningServer gateway(
        {{.host = endpoint.address().to_string(), .service = std::to_string(endpoint.port())}},
        pulsegate::http::RateLimitConfig{.requests_per_second = 1.0,
                                         .burst = 1.0,
                                         .per_client = true,
                                         .shard_count = 2,
                                         .max_keys = 8,
                                         .idle_ttl = std::chrono::seconds(1)});

    const auto first =
        exchange(gateway, {"GET /proxy/first HTTP/1.1\r\nHost: demo\r\nConnection: close\r\n\r\n"});
    const auto rejected = exchange(
        gateway, {"GET /proxy/second HTTP/1.1\r\nHost: demo\r\nConnection: close\r\n\r\n"});

    EXPECT_TRUE(first.ends_with("\r\n\r\n2\r\nOK\r\n0\r\n\r\n"));
    EXPECT_NE(rejected.find("HTTP/1.1 429 Too Many Requests\r\n"), std::string::npos);
    EXPECT_NE(rejected.find("retry-after: 1\r\n"), std::string::npos);
    EXPECT_TRUE(waitUntil([&upstream] { return upstream.received(); }));
    EXPECT_EQ(upstream.request().find("GET /proxy/second"), std::string::npos);
}

TEST(AsyncHttpServerTest, CacheHitAvoidsASecondProxyRequestToTheUpstream) {
    MockUpstream upstream({"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK"});
    const auto endpoint = upstream.endpoint();
    ProxyRunningServer gateway(
        {{.host = endpoint.address().to_string(), .service = std::to_string(endpoint.port())}},
        std::nullopt,
        pulsegate::http::ResponseCacheConfig{.ttl = std::chrono::seconds(1),
                                             .max_entry_bytes = 128,
                                             .max_bytes = 1024,
                                             .shard_count = 2,
                                             .vary_headers = {}});

    const auto first = exchange(
        gateway, {"GET /proxy/cached HTTP/1.1\r\nHost: demo\r\nConnection: close\r\n\r\n"});
    const auto second = exchange(
        gateway, {"GET /proxy/cached HTTP/1.1\r\nHost: demo\r\nConnection: close\r\n\r\n"});

    EXPECT_TRUE(first.ends_with("\r\n\r\nOK"));
    EXPECT_TRUE(second.ends_with("\r\n\r\nOK"));
    EXPECT_NE(first.find("x-cache: MISS\r\n"), std::string::npos);
    EXPECT_NE(second.find("x-cache: HIT\r\n"), std::string::npos);
    EXPECT_TRUE(waitUntil([&upstream] { return upstream.received(); }));
    EXPECT_EQ(upstream.connectionCount(), 1U);
    EXPECT_EQ(upstream.request().find("GET /proxy/cached"),
              upstream.request().rfind("GET /proxy/cached"));
}

TEST(AsyncHttpServerTest, CircuitOpenRejectsBeforeAFourthFailureReachesUpstream) {
    MockUpstream upstream({"HTTP/1.1 500 Internal Server Error\r\nContent-Length: 4\r\n"
                           "Connection: keep-alive\r\n\r\nfail"},
                          3);
    const auto endpoint = upstream.endpoint();
    pulsegate::http::ProxyLimits limits;
    limits.health_thresholds = {.healthy_threshold = 1, .unhealthy_threshold = 3};
    limits.circuit_breaker = {
        .failure_threshold = 3, .cooldown = std::chrono::seconds(5), .half_open_max_probes = 1};
    limits.pool_limits.max_connections = 1;
    limits.pool_limits.max_idle_connections = 1;
    ProxyRunningServer gateway(
        {{.host = endpoint.address().to_string(), .service = std::to_string(endpoint.port())}},
        std::nullopt, std::nullopt, limits);

    for (const auto path : {"/proxy/one", "/proxy/two", "/proxy/three"}) {
        const auto response = exchange(gateway, {std::string("GET ") + path +
                                                 " HTTP/1.1\r\nHost: demo\r\n"
                                                 "Connection: close\r\n\r\n"});
        EXPECT_NE(response.find("HTTP/1.1 500 Internal Server Error\r\n"), std::string::npos);
    }
    const auto rejected =
        exchange(gateway, {"GET /proxy/four HTTP/1.1\r\nHost: demo\r\nConnection: close\r\n\r\n"});

    EXPECT_NE(rejected.find("HTTP/1.1 503 Service Unavailable\r\n"), std::string::npos);
    EXPECT_NE(rejected.find("retry-after: 5\r\n"), std::string::npos);
    EXPECT_TRUE(waitUntil([&upstream] { return upstream.received(); }));
    EXPECT_EQ(upstream.request().find("GET /proxy/four"), std::string::npos);
}

TEST(AsyncHttpServerTest, ConfiguredFiveHundredsDoNotTripCircuitWhenExcludedFromFailures) {
    MockUpstream upstream({"HTTP/1.1 500 Internal Server Error\r\nContent-Length: 4\r\n"
                           "Connection: keep-alive\r\n\r\nfail"},
                          4);
    const auto endpoint = upstream.endpoint();
    pulsegate::http::ProxyLimits limits;
    limits.count_5xx_as_health_failure = false;
    limits.health_thresholds = {.healthy_threshold = 1, .unhealthy_threshold = 1};
    limits.circuit_breaker = {
        .failure_threshold = 1, .cooldown = std::chrono::seconds(5), .half_open_max_probes = 1};
    limits.pool_limits.max_connections = 1;
    limits.pool_limits.max_idle_connections = 1;
    ProxyRunningServer gateway(
        {{.host = endpoint.address().to_string(), .service = std::to_string(endpoint.port())}},
        std::nullopt, std::nullopt, limits);

    for (const auto path : {"/proxy/one", "/proxy/two", "/proxy/three", "/proxy/four"}) {
        const auto response = exchange(gateway, {std::string("GET ") + path +
                                                 " HTTP/1.1\r\nHost: demo\r\n"
                                                 "Connection: close\r\n\r\n"});
        EXPECT_NE(response.find("HTTP/1.1 500 Internal Server Error\r\n"), std::string::npos);
    }
    EXPECT_TRUE(waitUntil([&upstream] { return upstream.received(); }));
}

TEST(AsyncHttpServerTest, InFlightLimitRejectsBeforeSecondRequestReachesUpstreamPool) {
    MockUpstream upstream({"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK"},
                          1, std::chrono::milliseconds(100));
    const auto endpoint = upstream.endpoint();
    pulsegate::http::ProxyLimits limits;
    limits.max_in_flight_requests = 1;
    limits.pool_limits.max_connections = 1;
    limits.pool_limits.max_idle_connections = 1;
    ProxyRunningServer gateway(
        {{.host = endpoint.address().to_string(), .service = std::to_string(endpoint.port())}},
        std::nullopt, std::nullopt, limits);

    std::string first_response;
    std::jthread first([&] {
        first_response = exchange(
            gateway, {"GET /proxy/first HTTP/1.1\r\nHost: demo\r\nConnection: close\r\n\r\n"});
    });
    ASSERT_TRUE(waitUntil([&upstream] { return upstream.requestStarted(); }));
    const auto rejected = exchange(
        gateway, {"GET /proxy/second HTTP/1.1\r\nHost: demo\r\nConnection: close\r\n\r\n"});
    first.join();

    EXPECT_NE(first_response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(rejected.find("HTTP/1.1 503 Service Unavailable\r\n"), std::string::npos);
    EXPECT_NE(rejected.find("retry-after: 1\r\n"), std::string::npos);
    EXPECT_EQ(upstream.request().find("GET /proxy/second"), std::string::npos);
}

TEST(AsyncHttpServerTest, ServesTheSameProtocolWithOneTwoAndFourWorkers) {
    for (const auto thread_count : {1U, 2U, 4U}) {
        MultiThreadedRunningServer server(thread_count);
        const auto response = exchange(server, {kHealthRequest});
        EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    }
}

TEST(AsyncHttpServerTest, SerializesHandlersForOnePipelinedSession) {
    std::atomic_int active{0};
    std::atomic_int maximum{0};
    MultiThreadedRunningServer server(4, [&active, &maximum](const pulsegate::http::HttpRequest&) {
        const auto now = active.fetch_add(1, std::memory_order_relaxed) + 1;
        updateMaximum(maximum, now);
        // Test-only contention probe; production handlers must not block an io worker.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        active.fetch_sub(1, std::memory_order_relaxed);
        pulsegate::http::HttpResponse response;
        response.body = "ok\n";
        return response;
    });

    asio::io_context client_context;
    tcp::socket client(client_context);
    client.connect(server.endpoint());
    constexpr std::string_view requests =
        "GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    asio::write(client, asio::buffer(requests));

    ResponseReader reader(client);
    for (int index = 0; index < 4; ++index) {
        EXPECT_NE(reader.readResponse().find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    }
    EXPECT_EQ(maximum.load(std::memory_order_relaxed), 1);
}

TEST(AsyncHttpServerTest, AllowsDifferentSessionsToAdvanceOnDifferentWorkers) {
    constexpr std::size_t kClients = 8;
    std::atomic_int active{0};
    std::atomic_int maximum{0};
    MultiThreadedRunningServer server(4, [&active, &maximum](const pulsegate::http::HttpRequest&) {
        const auto now = active.fetch_add(1, std::memory_order_relaxed) + 1;
        updateMaximum(maximum, now);
        // Test-only probe: separate Session strands may run concurrently.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        active.fetch_sub(1, std::memory_order_relaxed);
        pulsegate::http::HttpResponse response;
        response.body = "ok\n";
        return response;
    });

    asio::io_context client_context;
    std::vector<tcp::socket> clients;
    clients.reserve(kClients);
    for (std::size_t index = 0; index < kClients; ++index) {
        clients.emplace_back(client_context);
        clients.back().connect(server.endpoint());
        asio::write(clients.back(), asio::buffer(kHealthRequest));
    }
    for (auto& client : clients) {
        ResponseReader reader(client);
        EXPECT_NE(reader.readResponse().find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    }
    EXPECT_GE(maximum.load(std::memory_order_relaxed), 2);
}

TEST(AsyncHttpServerTest, AcceptsExternalThreadStopOnMultiWorkerRuntime) {
    MultiThreadedRunningServer server(4, {}, shortTimeoutLimits());
    asio::io_context client_context;
    tcp::socket client(client_context);
    client.connect(server.endpoint());
    asio::write(client, asio::buffer("GET /healthz HTTP/1.1\r\nHo"));
    ASSERT_TRUE(waitUntil([&server] { return server.connectionCount() == 1U; }));

    std::jthread stop_thread([&server] { server.stop(); });
    stop_thread.join();
    EXPECT_TRUE(waitForClose(client));
    EXPECT_TRUE(
        waitUntil([&server] { return server.closedCount(StopReason::ServerShutdown) == 1U; }));
    EXPECT_EQ(server.connectionCount(), 0U);
}

TEST(AsyncHttpServerTest, DrainStopsAcceptingAndCompletesWhenNoSessionsRemain) {
    RunningServer server;
    const auto endpoint = server.endpoint();
    std::promise<bool> completed;
    server.beginDrain(std::chrono::milliseconds(100),
                      [&completed](bool drained) { completed.set_value(drained); });

    EXPECT_TRUE(completed.get_future().get());
    EXPECT_TRUE(server.isDraining());
    asio::io_context client_context;
    tcp::socket client(client_context);
    boost::system::error_code error;
    client.connect(endpoint, error);
    EXPECT_TRUE(error);
}

TEST(AsyncHttpServerTest, DrainClosesIncompleteSessionBeforeDeadline) {
    RunningServer server;
    asio::io_context client_context;
    tcp::socket client(client_context);
    client.connect(server.endpoint());
    asio::write(client, asio::buffer("GET /healthz HTTP/1.1\r\nHo"));
    ASSERT_TRUE(waitUntil([&server] { return server.connectionCount() == 1U; }));

    std::promise<bool> completed;
    server.beginDrain(std::chrono::milliseconds(20),
                      [&completed](bool drained) { completed.set_value(drained); });
    EXPECT_TRUE(completed.get_future().get());
    EXPECT_TRUE(waitForClose(client));
    EXPECT_TRUE(
        waitUntil([&server] { return server.closedCount(StopReason::ServerShutdown) == 1U; }));
}

TEST(AsyncHttpServerTest, DrainLetsSlowProxyRequestFinishBeforeDeadline) {
    MockUpstream upstream({"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nok\n"}, 1,
                          std::chrono::milliseconds(30));
    const auto endpoint = upstream.endpoint();
    ProxyRunningServer gateway(
        {{.host = endpoint.address().to_string(), .service = std::to_string(endpoint.port())}});

    std::promise<std::string> response;
    std::jthread request([&] {
        try {
            response.set_value(exchange(gateway, {"GET /proxy/slow HTTP/1.1\r\nHost: localhost\r\n"
                                                  "Connection: close\r\n\r\n"}));
        } catch (...) {
            response.set_exception(std::current_exception());
        }
    });
    ASSERT_TRUE(waitUntil([&upstream] { return upstream.requestStarted(); }));

    std::promise<bool> completed;
    gateway.beginDrain(std::chrono::milliseconds(250),
                       [&completed](bool drained) { completed.set_value(drained); });
    EXPECT_TRUE(completed.get_future().get());
    EXPECT_NE(response.get_future().get().find("HTTP/1.1 200 OK\r\n"), std::string::npos);
}

TEST(AsyncHttpServerTest, DrainForceClosesSlowProxyRequestAfterDeadline) {
    MockUpstream upstream({"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nok\n"}, 1,
                          std::chrono::milliseconds(500));
    const auto endpoint = upstream.endpoint();
    ProxyRunningServer gateway(
        {{.host = endpoint.address().to_string(), .service = std::to_string(endpoint.port())}});

    std::promise<void> request_finished;
    std::jthread request([&] {
        try {
            static_cast<void>(exchange(gateway, {"GET /proxy/slow HTTP/1.1\r\nHost: localhost\r\n"
                                                 "Connection: close\r\n\r\n"}));
        } catch (...) {
        }
        request_finished.set_value();
    });
    ASSERT_TRUE(waitUntil([&upstream] { return upstream.requestStarted(); }));

    std::promise<bool> completed;
    gateway.beginDrain(std::chrono::milliseconds(30),
                       [&completed](bool drained) { completed.set_value(drained); });
    EXPECT_FALSE(completed.get_future().get());
    EXPECT_EQ(request_finished.get_future().wait_for(std::chrono::milliseconds(200)),
              std::future_status::ready);
    EXPECT_TRUE(waitUntil([&upstream] { return upstream.received(); }));
}

TEST(AsyncHttpServerTest, DrainsRegistryAfterManyMultiWorkerConnections) {
    constexpr std::size_t kConnectionCount = 300;
    MultiThreadedRunningServer server(4);
    asio::io_context client_context;
    std::vector<tcp::socket> clients;
    clients.reserve(kConnectionCount);

    for (std::size_t index = 0; index < kConnectionCount; ++index) {
        clients.emplace_back(client_context);
        clients.back().connect(server.endpoint());
        asio::write(clients.back(), asio::buffer(kHealthRequest));
    }
    for (auto& client : clients) {
        ResponseReader reader(client);
        EXPECT_NE(reader.readResponse().find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    }
    EXPECT_TRUE(waitUntil([&server] { return server.connectionCount() == 0U; }));
}

}  // namespace
