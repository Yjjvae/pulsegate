#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pulsegate::http {

using RateLimitClock = std::chrono::steady_clock;

class TokenBucket {
   public:
    TokenBucket(double rate_per_second, double burst,
                RateLimitClock::time_point initial_time = RateLimitClock::now());

    [[nodiscard]] bool allow(double tokens, RateLimitClock::time_point now);
    [[nodiscard]] std::chrono::milliseconds retryAfter(double tokens,
                                                       RateLimitClock::time_point now) const;

   private:
    void refill(RateLimitClock::time_point now) const;

    double rate_per_second_;
    double capacity_;
    mutable double available_tokens_;
    mutable RateLimitClock::time_point last_refill_;
    mutable std::mutex mutex_;
};

struct RateLimitConfig {
    double requests_per_second{0.0};
    double burst{0.0};
    bool per_client{false};
    std::size_t shard_count{16};
    std::size_t max_keys{10'000};
    std::chrono::milliseconds idle_ttl{std::chrono::minutes(10)};
};

struct RateLimitDecision {
    bool allowed{true};
    std::chrono::milliseconds retry_after{};
    bool key_capacity_exhausted{false};
};

struct RateLimitStats {
    std::uint64_t allowed{0};
    std::uint64_t rejected{0};
    std::uint64_t rejected_key_capacity{0};
};

class RateLimiter {
   public:
    explicit RateLimiter(RateLimitConfig config,
                         RateLimitClock::time_point initial_time = RateLimitClock::now());

    [[nodiscard]] RateLimitDecision check(std::string_view client_key,
                                          RateLimitClock::time_point now = RateLimitClock::now());
    [[nodiscard]] const RateLimitConfig& config() const noexcept;
    [[nodiscard]] RateLimitStats stats() const noexcept;

   private:
    struct ClientBucket {
        explicit ClientBucket(double rate, double burst, RateLimitClock::time_point now)
            : bucket(rate, burst, now), last_seen(now) {}
        TokenBucket bucket;
        RateLimitClock::time_point last_seen;
    };
    struct Shard {
        std::mutex mutex;
        std::unordered_map<std::string, std::unique_ptr<ClientBucket>> buckets;
    };

    [[nodiscard]] RateLimitDecision checkClient(std::string_view client_key,
                                                RateLimitClock::time_point now);
    void record(const RateLimitDecision& decision) noexcept;
    void pruneExpired(Shard& shard, RateLimitClock::time_point now);

    RateLimitConfig config_;
    std::optional<TokenBucket> global_bucket_;
    std::vector<std::unique_ptr<Shard>> shards_;
    std::atomic_size_t key_count_{0};
    std::atomic_uint64_t allowed_{0};
    std::atomic_uint64_t rejected_{0};
    std::atomic_uint64_t rejected_key_capacity_{0};
};

}  // namespace pulsegate::http
