#include "pulsegate/http/config.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <boost/asio/post.hpp>
#include <exception>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace pulsegate::http {
namespace {

template <typename T>
void readScalar(const YAML::Node& object, std::string_view key, std::string path, T& output,
                std::vector<ConfigError>& errors) {
    const auto node = object[std::string(key)];
    if (!node) return;
    try {
        output = node.as<T>();
    } catch (const YAML::Exception& error) {
        errors.push_back({std::move(path), error.msg});
    }
}

std::chrono::milliseconds milliseconds(std::uint64_t value) {
    constexpr auto maximum =
        static_cast<std::uint64_t>(std::numeric_limits<std::chrono::milliseconds::rep>::max());
    if (value > maximum) return std::chrono::milliseconds::max();
    return std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(value));
}

void readMilliseconds(const YAML::Node& object, std::string_view key, std::string path,
                      std::chrono::milliseconds& output, std::vector<ConfigError>& errors) {
    std::uint64_t value = 0;
    const auto before = errors.size();
    readScalar(object, key, std::move(path), value, errors);
    if (errors.size() == before && object[std::string(key)]) output = milliseconds(value);
}

UpstreamEndpoint endpointFromText(std::string_view value) {
    std::string_view host;
    std::string_view service;
    if (value.starts_with('[')) {
        const auto closing = value.find(']');
        if (closing == std::string_view::npos || closing + 1 >= value.size() ||
            value[closing + 1] != ':') {
            throw std::invalid_argument("IPv6 endpoint must look like [::1]:8080");
        }
        host = value.substr(1, closing - 1);
        service = value.substr(closing + 2);
    } else {
        const auto separator = value.rfind(':');
        if (separator == std::string_view::npos) {
            throw std::invalid_argument("endpoint must look like HOST:PORT");
        }
        host = value.substr(0, separator);
        service = value.substr(separator + 1);
    }
    if (host.empty() || service.empty()) throw std::invalid_argument("endpoint is incomplete");
    return {.host = std::string(host), .service = std::string(service)};
}

std::string joinErrors(const std::vector<ConfigError>& errors) {
    std::ostringstream output;
    for (const auto& error : errors) output << error.format() << '\n';
    return output.str();
}

}  // namespace

std::string ConfigError::format() const {
    return path + ": " + message;
}

ConfigValidationError::ConfigValidationError(std::vector<ConfigError> errors)
    : std::runtime_error(joinErrors(errors)), errors_(std::move(errors)) {}

const std::vector<ConfigError>& ConfigValidationError::errors() const noexcept {
    return errors_;
}

Config ConfigLoader::loadFromFile(const std::filesystem::path& path) const {
    Config config;
    std::vector<ConfigError> errors;
    YAML::Node root;
    try {
        root = YAML::LoadFile(path.string());
    } catch (const YAML::Exception& error) {
        throw ConfigValidationError({{"config", error.msg}});
    }
    if (!root || !root.IsMap()) throw ConfigValidationError({{"config", "must be a mapping"}});

    if (const auto server = root["server"]) {
        if (!server.IsMap()) {
            errors.push_back({"server", "must be a mapping"});
        } else {
            readScalar(server, "listen_host", "server.listen_host", config.server.listen_host,
                       errors);
            readScalar(server, "listen_port", "server.listen_port", config.server.listen_port,
                       errors);
            readScalar(server, "io_threads", "server.io_threads", config.server.io_threads, errors);
            readMilliseconds(server, "idle_timeout_ms", "server.idle_timeout_ms",
                             config.server.idle_timeout, errors);
            readMilliseconds(server, "header_timeout_ms", "server.header_timeout_ms",
                             config.server.header_timeout, errors);
            readMilliseconds(server, "body_timeout_ms", "server.body_timeout_ms",
                             config.server.body_timeout, errors);
            readMilliseconds(server, "graceful_shutdown_ms", "server.graceful_shutdown_ms",
                             config.server.graceful_shutdown, errors);
            readScalar(server, "max_connections", "server.max_connections",
                       config.server.max_connections, errors);
            readScalar(server, "max_header_bytes", "server.max_header_bytes",
                       config.server.max_header_bytes, errors);
            readScalar(server, "max_body_bytes", "server.max_body_bytes",
                       config.server.max_body_bytes, errors);
            readScalar(server, "output_high_water_bytes", "server.output_high_water_bytes",
                       config.server.output_high_water_bytes, errors);
            readScalar(server, "output_low_water_bytes", "server.output_low_water_bytes",
                       config.server.output_low_water_bytes, errors);
        }
    }

    if (const auto upstreams = root["upstreams"]) {
        if (!upstreams.IsSequence()) {
            errors.push_back({"upstreams", "must be a sequence"});
        } else {
            for (std::size_t index = 0; index < upstreams.size(); ++index) {
                const auto upstream = upstreams[index];
                const auto prefix = "upstreams[" + std::to_string(index) + "]";
                if (!upstream.IsMap()) {
                    errors.push_back({prefix, "must be a mapping"});
                    continue;
                }
                UpstreamConfig parsed;
                readScalar(upstream, "name", prefix + ".name", parsed.name, errors);
                readMilliseconds(upstream, "connect_timeout_ms", prefix + ".connect_timeout_ms",
                                 parsed.proxy_limits.connect_timeout, errors);
                readMilliseconds(upstream, "response_timeout_ms", prefix + ".response_timeout_ms",
                                 parsed.proxy_limits.response_timeout, errors);
                readScalar(upstream, "max_connections_per_endpoint",
                           prefix + ".max_connections_per_endpoint",
                           parsed.proxy_limits.pool_limits.max_connections, errors);
                readScalar(upstream, "pool_shards", prefix + ".pool_shards", parsed.pool_shards,
                           errors);
                parsed.proxy_limits.pool_limits.max_idle_connections =
                    std::min(parsed.proxy_limits.pool_limits.max_idle_connections,
                             parsed.proxy_limits.pool_limits.max_connections);
                const auto endpoints = upstream["endpoints"];
                if (!endpoints || !endpoints.IsSequence()) {
                    errors.push_back({prefix + ".endpoints", "must be a sequence"});
                } else {
                    for (std::size_t endpoint_index = 0; endpoint_index < endpoints.size();
                         ++endpoint_index) {
                        const auto endpoint_path =
                            prefix + ".endpoints[" + std::to_string(endpoint_index) + "]";
                        try {
                            parsed.endpoints.push_back(
                                endpointFromText(endpoints[endpoint_index].as<std::string>()));
                        } catch (const std::exception& error) {
                            errors.push_back({endpoint_path, error.what()});
                        }
                    }
                }
                config.upstreams.push_back(std::move(parsed));
            }
        }
    }

    if (const auto routes = root["routes"]) {
        if (!routes.IsSequence()) {
            errors.push_back({"routes", "must be a sequence"});
        } else {
            for (std::size_t index = 0; index < routes.size(); ++index) {
                const auto route = routes[index];
                const auto prefix = "routes[" + std::to_string(index) + "]";
                if (!route.IsMap()) {
                    errors.push_back({prefix, "must be a mapping"});
                    continue;
                }
                RouteConfig parsed;
                readScalar(route, "path_prefix", prefix + ".path_prefix", parsed.path_prefix,
                           errors);
                readScalar(route, "upstream", prefix + ".upstream", parsed.upstream, errors);
                if (const auto limit = route["rate_limit"]) {
                    if (!limit.IsMap()) {
                        errors.push_back({prefix + ".rate_limit", "must be a mapping"});
                    } else {
                        RateLimitConfig rate;
                        readScalar(limit, "requests_per_second",
                                   prefix + ".rate_limit.requests_per_second",
                                   rate.requests_per_second, errors);
                        readScalar(limit, "burst", prefix + ".rate_limit.burst", rate.burst,
                                   errors);
                        parsed.rate_limit = rate;
                    }
                }
                if (const auto cache = route["cache"]) {
                    if (!cache.IsMap()) {
                        errors.push_back({prefix + ".cache", "must be a mapping"});
                    } else {
                        bool enabled = false;
                        readScalar(cache, "enabled", prefix + ".cache.enabled", enabled, errors);
                        if (enabled) {
                            ResponseCacheConfig value;
                            readMilliseconds(cache, "ttl_ms", prefix + ".cache.ttl_ms", value.ttl,
                                             errors);
                            readScalar(cache, "max_object_bytes",
                                       prefix + ".cache.max_object_bytes", value.max_entry_bytes,
                                       errors);
                            readScalar(cache, "max_bytes", prefix + ".cache.max_bytes",
                                       value.max_bytes, errors);
                            readScalar(cache, "shards", prefix + ".cache.shards", value.shard_count,
                                       errors);
                            parsed.cache = std::move(value);
                        }
                    }
                }
                config.routes.push_back(std::move(parsed));
            }
        }
    }

    if (const auto logging = root["logging"]) {
        if (!logging.IsMap()) {
            errors.push_back({"logging", "must be a mapping"});
        } else {
            readScalar(logging, "level", "logging.level", config.logging.level, errors);
            readScalar(logging, "format", "logging.format", config.logging.format, errors);
        }
    }

    auto validation = validate(config);
    errors.insert(errors.end(), std::make_move_iterator(validation.begin()),
                  std::make_move_iterator(validation.end()));
    if (!errors.empty()) throw ConfigValidationError(std::move(errors));
    return config;
}

std::vector<ConfigError> ConfigLoader::validate(const Config& config) const {
    std::vector<ConfigError> errors;
    const auto positive = [&errors](std::string path, auto value) {
        if (value == 0) errors.push_back({std::move(path), "must be positive"});
    };
    if (config.server.listen_host.empty())
        errors.push_back({"server.listen_host", "must not be empty"});
    if (config.server.listen_port == 0 || config.server.listen_port > 65535) {
        errors.push_back({"server.listen_port", "must be between 1 and 65535"});
    }
    positive("server.io_threads", config.server.io_threads);
    positive("server.max_connections", config.server.max_connections);
    positive("server.max_header_bytes", config.server.max_header_bytes);
    positive("server.max_body_bytes", config.server.max_body_bytes);
    if (config.server.max_body_bytes > 64U * 1024U * 1024U) {
        errors.push_back({"server.max_body_bytes", "must not exceed 67108864"});
    }
    if (config.server.idle_timeout <= std::chrono::milliseconds::zero() ||
        config.server.header_timeout <= std::chrono::milliseconds::zero() ||
        config.server.body_timeout <= std::chrono::milliseconds::zero() ||
        config.server.graceful_shutdown <= std::chrono::milliseconds::zero()) {
        errors.push_back({"server", "timeout values must be positive"});
    }
    if (config.server.output_low_water_bytes >= config.server.output_high_water_bytes) {
        errors.push_back({"server.output_low_water_bytes", "must be lower than high water"});
    }

    std::set<std::string> names;
    for (std::size_t index = 0; index < config.upstreams.size(); ++index) {
        const auto& upstream = config.upstreams[index];
        const auto prefix = "upstreams[" + std::to_string(index) + "]";
        if (upstream.name.empty()) errors.push_back({prefix + ".name", "must not be empty"});
        if (!names.insert(upstream.name).second) {
            errors.push_back({prefix + ".name", "must be unique"});
        }
        if (upstream.endpoints.empty())
            errors.push_back({prefix + ".endpoints", "must not be empty"});
        positive(prefix + ".pool_shards", upstream.pool_shards);
        std::set<std::string> endpoints;
        for (std::size_t endpoint_index = 0; endpoint_index < upstream.endpoints.size();
             ++endpoint_index) {
            const auto& endpoint = upstream.endpoints[endpoint_index];
            const auto endpoint_path =
                prefix + ".endpoints[" + std::to_string(endpoint_index) + "]";
            if (endpoint.host.empty() || endpoint.service.empty()) {
                errors.push_back({endpoint_path, "host and port must not be empty"});
            }
            if (!endpoints.insert(endpoint.host + ":" + endpoint.service).second) {
                errors.push_back({endpoint_path, "must not duplicate an endpoint"});
            }
        }
    }
    for (std::size_t index = 0; index < config.routes.size(); ++index) {
        const auto& route = config.routes[index];
        const auto prefix = "routes[" + std::to_string(index) + "]";
        if (route.path_prefix.empty() || route.path_prefix.front() != '/') {
            errors.push_back({prefix + ".path_prefix", "must be an absolute path prefix"});
        }
        if (!names.contains(route.upstream)) {
            errors.push_back({prefix + ".upstream", "unknown upstream \"" + route.upstream + "\""});
        }
        if (route.rate_limit &&
            (route.rate_limit->requests_per_second <= 0.0 || route.rate_limit->burst <= 0.0)) {
            errors.push_back(
                {prefix + ".rate_limit", "requests_per_second and burst must be positive"});
        }
        if (route.cache) {
            if (route.cache->ttl <= std::chrono::milliseconds::zero() ||
                route.cache->max_entry_bytes == 0 || route.cache->max_bytes == 0 ||
                route.cache->shard_count == 0) {
                errors.push_back({prefix + ".cache", "ttl, sizes, and shards must be positive"});
            } else if (route.cache->max_entry_bytes >
                       route.cache->max_bytes / route.cache->shard_count) {
                errors.push_back(
                    {prefix + ".cache.max_object_bytes", "must fit within one cache shard"});
            }
        }
    }
    return errors;
}

ConfigManager::ConfigManager(net::asio::any_io_executor executor, std::filesystem::path path,
                             std::shared_ptr<const ConfigSnapshot> initial)
    : strand_(net::asio::make_strand(std::move(executor))), path_(std::move(path)) {
    if (!initial) throw std::invalid_argument("configuration snapshot must not be null");
    snapshot_.store(std::move(initial), std::memory_order_release);
}

ConfigManager::~ConfigManager() {
    worker_.join();
}

void ConfigManager::requestReload(ReloadHandler handler) {
    const auto self = shared_from_this();
    net::asio::post(strand_, [self, handler = std::move(handler)]() mutable {
        if (self->reload_in_progress_) {
            if (handler) {
                handler({.published = false,
                         .errors = {{"config", "reload already in progress"}},
                         .restart_required = {}});
            }
            return;
        }
        self->reload_in_progress_ = true;
        net::asio::post(self->worker_, [self, handler = std::move(handler)]() mutable {
            ConfigReloadResult result;
            std::shared_ptr<const ConfigSnapshot> next;
            try {
                next = std::make_shared<const ConfigSnapshot>(
                    ConfigSnapshot{.value = self->loader_.loadFromFile(self->path_),
                                   .source_path = self->path_,
                                   .loaded_at = std::chrono::system_clock::now()});
            } catch (const ConfigValidationError& error) {
                result.errors = error.errors();
            } catch (const std::exception& error) {
                result.errors.push_back({"config", error.what()});
            }
            net::asio::post(self->strand_, [self, handler = std::move(handler),
                                            result = std::move(result),
                                            next = std::move(next)]() mutable {
                self->reload_in_progress_ = false;
                if (next) {
                    result.restart_required = staticChanges(self->snapshot()->value, next->value);
                    if (result.restart_required.empty()) {
                        self->snapshot_.store(std::move(next), std::memory_order_release);
                        result.published = true;
                    }
                }
                if (handler) handler(std::move(result));
            });
        });
    });
}

std::shared_ptr<const ConfigSnapshot> ConfigManager::snapshot() const noexcept {
    return snapshot_.load(std::memory_order_acquire);
}

std::vector<std::string> ConfigManager::staticChanges(const Config& before, const Config& after) {
    std::vector<std::string> changes;
    if (before.server.listen_host != after.server.listen_host)
        changes.push_back("server.listen_host");
    if (before.server.listen_port != after.server.listen_port)
        changes.push_back("server.listen_port");
    if (before.server.io_threads != after.server.io_threads) changes.push_back("server.io_threads");
    if (before.server.max_connections != after.server.max_connections) {
        changes.push_back("server.max_connections");
    }
    if (before.server.idle_timeout != after.server.idle_timeout ||
        before.server.header_timeout != after.server.header_timeout ||
        before.server.body_timeout != after.server.body_timeout ||
        before.server.graceful_shutdown != after.server.graceful_shutdown ||
        before.server.max_header_bytes != after.server.max_header_bytes ||
        before.server.max_body_bytes != after.server.max_body_bytes ||
        before.server.output_high_water_bytes != after.server.output_high_water_bytes ||
        before.server.output_low_water_bytes != after.server.output_low_water_bytes) {
        changes.push_back("server.limits");
    }
    const auto equal_rate = [](const std::optional<RateLimitConfig>& left,
                               const std::optional<RateLimitConfig>& right) {
        if (left.has_value() != right.has_value()) return false;
        if (!left) return true;
        return left->requests_per_second == right->requests_per_second &&
               left->burst == right->burst && left->per_client == right->per_client &&
               left->shard_count == right->shard_count && left->max_keys == right->max_keys &&
               left->idle_ttl == right->idle_ttl;
    };
    const auto equal_cache = [](const std::optional<ResponseCacheConfig>& left,
                                const std::optional<ResponseCacheConfig>& right) {
        if (left.has_value() != right.has_value()) return false;
        if (!left) return true;
        return left->ttl == right->ttl && left->max_entry_bytes == right->max_entry_bytes &&
               left->max_bytes == right->max_bytes && left->shard_count == right->shard_count &&
               left->vary_headers == right->vary_headers;
    };
    if (before.routes.size() != after.routes.size()) {
        changes.push_back("routes");
    } else {
        for (std::size_t index = 0; index < before.routes.size(); ++index) {
            const auto& left = before.routes[index];
            const auto& right = after.routes[index];
            if (left.path_prefix != right.path_prefix || left.upstream != right.upstream ||
                !equal_rate(left.rate_limit, right.rate_limit) ||
                !equal_cache(left.cache, right.cache)) {
                changes.push_back("routes[" + std::to_string(index) + "]");
            }
        }
    }
    const auto equal_upstream = [](const UpstreamConfig& left, const UpstreamConfig& right) {
        if (left.name != right.name || left.pool_shards != right.pool_shards ||
            left.endpoints.size() != right.endpoints.size()) {
            return false;
        }
        if (left.proxy_limits.connect_timeout != right.proxy_limits.connect_timeout ||
            left.proxy_limits.response_timeout != right.proxy_limits.response_timeout ||
            left.proxy_limits.pool_limits.max_connections !=
                right.proxy_limits.pool_limits.max_connections) {
            return false;
        }
        return std::equal(left.endpoints.begin(), left.endpoints.end(), right.endpoints.begin(),
                          [](const UpstreamEndpoint& first, const UpstreamEndpoint& second) {
                              return first.host == second.host && first.service == second.service;
                          });
    };
    if (before.upstreams.size() != after.upstreams.size()) {
        changes.push_back("upstreams");
    } else {
        for (std::size_t index = 0; index < before.upstreams.size(); ++index) {
            if (!equal_upstream(before.upstreams[index], after.upstreams[index])) {
                changes.push_back("upstreams[" + std::to_string(index) + "]");
            }
        }
    }
    return changes;
}

}  // namespace pulsegate::http
