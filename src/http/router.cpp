#include "pulsegate/http/router.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

namespace pulsegate::http {
namespace {

std::string methodName(HttpMethod method) {
    switch (method) {
        case HttpMethod::Get:
            return "GET";
        case HttpMethod::Head:
            return "HEAD";
        case HttpMethod::Post:
            return "POST";
        case HttpMethod::Put:
            return "PUT";
        case HttpMethod::Delete:
            return "DELETE";
        case HttpMethod::Options:
            return "OPTIONS";
        case HttpMethod::Unknown:
            return "UNKNOWN";
    }
    return "UNKNOWN";
}

}  // namespace

Router::Router(std::optional<RateLimitConfig> global_rate_limit)
    : routes_(std::make_shared<const Routes>()),
      route_limiters_(std::make_shared<const RouteLimiters>()),
      route_caches_(std::make_shared<const RouteCaches>()) {
    if (global_rate_limit) global_limiter_ = std::make_shared<RateLimiter>(*global_rate_limit);
}

void Router::add(Route route, std::optional<RateLimitConfig> rate_limit,
                 std::optional<ResponseCacheConfig> cache) {
    if (route.pattern.empty() || route.pattern.front() != '/' || route.name.empty() ||
        !route.handler) {
        throw std::invalid_argument("route requires an absolute pattern, name, and handler");
    }
    std::scoped_lock lock(update_mutex_);
    auto current = routes_.load(std::memory_order_acquire);
    auto updated = std::make_shared<Routes>(*current);
    auto current_limiters = route_limiters_.load(std::memory_order_acquire);
    auto updated_limiters = std::make_shared<RouteLimiters>(*current_limiters);
    auto current_caches = route_caches_.load(std::memory_order_acquire);
    auto updated_caches = std::make_shared<RouteCaches>(*current_caches);
    if (rate_limit) (*updated_limiters)[route.name] = std::make_shared<RateLimiter>(*rate_limit);
    if (cache) (*updated_caches)[route.name] = std::make_shared<ResponseCache>(*cache);
    updated->push_back(std::move(route));
    routes_.store(std::const_pointer_cast<const Routes>(updated), std::memory_order_release);
    route_limiters_.store(std::const_pointer_cast<const RouteLimiters>(updated_limiters),
                          std::memory_order_release);
    route_caches_.store(std::const_pointer_cast<const RouteCaches>(updated_caches),
                        std::memory_order_release);
}

void Router::setObservability(std::shared_ptr<Observability> observability) {
    std::scoped_lock lock(update_mutex_);
    observability_ = std::move(observability);
}

std::shared_ptr<Observability> Router::observability() const {
    std::scoped_lock lock(update_mutex_);
    return observability_;
}

void Router::setReadyCallback(std::function<bool()> callback) {
    std::scoped_lock lock(update_mutex_);
    ready_callback_ = std::move(callback);
}

bool Router::ready() const {
    std::scoped_lock lock(update_mutex_);
    return !ready_callback_ || ready_callback_();
}

std::optional<Route> Router::match(HttpMethod method, std::string_view target) const {
    const auto path = pathPart(target);
    const auto snapshot = routes_.load(std::memory_order_acquire);
    const auto iterator = std::find_if(
        snapshot->begin(), snapshot->end(),
        [method, path](const Route& route) { return routeMatches(route, method, path); });
    if (iterator == snapshot->end()) {
        return std::nullopt;
    }
    return *iterator;
}

net::Awaitable<HttpResponse> Router::handle(RequestContext& context, HttpRequest request) const {
    if (global_limiter_) {
        const auto decision = global_limiter_->check(clientKey(context));
        if (!decision.allowed) {
            auto response = makeRateLimited(decision);
            response.headers.set("X-Request-Id", context.request_id);
            context.route_name = "global";
            if (observability_) observability_->metrics().recordRateLimitRejection("global");
            co_return response;
        }
    }
    const auto path = pathPart(request.target);
    const auto snapshot = routes_.load(std::memory_order_acquire);
    const auto iterator = std::find_if(
        snapshot->begin(), snapshot->end(),
        [&request, path](const Route& route) { return routeMatches(route, request.method, path); });

    HttpResponse response;
    if (iterator == snapshot->end()) {
        bool path_exists = false;
        std::string allow;
        for (const auto& route : *snapshot) {
            if (route.pattern == path || (route.prefix_match && path.starts_with(route.pattern))) {
                path_exists = true;
                if (!allow.empty()) {
                    allow.append(", ");
                }
                allow.append(methodName(route.method));
            }
        }
        response = path_exists ? makeError(405, "Method Not Allowed", "method not allowed\n")
                               : makeError(404, "Not Found", "not found\n");
        if (path_exists) {
            response.headers.add("Allow", allow);
        }
    } else {
        context.route_name = iterator->name;
        const auto route_limiters = route_limiters_.load(std::memory_order_acquire);
        const auto limiter = route_limiters->find(iterator->name);
        bool allowed = true;
        std::optional<RateLimitDecision> decision;
        if (limiter != route_limiters->end()) {
            decision = limiter->second->check(clientKey(context));
            allowed = decision->allowed;
        }
        if (!allowed) {
            response = makeRateLimited(*decision);
            if (observability_) observability_->metrics().recordRateLimitRejection(iterator->name);
        } else {
            const auto route_caches = route_caches_.load(std::memory_order_acquire);
            const auto cache = route_caches->find(iterator->name);
            const bool cache_configured = cache != route_caches->end();
            const bool cacheable_request =
                cache_configured && ResponseCache::cacheableRequest(request);
            const bool can_store_response = request.method == HttpMethod::Get;
            std::optional<std::string> cache_key;
            if (cacheable_request) {
                cache_key = ResponseCache::makeKey(request, cache->second->config());
                if (auto cached = cache->second->get(*cache_key)) {
                    response = std::move(*cached);
                    response.headers.set("X-Cache", "HIT");
                    response.headers.set("X-Request-Id", context.request_id);
                    context.cache_status = "HIT";
                    if (observability_) observability_->metrics().recordCache("hit");
                    co_return response;
                }
                context.buffer_response_for_cache = true;
            }
            try {
                response = co_await iterator->handler(context, std::move(request));
            } catch (...) {
                response = makeError(500, "Internal Server Error", "internal server error\n");
            }
            context.buffer_response_for_cache = false;
            if (cacheable_request) {
                if (can_store_response &&
                    ResponseCache::cacheableResponse(response, cache->second->config()) &&
                    cache->second->put(std::move(*cache_key), response)) {
                    response.headers.set("X-Cache", "MISS");
                    context.cache_status = "MISS";
                    if (observability_) observability_->metrics().recordCache("miss");
                } else {
                    response.headers.set("X-Cache", "BYPASS");
                    context.cache_status = "BYPASS";
                    if (observability_) observability_->metrics().recordCache("bypass");
                }
            } else if (cache_configured) {
                response.headers.set("X-Cache", "BYPASS");
                context.cache_status = "BYPASS";
                if (observability_) observability_->metrics().recordCache("bypass");
            }
        }
    }
    response.headers.set("X-Request-Id", context.request_id);
    co_return response;
}

std::string Router::nextRequestId() {
    return std::to_string(next_request_id_.fetch_add(1, std::memory_order_relaxed));
}

std::string Router::rateLimitMetrics() const {
    const auto append = [](std::string& output, std::string_view scope,
                           const RateLimitStats& stats) {
        const auto add = [&output, scope](std::string_view outcome, std::uint64_t value) {
            output.append("pulsegate_rate_limit_requests_total{scope=\"");
            output.append(scope);
            output.append("\",outcome=\"");
            output.append(outcome);
            output.append("\"} ");
            output.append(std::to_string(value));
            output.push_back('\n');
        };
        add("allowed", stats.allowed);
        add("rejected", stats.rejected);
        add("key_capacity", stats.rejected_key_capacity);
    };

    std::string output;
    output.append("# TYPE pulsegate_rate_limit_requests_total counter\n");
    if (global_limiter_) append(output, "global", global_limiter_->stats());

    RateLimitStats routes;
    const auto route_limiters = route_limiters_.load(std::memory_order_acquire);
    for (const auto& [name, limiter] : *route_limiters) {
        static_cast<void>(name);
        const auto stats = limiter->stats();
        routes.allowed += stats.allowed;
        routes.rejected += stats.rejected;
        routes.rejected_key_capacity += stats.rejected_key_capacity;
    }
    if (!route_limiters->empty()) append(output, "route", routes);
    return output;
}

std::string Router::cacheMetrics() const {
    const auto caches = route_caches_.load(std::memory_order_acquire);
    if (caches->empty()) return {};

    CacheStats totals;
    for (const auto& [name, cache] : *caches) {
        static_cast<void>(name);
        const auto stats = cache->stats();
        totals.hits += stats.hits;
        totals.misses += stats.misses;
        totals.stores += stats.stores;
        totals.evictions += stats.evictions;
        totals.expired += stats.expired;
    }
    std::string output{"# TYPE pulsegate_response_cache_operations_total counter\n"};
    const auto append = [&output](std::string_view operation, std::uint64_t value) {
        output.append("pulsegate_response_cache_operations_total{operation=\"");
        output.append(operation);
        output.append("\"} ");
        output.append(std::to_string(value));
        output.push_back('\n');
    };
    append("hit", totals.hits);
    append("miss", totals.misses);
    append("store", totals.stores);
    append("eviction", totals.evictions);
    append("expired", totals.expired);
    return output;
}

std::string_view Router::pathPart(std::string_view target) noexcept {
    const auto query = target.find('?');
    return query == std::string_view::npos ? target : target.substr(0, query);
}

bool Router::routeMatches(const Route& route, HttpMethod method, std::string_view path) noexcept {
    if (route.method != HttpMethod::Unknown && route.method != method) {
        return false;
    }
    return route.prefix_match ? path.starts_with(route.pattern) : path == route.pattern;
}

HttpResponse Router::makeError(int status_code, std::string reason, std::string body) {
    HttpResponse response;
    response.status_code = status_code;
    response.reason = std::move(reason);
    response.body = std::move(body);
    response.headers.add("Content-Type", "text/plain");
    return response;
}

HttpResponse Router::makeRateLimited(const RateLimitDecision& decision) {
    auto response = makeError(429, "Too Many Requests", "rate limit exceeded\n");
    const auto milliseconds = decision.retry_after.count();
    const auto seconds = std::max<decltype(milliseconds)>(1, (milliseconds + 999) / 1000);
    response.headers.add("Retry-After", std::to_string(seconds));
    return response;
}

std::string Router::clientKey(const RequestContext& context) {
    return context.peer.address().is_unspecified() ? "unknown" : context.peer.address().to_string();
}

}  // namespace pulsegate::http
