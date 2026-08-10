#include "pulsegate/http/rate_limiter.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

namespace pulsegate::http {
namespace {

bool validPositive(double value) {
    return std::isfinite(value) && value > 0.0;
}

}  // namespace

TokenBucket::TokenBucket(double rate_per_second, double burst,
                         RateLimitClock::time_point initial_time)
    : rate_per_second_(rate_per_second),
      capacity_(burst),
      available_tokens_(burst),
      last_refill_(initial_time) {
    if (!validPositive(rate_per_second_) || !validPositive(capacity_)) {
        throw std::invalid_argument("token bucket rate and burst must be positive finite values");
    }
}

bool TokenBucket::allow(double tokens, RateLimitClock::time_point now) {
    if (!validPositive(tokens))
        throw std::invalid_argument("requested token count must be positive");
    std::scoped_lock lock(mutex_);
    refill(now);
    if (tokens > available_tokens_) return false;
    available_tokens_ -= tokens;
    return true;
}

std::chrono::milliseconds TokenBucket::retryAfter(double tokens,
                                                  RateLimitClock::time_point now) const {
    if (!validPositive(tokens))
        throw std::invalid_argument("requested token count must be positive");
    std::scoped_lock lock(mutex_);
    refill(now);
    if (tokens <= available_tokens_) return {};
    const auto seconds = (tokens - available_tokens_) / rate_per_second_;
    return std::chrono::milliseconds(
        static_cast<std::chrono::milliseconds::rep>(std::ceil(seconds * 1000.0)));
}

void TokenBucket::refill(RateLimitClock::time_point now) const {
    if (now <= last_refill_) return;
    const auto elapsed = std::chrono::duration<double>(now - last_refill_).count();
    available_tokens_ = std::min(capacity_, available_tokens_ + elapsed * rate_per_second_);
    last_refill_ = now;
}

RateLimiter::RateLimiter(RateLimitConfig config, RateLimitClock::time_point initial_time)
    : config_(config) {
    if (!validPositive(config_.requests_per_second) || !validPositive(config_.burst) ||
        config_.shard_count == 0 || config_.max_keys == 0 ||
        config_.idle_ttl <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("rate limit configuration is invalid");
    }
    if (config_.per_client) {
        shards_.reserve(config_.shard_count);
        for (std::size_t index = 0; index < config_.shard_count; ++index) {
            shards_.push_back(std::make_unique<Shard>());
        }
    } else {
        global_bucket_.emplace(config_.requests_per_second, config_.burst, initial_time);
    }
}

RateLimitDecision RateLimiter::check(std::string_view client_key, RateLimitClock::time_point now) {
    RateLimitDecision decision;
    if (config_.per_client) {
        decision = checkClient(client_key, now);
    } else {
        const bool allowed = global_bucket_->allow(1.0, now);
        decision = {.allowed = allowed,
                    .retry_after = allowed ? std::chrono::milliseconds{}
                                           : global_bucket_->retryAfter(1.0, now)};
    }
    record(decision);
    return decision;
}

const RateLimitConfig& RateLimiter::config() const noexcept {
    return config_;
}

RateLimitStats RateLimiter::stats() const noexcept {
    return {.allowed = allowed_.load(std::memory_order_relaxed),
            .rejected = rejected_.load(std::memory_order_relaxed),
            .rejected_key_capacity = rejected_key_capacity_.load(std::memory_order_relaxed)};
}

RateLimitDecision RateLimiter::checkClient(std::string_view client_key,
                                           RateLimitClock::time_point now) {
    if (client_key.empty()) client_key = "unknown";
    const auto hash = std::hash<std::string_view>{}(client_key);
    auto& shard = *shards_[hash % shards_.size()];
    std::scoped_lock lock(shard.mutex);
    pruneExpired(shard, now);
    const std::string key(client_key);
    auto iterator = shard.buckets.find(key);
    if (iterator == shard.buckets.end()) {
        auto current_count = key_count_.load(std::memory_order_relaxed);
        while (current_count < config_.max_keys &&
               !key_count_.compare_exchange_weak(current_count, current_count + 1,
                                                 std::memory_order_relaxed)) {
        }
        if (current_count >= config_.max_keys) {
            return {.allowed = false,
                    .retry_after = std::chrono::milliseconds(1000),
                    .key_capacity_exhausted = true};
        }
        try {
            auto bucket =
                std::make_unique<ClientBucket>(config_.requests_per_second, config_.burst, now);
            iterator = shard.buckets.emplace(key, std::move(bucket)).first;
        } catch (...) {
            key_count_.fetch_sub(1, std::memory_order_relaxed);
            throw;
        }
    }
    auto& entry = *iterator->second;
    entry.last_seen = now;
    const bool allowed = entry.bucket.allow(1.0, now);
    return {
        .allowed = allowed,
        .retry_after = allowed ? std::chrono::milliseconds{} : entry.bucket.retryAfter(1.0, now)};
}

void RateLimiter::record(const RateLimitDecision& decision) noexcept {
    if (decision.allowed) {
        allowed_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    rejected_.fetch_add(1, std::memory_order_relaxed);
    if (decision.key_capacity_exhausted) {
        rejected_key_capacity_.fetch_add(1, std::memory_order_relaxed);
    }
}

void RateLimiter::pruneExpired(Shard& shard, RateLimitClock::time_point now) {
    for (auto iterator = shard.buckets.begin(); iterator != shard.buckets.end();) {
        if (now - iterator->second->last_seen >= config_.idle_ttl) {
            iterator = shard.buckets.erase(iterator);
            key_count_.fetch_sub(1, std::memory_order_relaxed);
        } else {
            ++iterator;
        }
    }
}

}  // namespace pulsegate::http
