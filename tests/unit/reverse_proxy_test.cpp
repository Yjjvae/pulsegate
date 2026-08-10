#include "pulsegate/http/reverse_proxy.h"

#include <gtest/gtest.h>

namespace pulsegate::http {
namespace {

RequestContext context() {
    boost::asio::io_context io;
    return {.executor = io.get_executor(),
            .request_id = "request-42",
            .peer = {boost::asio::ip::make_address("192.0.2.9"), 4567},
            .downstream = {},
            .set_current_proxy = {},
            .write_downstream = {}};
}

TEST(ReverseProxyTest, RewritesRequestAndStripsHopByHopHeaders) {
    HttpRequest request;
    request.method = HttpMethod::Post;
    request.target = "/proxy/api/items";
    request.headers.add("Host", "untrusted.example");
    request.headers.add("Connection", "keep-alive, X-Remove");
    request.headers.add("Keep-Alive", "timeout=5");
    request.headers.add("X-Remove", "no");
    request.headers.add("X-Forwarded-For", "spoofed");
    request.headers.add("Content-Type", "application/json");
    request.headers.add("Content-Length", "999");
    request.body = "{}";

    const auto bytes = serializeUpstreamRequest(
        request, UpstreamEndpoint{.host = "backend.local", .service = "8081"}, context());
    EXPECT_TRUE(bytes.starts_with("POST /proxy/api/items HTTP/1.1\r\n"));
    EXPECT_NE(bytes.find("Host: backend.local:8081\r\n"), std::string::npos);
    EXPECT_NE(bytes.find("content-type: application/json\r\n"), std::string::npos);
    EXPECT_NE(bytes.find("X-Forwarded-For: 192.0.2.9\r\n"), std::string::npos);
    EXPECT_NE(bytes.find("X-Request-Id: request-42\r\n"), std::string::npos);
    EXPECT_NE(bytes.find("Content-Length: 2\r\n"), std::string::npos);
    EXPECT_NE(bytes.find("Connection: keep-alive\r\n"), std::string::npos);
    EXPECT_EQ(bytes.find("keep-alive:"), std::string::npos);
    EXPECT_EQ(bytes.find("x-remove:"), std::string::npos);
    EXPECT_EQ(bytes.find("spoofed"), std::string::npos);
}

TEST(ReverseProxyTest, MapsStructuredErrorsWithoutLeakingTransportDetails) {
    EXPECT_EQ(makeProxyErrorResponse(ProxyError::DnsFailure).status_code, 502);
    EXPECT_EQ(makeProxyErrorResponse(ProxyError::ConnectTimeout).status_code, 504);
    EXPECT_EQ(makeProxyErrorResponse(ProxyError::ResponseTimeout).status_code, 504);
    EXPECT_EQ(makeProxyErrorResponse(ProxyError::Cancelled).status_code, 503);
    EXPECT_EQ(makeProxyErrorResponse(ProxyError::UnsupportedRequest).status_code, 501);
}

TEST(ReverseProxyTest, RequiresAtLeastOneWellFormedUpstream) {
    EXPECT_THROW((ReverseProxy({}, {})), std::invalid_argument);
    EXPECT_THROW((ReverseProxy({UpstreamEndpoint{}}, {})), std::invalid_argument);
}

TEST(ReverseProxyTest, BuildsHealthSnapshotUsingConfiguredThresholds) {
    ProxyLimits limits;
    limits.health_thresholds = {.healthy_threshold = 1, .unhealthy_threshold = 1};
    ReverseProxy proxy({UpstreamEndpoint{.host = "backend.local", .service = "8081"}}, limits);
    const auto health = proxy.health();
    health->recordFailure("backend.local:8081");
    EXPECT_FALSE(health->isHealthy("backend.local:8081"));

    limits.health_thresholds.healthy_threshold = 0;
    EXPECT_THROW(
        (ReverseProxy({UpstreamEndpoint{.host = "backend.local", .service = "8081"}}, limits)),
        std::invalid_argument);
}

}  // namespace
}  // namespace pulsegate::http
