#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "pulsegate/http/http_request.h"
#include "pulsegate/http/http_response.h"

namespace pulsegate::http {

using CacheClock = std::chrono::steady_clock;

struct ResponseCacheConfig {
    std::chrono::milliseconds ttl{std::chrono::seconds(30)};
    std::size_t max_entry_bytes{256 * 1024};
    std::size_t max_bytes{4 * 1024 * 1024};
    std::size_t shard_count{16};
    std::vector<std::string> vary_headers;
};

struct CacheStats {
    std::uint64_t hits{0};
    std::uint64_t misses{0};
    std::uint64_t stores{0};
    std::uint64_t evictions{0};
    std::uint64_t expired{0};
};

// A bounded, thread-safe response cache. Each shard owns a mutex-protected
// LRU list, so unrelated keys do not serialize on one global cache lock.
class ResponseCache {
   public:
    explicit ResponseCache(ResponseCacheConfig config);

    [[nodiscard]] std::optional<HttpResponse> get(std::string_view key,
                                                  CacheClock::time_point now = CacheClock::now());
    // Returns false when one entry cannot fit in its shard's byte budget.
    [[nodiscard]] bool put(std::string key, HttpResponse response,
                           CacheClock::time_point now = CacheClock::now());
    [[nodiscard]] const ResponseCacheConfig& config() const noexcept;
    [[nodiscard]] CacheStats stats() const noexcept;

    [[nodiscard]] static bool cacheableRequest(const HttpRequest& request) noexcept;
    [[nodiscard]] static bool cacheableResponse(const HttpResponse& response,
                                                const ResponseCacheConfig& config) noexcept;
    [[nodiscard]] static std::string makeKey(const HttpRequest& request,
                                             const ResponseCacheConfig& config);

   private:
    struct CacheEntry {
        HttpResponse response;
        CacheClock::time_point expires_at;
        std::size_t charge{0};
    };
    using LruList = std::list<std::pair<std::string, CacheEntry>>;
    struct TransparentHash {
        using is_transparent = void;
        [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
            return std::hash<std::string_view>{}(value);
        }
        [[nodiscard]] std::size_t operator()(const std::string& value) const noexcept {
            return operator()(std::string_view(value));
        }
    };
    struct TransparentEqual {
        using is_transparent = void;
        [[nodiscard]] bool operator()(std::string_view left,
                                      std::string_view right) const noexcept {
            return left == right;
        }
    };
    struct Shard {
        explicit Shard(std::size_t byte_limit) : max_bytes(byte_limit) {}
        std::mutex mutex;
        LruList lru;
        std::unordered_map<std::string, LruList::iterator, TransparentHash, TransparentEqual> index;
        std::size_t current_bytes{0};
        std::size_t max_bytes;
    };

    [[nodiscard]] Shard& shardFor(std::string_view key) noexcept;
    [[nodiscard]] static std::size_t charge(std::string_view key, const HttpResponse& response);
    void erase(Shard& shard, LruList::iterator iterator, bool evicted) noexcept;

    ResponseCacheConfig config_;
    std::vector<std::unique_ptr<Shard>> shards_;
    std::atomic_uint64_t hits_{0};
    std::atomic_uint64_t misses_{0};
    std::atomic_uint64_t stores_{0};
    std::atomic_uint64_t evictions_{0};
    std::atomic_uint64_t expired_{0};
};

}  // namespace pulsegate::http
