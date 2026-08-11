#include "pulsegate/http/response_cache.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>

namespace pulsegate::http {
namespace {

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

std::string_view trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    return value;
}

bool containsDirective(std::string_view value, std::string_view directive) {
    while (!value.empty()) {
        const auto comma = value.find(',');
        auto item = trim(value.substr(0, comma));
        item = trim(item.substr(0, item.find('=')));
        if (lower(item) == directive) return true;
        if (comma == std::string_view::npos) return false;
        value.remove_prefix(comma + 1);
    }
    return false;
}

bool hasSensitiveResponseHeaders(const Headers& headers) {
    if (headers.contains("set-cookie")) return true;
    for (const auto value : headers.values("cache-control")) {
        if (containsDirective(value, "no-store") || containsDirective(value, "private"))
            return true;
    }
    return false;
}

bool varyIsCovered(const Headers& headers, const ResponseCacheConfig& config) {
    for (const auto value : headers.values("vary")) {
        std::string_view pending = value;
        while (!pending.empty()) {
            const auto comma = pending.find(',');
            const auto name = normalizeHeaderName(trim(pending.substr(0, comma)));
            const auto configured = std::find_if(
                config.vary_headers.begin(), config.vary_headers.end(),
                [&name](const auto& header) { return normalizeHeaderName(header) == name; });
            if (!isValidHeaderName(name) || name == "*" ||
                configured == config.vary_headers.end()) {
                return false;
            }
            if (comma == std::string_view::npos) break;
            pending.remove_prefix(comma + 1);
        }
    }
    return true;
}

void appendField(std::string& output, std::string_view value) {
    output.append(std::to_string(value.size()));
    output.push_back(':');
    output.append(value);
    output.push_back('|');
}

}  // namespace

ResponseCache::ResponseCache(ResponseCacheConfig config) : config_(std::move(config)) {
    if (config_.ttl <= std::chrono::milliseconds::zero() || config_.max_entry_bytes == 0 ||
        config_.max_entry_bytes > config_.max_bytes || config_.max_bytes == 0 ||
        config_.shard_count == 0 || config_.shard_count > config_.max_bytes ||
        config_.max_entry_bytes > config_.max_bytes / config_.shard_count) {
        throw std::invalid_argument("response cache configuration is invalid");
    }
    for (auto& header : config_.vary_headers) {
        header = normalizeHeaderName(header);
        if (!isValidHeaderName(header)) {
            throw std::invalid_argument("cache vary header is invalid");
        }
    }
    std::sort(config_.vary_headers.begin(), config_.vary_headers.end());
    config_.vary_headers.erase(
        std::unique(config_.vary_headers.begin(), config_.vary_headers.end()),
        config_.vary_headers.end());

    const auto base = config_.max_bytes / config_.shard_count;
    const auto remainder = config_.max_bytes % config_.shard_count;
    shards_.reserve(config_.shard_count);
    for (std::size_t index = 0; index < config_.shard_count; ++index) {
        shards_.push_back(std::make_unique<Shard>(base + (index < remainder ? 1U : 0U)));
    }
}

std::optional<HttpResponse> ResponseCache::get(std::string_view key, CacheClock::time_point now) {
    auto& shard = shardFor(key);
    std::scoped_lock lock(shard.mutex);
    const auto found = shard.index.find(key);
    if (found == shard.index.end()) {
        misses_.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }
    const auto iterator = found->second;
    if (iterator->second.expires_at <= now) {
        erase(shard, iterator, false);
        expired_.fetch_add(1, std::memory_order_relaxed);
        misses_.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }
    shard.lru.splice(shard.lru.begin(), shard.lru, iterator);
    hits_.fetch_add(1, std::memory_order_relaxed);
    return shard.lru.front().second.response;
}

bool ResponseCache::put(std::string key, HttpResponse response, CacheClock::time_point now) {
    const auto entry_charge = charge(key, response);
    auto& shard = shardFor(key);
    std::scoped_lock lock(shard.mutex);
    if (entry_charge > shard.max_bytes) return false;

    if (const auto found = shard.index.find(key); found != shard.index.end()) {
        erase(shard, found->second, false);
    }
    while (!shard.lru.empty() && shard.current_bytes > shard.max_bytes - entry_charge) {
        erase(shard, std::prev(shard.lru.end()), true);
    }
    CacheEntry entry{
        .response = std::move(response), .expires_at = now + config_.ttl, .charge = entry_charge};
    shard.lru.emplace_front(std::move(key), std::move(entry));
    shard.index.emplace(shard.lru.front().first, shard.lru.begin());
    shard.current_bytes += entry_charge;
    stores_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

const ResponseCacheConfig& ResponseCache::config() const noexcept {
    return config_;
}

CacheStats ResponseCache::stats() const noexcept {
    return {.hits = hits_.load(std::memory_order_relaxed),
            .misses = misses_.load(std::memory_order_relaxed),
            .stores = stores_.load(std::memory_order_relaxed),
            .evictions = evictions_.load(std::memory_order_relaxed),
            .expired = expired_.load(std::memory_order_relaxed)};
}

bool ResponseCache::cacheableRequest(const HttpRequest& request) noexcept {
    return (request.method == HttpMethod::Get || request.method == HttpMethod::Head) &&
           !request.headers.contains("authorization") && !request.headers.contains("cookie");
}

bool ResponseCache::cacheableResponse(const HttpResponse& response,
                                      const ResponseCacheConfig& config) noexcept {
    return response.status_code == 200 && !response.already_written &&
           response.body.size() < config.max_entry_bytes &&
           !hasSensitiveResponseHeaders(response.headers) &&
           varyIsCovered(response.headers, config);
}

std::string ResponseCache::makeKey(const HttpRequest& request, const ResponseCacheConfig& config) {
    const std::string_view target(request.target);
    const auto question = target.find('?');
    auto path = target.substr(0, question);
    if (path.empty()) path = "/";
    const auto query =
        question == std::string_view::npos ? std::string_view{} : target.substr(question + 1);

    std::string key;
    key.reserve(target.size() + 64);
    appendField(key, "http");
    appendField(key, lower(request.headers.get("host").value_or("")));
    appendField(key, path);
    appendField(key, query);
    for (const auto& header : config.vary_headers) {
        appendField(key, header);
        const auto values = request.headers.values(header);
        for (const auto value : values) appendField(key, value);
        appendField(key, "");
    }
    return key;
}

ResponseCache::Shard& ResponseCache::shardFor(std::string_view key) noexcept {
    return *shards_[std::hash<std::string_view>{}(key) % shards_.size()];
}

std::size_t ResponseCache::charge(std::string_view key, const HttpResponse& response) {
    if (key.size() > std::numeric_limits<std::size_t>::max() - response.reason.size() ||
        response.body.size() >
            std::numeric_limits<std::size_t>::max() - key.size() - response.reason.size()) {
        return std::numeric_limits<std::size_t>::max();
    }
    std::size_t result = key.size() + response.reason.size() + response.body.size();
    for (const auto& header : response.headers.entries()) {
        if (header.name.size() > std::numeric_limits<std::size_t>::max() - result ||
            header.value.size() >
                std::numeric_limits<std::size_t>::max() - result - header.name.size()) {
            return std::numeric_limits<std::size_t>::max();
        }
        result += header.name.size() + header.value.size();
    }
    return result;
}

void ResponseCache::erase(Shard& shard, LruList::iterator iterator, bool evicted) noexcept {
    shard.current_bytes -= iterator->second.charge;
    shard.index.erase(iterator->first);
    shard.lru.erase(iterator);
    if (evicted) evictions_.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace pulsegate::http
