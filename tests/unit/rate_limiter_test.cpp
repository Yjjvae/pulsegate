#include "pulsegate/http/rate_limiter.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace pulsegate::http {
namespace {

TEST(TokenBucketTest, StartsFullAndRefillsUsingTheProvidedClock) {
    const auto start = RateLimitClock::time_point{};
    TokenBucket bucket(2.0, 3.0, start);
    EXPECT_TRUE(bucket.allow(1.0, start));
    EXPECT_TRUE(bucket.allow(1.0, start));
    EXPECT_TRUE(bucket.allow(1.0, start));
    EXPECT_FALSE(bucket.allow(1.0, start));

    const auto after_half_second = start + std::chrono::milliseconds(500);
    EXPECT_TRUE(bucket.allow(1.0, after_half_second));
    EXPECT_FALSE(bucket.allow(1.0, after_half_second));
    EXPECT_EQ(bucket.retryAfter(1.0, after_half_second), std::chrono::milliseconds(500));
}

TEST(TokenBucketTest, ConcurrentCallsDoNotOverspendTheBurst) {
    TokenBucket bucket(1.0, 10.0, RateLimitClock::time_point{});
    std::atomic_int allowed{0};
    std::vector<std::thread> workers;
    workers.reserve(32);
    for (int index = 0; index < 32; ++index) {
        workers.emplace_back([&] {
            if (bucket.allow(1.0, RateLimitClock::time_point{})) {
                allowed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    EXPECT_EQ(allowed.load(std::memory_order_relaxed), 10);
}

TEST(TokenBucketTest, RejectsInvalidValues) {
    EXPECT_THROW(TokenBucket(0.0, 1.0), std::invalid_argument);
    EXPECT_THROW(TokenBucket(1.0, 0.0), std::invalid_argument);

    TokenBucket bucket(1.0, 1.0, RateLimitClock::time_point{});
    EXPECT_THROW(static_cast<void>(bucket.allow(0.0, RateLimitClock::time_point{})),
                 std::invalid_argument);
}

TEST(RateLimiterTest, PerClientKeysExpireAndCapacityIsBounded) {
    const auto start = RateLimitClock::time_point{};
    RateLimiter limiter({.requests_per_second = 1.0,
                         .burst = 1.0,
                         .per_client = true,
                         .shard_count = 2,
                         .max_keys = 1,
                         .idle_ttl = std::chrono::milliseconds(10)},
                        start);
    EXPECT_TRUE(limiter.check("192.0.2.1", start).allowed);
    const auto full = limiter.check("192.0.2.2", start);
    EXPECT_FALSE(full.allowed);
    EXPECT_TRUE(full.key_capacity_exhausted);

    const auto later = start + std::chrono::milliseconds(10);
    EXPECT_TRUE(limiter.check("192.0.2.2", later).allowed);
}

TEST(RateLimiterTest, DifferentClientsReceiveIndependentBuckets) {
    const auto start = RateLimitClock::time_point{};
    RateLimiter limiter({.requests_per_second = 1.0,
                         .burst = 1.0,
                         .per_client = true,
                         .shard_count = 2,
                         .max_keys = 4,
                         .idle_ttl = std::chrono::seconds(1)},
                        start);
    EXPECT_TRUE(limiter.check("a", start).allowed);
    EXPECT_FALSE(limiter.check("a", start).allowed);
    EXPECT_TRUE(limiter.check("b", start).allowed);
}

TEST(RateLimiterTest, RecordsAggregateOutcomesWithoutClientLabels) {
    const auto start = RateLimitClock::time_point{};
    RateLimiter limiter({.requests_per_second = 1.0,
                         .burst = 1.0,
                         .per_client = true,
                         .shard_count = 2,
                         .max_keys = 1,
                         .idle_ttl = std::chrono::seconds(1)},
                        start);
    EXPECT_TRUE(limiter.check("client-a", start).allowed);
    EXPECT_FALSE(limiter.check("client-a", start).allowed);
    EXPECT_TRUE(limiter.check("client-b", start).key_capacity_exhausted);

    const auto stats = limiter.stats();
    EXPECT_EQ(stats.allowed, 1U);
    EXPECT_EQ(stats.rejected, 2U);
    EXPECT_EQ(stats.rejected_key_capacity, 1U);
}

TEST(RateLimiterTest, RejectsInvalidConfiguration) {
    EXPECT_THROW(RateLimiter({.requests_per_second = 0.0, .burst = 1.0}), std::invalid_argument);
    EXPECT_THROW(RateLimiter({.requests_per_second = 1.0, .burst = 1.0, .shard_count = 0}),
                 std::invalid_argument);
    EXPECT_THROW(RateLimiter({.requests_per_second = 1.0,
                              .burst = 1.0,
                              .idle_ttl = std::chrono::milliseconds::zero()}),
                 std::invalid_argument);
}

}  // namespace
}  // namespace pulsegate::http
