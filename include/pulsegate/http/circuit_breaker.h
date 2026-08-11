#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace pulsegate::http {

enum class CircuitState { Closed, Open, HalfOpen };

struct CircuitBreakerLimits {
    std::uint32_t failure_threshold{3};
    std::chrono::milliseconds cooldown{std::chrono::seconds(5)};
    // A small probe budget prevents a recovered (or still failing) upstream
    // from receiving a thundering herd when its cooldown expires.
    std::size_t half_open_max_probes{1};
};

struct CircuitBreakerSnapshot {
    CircuitState state{CircuitState::Closed};
    std::uint32_t consecutive_failures{0};
    std::size_t probes_in_flight{0};
    std::chrono::steady_clock::time_point retry_at{};
};

// Thread-safe endpoint-local circuit breaker. Call allowRequest before an
// upstream acquire, then record exactly one upstream outcome for each allowed
// request. Client cancellation and local admission rejection are not outcomes.
class CircuitBreaker {
   public:
    explicit CircuitBreaker(CircuitBreakerLimits limits = {});

    [[nodiscard]] bool allowRequest(
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    void recordSuccess();
    void recordFailure(
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    // Releases a HalfOpen probe when no upstream outcome exists (for example
    // pool admission rejection or downstream cancellation before dispatch).
    void recordNeutral() noexcept;
    [[nodiscard]] CircuitBreakerSnapshot snapshot() const;
    [[nodiscard]] CircuitState state() const;

   private:
    void open(std::chrono::steady_clock::time_point now) noexcept;

    CircuitBreakerLimits limits_;
    mutable std::mutex mutex_;
    CircuitState state_{CircuitState::Closed};
    std::uint32_t consecutive_failures_{0};
    std::size_t probes_in_flight_{0};
    std::chrono::steady_clock::time_point retry_at_{};
};

}  // namespace pulsegate::http
