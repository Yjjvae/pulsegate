#include "pulsegate/http/circuit_breaker.h"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <thread>
#include <vector>

namespace pulsegate::http {
namespace {

using Clock = std::chrono::steady_clock;

TEST(CircuitBreakerTest, OpensAfterThresholdAndRejectsBeforeCooldown) {
    CircuitBreaker breaker(
        {.failure_threshold = 2, .cooldown = std::chrono::seconds(3), .half_open_max_probes = 1});
    const auto now = Clock::now();

    EXPECT_TRUE(breaker.allowRequest(now));
    breaker.recordFailure(now);
    EXPECT_EQ(breaker.state(), CircuitState::Closed);
    EXPECT_TRUE(breaker.allowRequest(now));
    breaker.recordFailure(now);

    EXPECT_EQ(breaker.state(), CircuitState::Open);
    EXPECT_FALSE(breaker.allowRequest(now + std::chrono::seconds(2)));
}

TEST(CircuitBreakerTest, SuccessfulHalfOpenProbeClosesAndResetsFailureCount) {
    CircuitBreaker breaker({.failure_threshold = 1,
                            .cooldown = std::chrono::milliseconds(10),
                            .half_open_max_probes = 1});
    const auto now = Clock::now();
    breaker.recordFailure(now);
    ASSERT_EQ(breaker.state(), CircuitState::Open);

    EXPECT_TRUE(breaker.allowRequest(now + std::chrono::milliseconds(10)));
    EXPECT_EQ(breaker.state(), CircuitState::HalfOpen);
    breaker.recordSuccess();

    const auto state = breaker.snapshot();
    EXPECT_EQ(state.state, CircuitState::Closed);
    EXPECT_EQ(state.consecutive_failures, 0U);
    EXPECT_EQ(state.probes_in_flight, 0U);
    EXPECT_TRUE(breaker.allowRequest(now + std::chrono::milliseconds(10)));
}

TEST(CircuitBreakerTest, FailedHalfOpenProbeReopensForAnotherCooldown) {
    CircuitBreaker breaker(
        {.failure_threshold = 1, .cooldown = std::chrono::seconds(1), .half_open_max_probes = 1});
    const auto now = Clock::now();
    breaker.recordFailure(now);
    ASSERT_TRUE(breaker.allowRequest(now + std::chrono::seconds(1)));
    breaker.recordFailure(now + std::chrono::seconds(1));

    EXPECT_EQ(breaker.state(), CircuitState::Open);
    EXPECT_FALSE(
        breaker.allowRequest(now + std::chrono::seconds(1) + std::chrono::milliseconds(999)));
    EXPECT_TRUE(breaker.allowRequest(now + std::chrono::seconds(2)));
}

TEST(CircuitBreakerTest, HalfOpenProbeBudgetIsThreadSafe) {
    CircuitBreaker breaker({.failure_threshold = 1,
                            .cooldown = std::chrono::milliseconds(1),
                            .half_open_max_probes = 1});
    const auto now = Clock::now();
    breaker.recordFailure(now);
    const auto probe_time = now + std::chrono::milliseconds(1);

    constexpr std::size_t callers = 16;
    std::barrier ready(callers);
    std::atomic_size_t allowed{0};
    std::vector<std::jthread> threads;
    threads.reserve(callers);
    for (std::size_t index = 0; index < callers; ++index) {
        threads.emplace_back([&] {
            ready.arrive_and_wait();
            if (breaker.allowRequest(probe_time)) ++allowed;
        });
    }
    for (auto& thread : threads) thread.join();

    EXPECT_EQ(allowed, 1U);
    EXPECT_EQ(breaker.state(), CircuitState::HalfOpen);
}

TEST(CircuitBreakerTest, NeutralOutcomeReleasesOnlyTheProbeBudget) {
    CircuitBreaker breaker({.failure_threshold = 1,
                            .cooldown = std::chrono::milliseconds(1),
                            .half_open_max_probes = 1});
    const auto now = Clock::now();
    breaker.recordFailure(now);
    ASSERT_TRUE(breaker.allowRequest(now + std::chrono::milliseconds(1)));
    breaker.recordNeutral();

    EXPECT_EQ(breaker.state(), CircuitState::HalfOpen);
    EXPECT_TRUE(breaker.allowRequest(now + std::chrono::milliseconds(1)));
}

}  // namespace
}  // namespace pulsegate::http
