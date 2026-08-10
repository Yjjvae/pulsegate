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
using pulsegate::http::RequestContext;
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

}  // namespace
