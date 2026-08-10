#include <gtest/gtest.h>

#include <array>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/system_error.hpp>
#include <charconv>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "pulsegate/http/http_server.h"

namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using pulsegate::http::HttpServer;
using pulsegate::net::ListenConfig;

constexpr std::string_view kHealthRequest =
    "GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";

class RunningServer {
   public:
    RunningServer() : server_(context_, ListenConfig{.host = "127.0.0.1", .port = 0}) {
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

   private:
    asio::io_context context_{1};
    HttpServer server_;
    std::jthread thread_;
};

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

std::string exchange(RunningServer& server, const std::vector<std::string_view>& fragments) {
    asio::io_context client_context;
    tcp::socket client(client_context);
    client.connect(server.endpoint());
    for (const auto fragment : fragments) {
        asio::write(client, asio::buffer(fragment));
    }

    ResponseReader reader(client);
    return reader.readResponse();
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

}  // namespace
