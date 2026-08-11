#include "pulsegate/http/router.h"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_future.hpp>
#include <chrono>
#include <stdexcept>

namespace {

using namespace std::chrono_literals;
using pulsegate::http::HttpMethod;
using pulsegate::http::HttpRequest;
using pulsegate::http::HttpResponse;
using pulsegate::http::RateLimitConfig;
using pulsegate::http::RequestContext;
using pulsegate::http::ResponseCacheConfig;
using pulsegate::http::Route;
using pulsegate::http::Router;

HttpResponse invoke(Router& router, HttpRequest request) {
    boost::asio::io_context context;
    RequestContext request_context{.executor = context.get_executor(),
                                   .request_id = "test-request",
                                   .peer = {},
                                   .downstream = {},
                                   .set_current_proxy = {},
                                   .write_downstream = {}};
    auto future = boost::asio::co_spawn(context, router.handle(request_context, std::move(request)),
                                        boost::asio::use_future);
    context.run();
    return future.get();
}

TEST(RouterTest, MatchesExactAndPrefixRoutesWithoutNetwork) {
    Router router;
    router.add(Route{
        .method = HttpMethod::Get,
        .pattern = "/items",
        .name = "items",
        .handler = [](RequestContext&, HttpRequest) -> pulsegate::net::Awaitable<HttpResponse> {
            HttpResponse response;
            response.body = "items\n";
            co_return response;
        }});
    router.add(Route{
        .method = HttpMethod::Get,
        .pattern = "/static/",
        .name = "static",
        .prefix_match = true,
        .handler = [](RequestContext&, HttpRequest) -> pulsegate::net::Awaitable<HttpResponse> {
            HttpResponse response;
            response.body = "static\n";
            co_return response;
        }});

    EXPECT_TRUE(router.match(HttpMethod::Get, "/items?offset=1"));
    EXPECT_TRUE(router.match(HttpMethod::Get, "/static/site.css"));
    EXPECT_FALSE(router.match(HttpMethod::Get, "/items/42"));
}

TEST(RouterTest, MapsNotFoundAndMethodNotAllowedWithRequestId) {
    Router router;
    router.add(Route{.method = HttpMethod::Get,
                     .pattern = "/only-get",
                     .name = "only_get",
                     .handler = [](RequestContext&, HttpRequest)
                         -> pulsegate::net::Awaitable<HttpResponse> { co_return HttpResponse{}; }});

    auto missing = invoke(router, HttpRequest{.method = HttpMethod::Get,
                                              .target = "/missing",
                                              .version_major = 1,
                                              .version_minor = 1,
                                              .headers = {},
                                              .body = {}});
    EXPECT_EQ(missing.status_code, 404);
    EXPECT_EQ(missing.headers.get("x-request-id"), "test-request");

    auto wrong_method = invoke(router, HttpRequest{.method = HttpMethod::Post,
                                                   .target = "/only-get",
                                                   .version_major = 1,
                                                   .version_minor = 1,
                                                   .headers = {},
                                                   .body = {}});
    EXPECT_EQ(wrong_method.status_code, 405);
    EXPECT_EQ(wrong_method.headers.get("allow"), "GET");
    EXPECT_EQ(wrong_method.headers.get("x-request-id"), "test-request");
}

TEST(RouterTest, AwaitsHandlerAndMapsExceptionToInternalServerError) {
    Router router;
    router.add(Route{.method = HttpMethod::Get,
                     .pattern = "/later",
                     .name = "later",
                     .handler = [](RequestContext& context,
                                   HttpRequest) -> pulsegate::net::Awaitable<HttpResponse> {
                         boost::asio::steady_timer timer(context.executor);
                         timer.expires_after(1ms);
                         co_await timer.async_wait(pulsegate::net::use_awaitable);
                         HttpResponse response;
                         response.body = "later\n";
                         co_return response;
                     }});
    router.add(Route{
        .method = HttpMethod::Get,
        .pattern = "/throws",
        .name = "throws",
        .handler = [](RequestContext&, HttpRequest) -> pulsegate::net::Awaitable<HttpResponse> {
            throw std::runtime_error("handler failure");
            co_return HttpResponse{};
        }});

    EXPECT_EQ(invoke(router, HttpRequest{.method = HttpMethod::Get,
                                         .target = "/later",
                                         .version_major = 1,
                                         .version_minor = 1,
                                         .headers = {},
                                         .body = {}})
                  .body,
              "later\n");
    auto failed = invoke(router, HttpRequest{.method = HttpMethod::Get,
                                             .target = "/throws",
                                             .version_major = 1,
                                             .version_minor = 1,
                                             .headers = {},
                                             .body = {}});
    EXPECT_EQ(failed.status_code, 500);
    EXPECT_EQ(failed.headers.get("x-request-id"), "test-request");
}

TEST(RouterTest, RejectsBeforeInvokingARouteHandlerWhenRouteLimitIsExhausted) {
    Router router;
    int calls = 0;
    router.add(Route{.method = HttpMethod::Get,
                     .pattern = "/limited",
                     .name = "limited",
                     .handler = [&calls](RequestContext&,
                                         HttpRequest) -> pulsegate::net::Awaitable<HttpResponse> {
                         ++calls;
                         co_return HttpResponse{};
                     }},
               RateLimitConfig{.requests_per_second = 1.0,
                               .burst = 1.0,
                               .per_client = true,
                               .shard_count = 2,
                               .max_keys = 8,
                               .idle_ttl = std::chrono::seconds(1)});

    const HttpRequest request{.method = HttpMethod::Get,
                              .target = "/limited",
                              .version_major = 1,
                              .version_minor = 1,
                              .headers = {},
                              .body = {}};
    EXPECT_EQ(invoke(router, request).status_code, 200);
    const auto rejected = invoke(router, request);
    EXPECT_EQ(rejected.status_code, 429);
    EXPECT_EQ(rejected.headers.get("retry-after"), "1");
    EXPECT_EQ(rejected.headers.get("x-request-id"), "test-request");
    EXPECT_EQ(calls, 1);
}

TEST(RouterTest, PreservesExceptionMappingForAnAllowedRateLimitedRoute) {
    Router router;
    router.add(Route{.method = HttpMethod::Get,
                     .pattern = "/limited-throws",
                     .name = "limited-throws",
                     .handler = [](RequestContext&,
                                   HttpRequest) -> pulsegate::net::Awaitable<HttpResponse> {
                         throw std::runtime_error("handler failure");
                         co_return HttpResponse{};
                     }},
               RateLimitConfig{.requests_per_second = 1.0, .burst = 1.0, .per_client = false});

    const auto response = invoke(router, HttpRequest{.method = HttpMethod::Get,
                                                     .target = "/limited-throws",
                                                     .version_major = 1,
                                                     .version_minor = 1,
                                                     .headers = {},
                                                     .body = {}});
    EXPECT_EQ(response.status_code, 500);
    EXPECT_EQ(response.headers.get("x-request-id"), "test-request");
}

TEST(RouterTest, AppliesAGlobalLimitBeforeRouteLookup) {
    Router router(RateLimitConfig{.requests_per_second = 1.0, .burst = 1.0, .per_client = false});
    const HttpRequest request{.method = HttpMethod::Get,
                              .target = "/not-configured",
                              .version_major = 1,
                              .version_minor = 1,
                              .headers = {},
                              .body = {}};

    EXPECT_EQ(invoke(router, request).status_code, 404);
    const auto rejected = invoke(router, request);
    EXPECT_EQ(rejected.status_code, 429);
    EXPECT_EQ(rejected.headers.get("retry-after"), "1");
    EXPECT_NE(router.rateLimitMetrics().find("scope=\"global\",outcome=\"rejected\"} 1"),
              std::string::npos);
}

TEST(RouterTest, CachesExplicitGetAndHeadRoutesWithoutCachingSensitiveRequests) {
    Router router;
    int calls = 0;
    const ResponseCacheConfig cache{.ttl = std::chrono::seconds(1),
                                    .max_entry_bytes = 128,
                                    .max_bytes = 1024,
                                    .shard_count = 2,
                                    .vary_headers = {}};
    const auto handler = [&calls](RequestContext&,
                                  HttpRequest) -> pulsegate::net::Awaitable<HttpResponse> {
        ++calls;
        HttpResponse response;
        response.body = "cached\n";
        co_return response;
    };
    router.add(
        Route{
            .method = HttpMethod::Get, .pattern = "/cached", .name = "cached", .handler = handler},
        std::nullopt, cache);
    router.add(
        Route{
            .method = HttpMethod::Head, .pattern = "/cached", .name = "cached", .handler = handler},
        std::nullopt, cache);

    const HttpRequest get{.method = HttpMethod::Get,
                          .target = "/cached",
                          .version_major = 1,
                          .version_minor = 1,
                          .headers = {},
                          .body = {}};
    const auto first = invoke(router, get);
    const auto second = invoke(router, get);
    EXPECT_EQ(first.headers.get("x-cache"), "MISS");
    EXPECT_EQ(second.headers.get("x-cache"), "HIT");
    EXPECT_EQ(calls, 1);

    const auto head = invoke(router, HttpRequest{.method = HttpMethod::Head,
                                                 .target = "/cached",
                                                 .version_major = 1,
                                                 .version_minor = 1,
                                                 .headers = {},
                                                 .body = {}});
    EXPECT_EQ(head.headers.get("x-cache"), "HIT");
    EXPECT_EQ(head.serialize(true).find("cached\n"), std::string::npos);
    EXPECT_EQ(calls, 1);

    auto sensitive = get;
    sensitive.headers.add("Authorization", "Bearer secret");
    EXPECT_EQ(invoke(router, std::move(sensitive)).headers.get("x-cache"), "BYPASS");
    EXPECT_EQ(calls, 2);
    EXPECT_NE(router.cacheMetrics().find("operation=\"hit\"} 2"), std::string::npos);
}

}  // namespace
