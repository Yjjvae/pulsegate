#pragma once

#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pulsegate/http/http_request.h"
#include "pulsegate/http/http_response.h"
#include "pulsegate/net/asio_types.h"

namespace pulsegate::http {

class HttpSession;

struct RequestContext {
    net::asio::any_io_executor executor;
    std::string request_id;
    net::tcp::endpoint peer;
    std::weak_ptr<HttpSession> downstream;
};

using HttpHandler = std::function<net::Awaitable<HttpResponse>(RequestContext&, HttpRequest)>;

struct Route {
    HttpMethod method{HttpMethod::Unknown};
    std::string pattern;
    std::string name;
    bool prefix_match{false};
    HttpHandler handler;
};

class Router {
   public:
    Router();

    // Configure before serving or publish a new immutable route-table snapshot.
    void add(Route route);
    [[nodiscard]] std::optional<Route> match(HttpMethod method, std::string_view target) const;
    net::Awaitable<HttpResponse> handle(RequestContext& context, HttpRequest request) const;
    [[nodiscard]] std::string nextRequestId();

   private:
    using Routes = std::vector<Route>;

    [[nodiscard]] static std::string_view pathPart(std::string_view target) noexcept;
    [[nodiscard]] static bool routeMatches(const Route& route, HttpMethod method,
                                           std::string_view path) noexcept;
    [[nodiscard]] static HttpResponse makeError(int status_code, std::string reason,
                                                std::string body);

    std::atomic<std::shared_ptr<const Routes>> routes_;
    std::mutex update_mutex_;
    std::atomic_uint64_t next_request_id_{1};
};

}  // namespace pulsegate::http
