#include "pulsegate/http/response_cache.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

namespace pulsegate::http {
namespace {

HttpResponse response(std::string body) {
    HttpResponse value;
    value.body = std::move(body);
    return value;
}

ResponseCacheConfig config(std::size_t max_bytes = 64) {
    return {.ttl = std::chrono::milliseconds(10),
            .max_entry_bytes = std::min<std::size_t>(32, max_bytes),
            .max_bytes = max_bytes,
            .shard_count = 1,
            .vary_headers = {}};
}

TEST(ResponseCacheTest, HitPromotesAnEntryAndEvictsLeastRecentlyUsed) {
    ResponseCache cache(config(8));
    const auto start = CacheClock::time_point{};
    ASSERT_TRUE(cache.put("a", response("a"), start));
    ASSERT_TRUE(cache.put("b", response("b"), start));
    EXPECT_EQ(cache.get("a", start)->body, "a");
    ASSERT_TRUE(cache.put("c", response("c"), start));

    EXPECT_TRUE(cache.get("a", start));
    EXPECT_FALSE(cache.get("b", start));
    EXPECT_TRUE(cache.get("c", start));
    EXPECT_EQ(cache.stats().evictions, 1U);
}

TEST(ResponseCacheTest, ExpiresAndReplacesEntriesDeterministically) {
    ResponseCache cache(config());
    const auto start = CacheClock::time_point{};
    ASSERT_TRUE(cache.put("same", response("old"), start));
    ASSERT_TRUE(cache.put("same", response("new"), start));
    EXPECT_EQ(cache.get("same", start)->body, "new");

    EXPECT_FALSE(cache.get("same", start + std::chrono::milliseconds(10)));
    const auto stats = cache.stats();
    EXPECT_EQ(stats.expired, 1U);
    EXPECT_EQ(stats.misses, 1U);
}

TEST(ResponseCacheTest, RejectsEntriesThatExceedTheirByteBudget) {
    ResponseCache cache(config(8));
    EXPECT_FALSE(cache.put("large", response("body"), CacheClock::time_point{}));
    EXPECT_FALSE(cache.get("large", CacheClock::time_point{}));
}

TEST(ResponseCacheTest, RejectsInvalidCapacityAndTtlConfiguration) {
    EXPECT_THROW(ResponseCache({.ttl = std::chrono::milliseconds::zero(),
                                .max_entry_bytes = 1,
                                .max_bytes = 1,
                                .shard_count = 1,
                                .vary_headers = {}}),
                 std::invalid_argument);
    EXPECT_THROW(ResponseCache({.ttl = std::chrono::seconds(1),
                                .max_entry_bytes = 5,
                                .max_bytes = 8,
                                .shard_count = 2,
                                .vary_headers = {}}),
                 std::invalid_argument);
}

TEST(ResponseCacheTest, SupportsConcurrentGetsAndPuts) {
    ResponseCache cache({.ttl = std::chrono::seconds(1),
                         .max_entry_bytes = 128,
                         .max_bytes = 16 * 1024,
                         .shard_count = 8,
                         .vary_headers = {}});
    std::atomic_bool failed{false};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 16; ++worker) {
        workers.emplace_back([&cache, &failed, worker] {
            for (int iteration = 0; iteration < 500; ++iteration) {
                const auto key = "key-" + std::to_string((worker + iteration) % 32);
                if (!cache.put(key, response("ok"))) failed.store(true, std::memory_order_relaxed);
                if (const auto cached = cache.get(key); !cached || cached->body != "ok") {
                    failed.store(true, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& worker : workers) worker.join();
    EXPECT_FALSE(failed.load(std::memory_order_relaxed));
}

TEST(ResponseCacheTest, RejectsSensitiveRequestsAndResponsesAndBuildsStableKeys) {
    HttpRequest request{.method = HttpMethod::Get,
                        .target = "/orders?limit=10",
                        .version_major = 1,
                        .version_minor = 1,
                        .headers = {},
                        .body = {}};
    request.headers.add("Host", "EXAMPLE.test");
    request.headers.add("Accept-Language", "zh-CN");
    auto configured = config();
    configured.vary_headers = {"Accept-Language"};
    const auto key = ResponseCache::makeKey(request, configured);
    request.headers.set("Host", "example.test");
    EXPECT_EQ(ResponseCache::makeKey(request, configured), key);

    request.headers.add("Authorization", "Bearer secret");
    EXPECT_FALSE(ResponseCache::cacheableRequest(request));
    request.headers.erase("authorization");
    request.headers.add("Cookie", "session=secret");
    EXPECT_FALSE(ResponseCache::cacheableRequest(request));

    auto sensitive = response("ok");
    sensitive.headers.add("Set-Cookie", "session=secret");
    EXPECT_FALSE(ResponseCache::cacheableResponse(sensitive, configured));
    sensitive.headers.erase("set-cookie");
    sensitive.headers.add("Cache-Control", "private");
    EXPECT_FALSE(ResponseCache::cacheableResponse(sensitive, configured));
    sensitive.headers.erase("cache-control");
    sensitive.headers.add("Vary", "Accept-Language");
    EXPECT_TRUE(ResponseCache::cacheableResponse(sensitive, configured));
    sensitive.headers.set("Vary", "Accept-Encoding");
    EXPECT_FALSE(ResponseCache::cacheableResponse(sensitive, configured));
    sensitive.headers.erase("vary");
    sensitive.body.assign(configured.max_entry_bytes, 'x');
    EXPECT_FALSE(ResponseCache::cacheableResponse(sensitive, configured));
}

}  // namespace
}  // namespace pulsegate::http
