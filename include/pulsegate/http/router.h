#pragma once

#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "pulsegate/http/http_request.h"
#include "pulsegate/http/http_response.h"
#include "pulsegate/http/observability.h"
#include "pulsegate/http/rate_limiter.h"
#include "pulsegate/http/response_cache.h"
#include "pulsegate/net/asio_types.h"

namespace pulsegate::http {

class HttpSession;
class ProxySession;

struct RequestContext {
    net::asio::any_io_executor executor;
    std::string request_id;
    net::tcp::endpoint peer;
    std::weak_ptr<HttpSession> downstream;
    // A proxy handler registers its in-flight transaction here. HttpSession
    // invokes it only from its own executor when shutdown/close is observed.
    std::function<void(std::weak_ptr<ProxySession>)> set_current_proxy;
    std::function<net::Awaitable<bool>(std::string)> write_downstream;
    // Cache-enabled proxy routes use their complete-response path on a miss so
    // the Router can decide whether the response is eligible for storage.
    bool buffer_response_for_cache{false};
    std::string route_name{"unmatched"};
    std::string cache_status{"BYPASS"};
    std::shared_ptr<Observability> observability{};
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
    explicit Router(std::optional<RateLimitConfig> global_rate_limit = std::nullopt);

    // Configure before serving or publish a new immutable route-table snapshot.
    void add(Route route, std::optional<RateLimitConfig> rate_limit = std::nullopt,
             std::optional<ResponseCacheConfig> cache = std::nullopt);
    [[nodiscard]] std::optional<Route> match(HttpMethod method, std::string_view target) const;
    net::Awaitable<HttpResponse> handle(RequestContext& context, HttpRequest request) const;
    [[nodiscard]] std::string nextRequestId();
    [[nodiscard]] std::string rateLimitMetrics() const;
    [[nodiscard]] std::string cacheMetrics() const;
    void setObservability(std::shared_ptr<Observability> observability);
    [[nodiscard]] std::shared_ptr<Observability> observability() const;
    void setReadyCallback(std::function<bool()> callback);
    [[nodiscard]] bool ready() const;

   private:
    using Routes = std::vector<Route>;
    using RouteLimiters = std::unordered_map<std::string, std::shared_ptr<RateLimiter>>;
    using RouteCaches = std::unordered_map<std::string, std::shared_ptr<ResponseCache>>;

    [[nodiscard]] static std::string_view pathPart(std::string_view target) noexcept;
    [[nodiscard]] static bool routeMatches(const Route& route, HttpMethod method,
                                           std::string_view path) noexcept;
    [[nodiscard]] static HttpResponse makeError(int status_code, std::string reason,
                                                std::string body);
    [[nodiscard]] static HttpResponse makeRateLimited(const RateLimitDecision& decision);
    [[nodiscard]] static std::string clientKey(const RequestContext& context);

    std::atomic<std::shared_ptr<const Routes>> routes_;
    std::atomic<std::shared_ptr<const RouteLimiters>> route_limiters_;
    std::atomic<std::shared_ptr<const RouteCaches>> route_caches_;
    mutable std::mutex update_mutex_;
    std::shared_ptr<RateLimiter> global_limiter_;
    std::shared_ptr<Observability> observability_;
    std::function<bool()> ready_callback_;
    std::atomic_uint64_t next_request_id_{1};
};

}  // namespace pulsegate::http
