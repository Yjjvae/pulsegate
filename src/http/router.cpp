#include "pulsegate/http/router.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
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

Router::Router() : routes_(std::make_shared<const Routes>()) {}

void Router::add(Route route) {
    if (route.pattern.empty() || route.pattern.front() != '/' || route.name.empty() ||
        !route.handler) {
        throw std::invalid_argument("route requires an absolute pattern, name, and handler");
    }
    std::scoped_lock lock(update_mutex_);
    auto current = routes_.load(std::memory_order_acquire);
    auto updated = std::make_shared<Routes>(*current);
    updated->push_back(std::move(route));
    routes_.store(std::const_pointer_cast<const Routes>(updated), std::memory_order_release);
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
        try {
            response = co_await iterator->handler(context, std::move(request));
        } catch (...) {
            response = makeError(500, "Internal Server Error", "internal server error\n");
        }
    }
    response.headers.set("X-Request-Id", context.request_id);
    co_return response;
}

std::string Router::nextRequestId() {
    return std::to_string(next_request_id_.fetch_add(1, std::memory_order_relaxed));
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

}  // namespace pulsegate::http
