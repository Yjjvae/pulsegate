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
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "pulsegate/http/http_server.h"
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
    for (int attempt = 0; attempt < 100; ++attempt) {
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
