#include "pulsegate/net/sync_http_server.h"

#include <gtest/gtest.h>

#include <array>
#include <boost/asio/buffer.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>
#include <cstddef>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using pulsegate::net::ListenConfig;
using pulsegate::net::SyncHttpServer;

constexpr std::string_view kValidRequest = "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";

std::string readUntilClose(tcp::socket& socket) {
    std::array<char, 1024> buffer{};
    std::string response;

    for (;;) {
        boost::system::error_code error;
        const auto bytes_read = socket.read_some(asio::buffer(buffer), error);
        response.append(buffer.data(), bytes_read);
        if (error == asio::error::eof || error == asio::error::connection_reset) {
            return response;
        }
        if (error) {
            throw boost::system::system_error(error, "read test response");
        }
    }
}

std::string exchange(const std::vector<std::string_view>& fragments) {
    asio::io_context server_context;
    SyncHttpServer server(server_context, ListenConfig{.host = "127.0.0.1", .port = 0});
    boost::system::error_code server_error;
    std::jthread server_thread([&server, &server_error] { server.run(1, server_error); });

    asio::io_context client_context;
    tcp::socket client(client_context);
    client.connect(server.localEndpoint());
    for (const auto fragment : fragments) {
        asio::write(client, asio::buffer(fragment));
    }

    auto response = readUntilClose(client);
    server_thread.join();
    if (server_error) {
        throw boost::system::system_error(server_error, "test accept loop");
    }
    return response;
}

TEST(SyncHttpServerTest, ReturnsMinimalResponseForValidGet) {
    const auto response = exchange({kValidRequest});

    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(response.find("Content-Type: text/plain\r\n"), std::string::npos);
    EXPECT_NE(response.find("Content-Length: 12\r\n"), std::string::npos);
    EXPECT_NE(response.find("Connection: close\r\n"), std::string::npos);
    EXPECT_TRUE(response.ends_with("\r\n\r\nhello world\n"));
}

TEST(SyncHttpServerTest, AcceptsAHeaderSplitAcrossTcpWrites) {
    const auto response = exchange({"GET / HTTP/1.1\r\nHo", "st: local", "host\r\n", "\r\n"});

    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
}

TEST(SyncHttpServerTest, StopsHandlingWhenClientClosesMidHeader) {
    asio::io_context server_context;
    SyncHttpServer server(server_context, ListenConfig{.host = "127.0.0.1", .port = 0});
    boost::system::error_code server_error;
    std::jthread server_thread([&server, &server_error] { server.run(1, server_error); });

    asio::io_context client_context;
    tcp::socket client(client_context);
    client.connect(server.localEndpoint());
    asio::write(client, asio::buffer("GET / HTTP/1.1\r\nHost:"));
    client.close();

    server_thread.join();
    EXPECT_FALSE(server_error);
}

TEST(SyncHttpServerTest, SurvivesClientClosingBeforeReadingResponse) {
    asio::io_context server_context;
    SyncHttpServer server(server_context, ListenConfig{.host = "127.0.0.1", .port = 0});
    boost::system::error_code server_error;
    std::jthread server_thread([&server, &server_error] { server.run(1, server_error); });

    asio::io_context client_context;
    tcp::socket client(client_context);
    client.connect(server.localEndpoint());
    asio::write(client, asio::buffer(kValidRequest));
    client.set_option(asio::socket_base::linger(true, 0));
    client.close();

    server_thread.join();
    EXPECT_FALSE(server_error);
}

TEST(SyncHttpServerTest, RejectsHeaderLargerThanSixteenKibibytes) {
    std::string oversized_header = "GET / HTTP/1.1\r\nX-Fill: ";
    oversized_header.append(17 * 1024, 'x');

    const auto response = exchange({oversized_header});

    EXPECT_NE(response.find("HTTP/1.1 431 Request Header Fields Too Large\r\n"), std::string::npos);
}

TEST(SyncHttpServerTest, RejectsMalformedRequestLine) {
    const auto response = exchange({"GET /missing-version\r\n\r\n"});

    EXPECT_NE(response.find("HTTP/1.1 400 Bad Request\r\n"), std::string::npos);
}

TEST(SyncHttpServerTest, RejectsMethodsOtherThanGet) {
    const auto response = exchange({"POST / HTTP/1.1\r\nHost: localhost\r\n\r\n"});

    EXPECT_NE(response.find("HTTP/1.1 405 Method Not Allowed\r\n"), std::string::npos);
    EXPECT_NE(response.find("Allow: GET\r\n"), std::string::npos);
}

TEST(SyncHttpServerTest, ReleasesThePortWhenDestroyed) {
    asio::io_context first_context;
    std::uint16_t port = 0;
    {
        SyncHttpServer first(first_context, ListenConfig{.host = "127.0.0.1", .port = 0});
        port = first.port();
    }

    asio::io_context second_context;
    EXPECT_NO_THROW(
        (SyncHttpServer(second_context, ListenConfig{.host = "127.0.0.1", .port = port})));
}

TEST(SyncHttpServerTest, ServesOneHundredSequentialRequests) {
    constexpr std::size_t request_count = 100;
    asio::io_context server_context;
    SyncHttpServer server(server_context, ListenConfig{.host = "127.0.0.1", .port = 0});
    boost::system::error_code server_error;
    std::jthread server_thread(
        [&server, &server_error] { server.run(request_count, server_error); });

    for (std::size_t index = 0; index < request_count; ++index) {
        asio::io_context client_context;
        tcp::socket client(client_context);
        client.connect(server.localEndpoint());
        asio::write(client, asio::buffer(kValidRequest));
        const auto response = readUntilClose(client);
        EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos)
            << "request index: " << index;
    }

    server_thread.join();
    EXPECT_FALSE(server_error);
}

}  // namespace
