#include "pulsegate/http/circuit_breaker.h"

#include <stdexcept>

namespace pulsegate::http {

CircuitBreaker::CircuitBreaker(CircuitBreakerLimits limits) : limits_(limits) {
    if (limits_.failure_threshold == 0 || limits_.cooldown <= std::chrono::milliseconds::zero() ||
        limits_.half_open_max_probes == 0) {
        throw std::invalid_argument("circuit breaker limits must be positive");
    }
}

bool CircuitBreaker::allowRequest(std::chrono::steady_clock::time_point now) {
    std::scoped_lock lock(mutex_);
    if (state_ == CircuitState::Open) {
        if (now < retry_at_) return false;
        state_ = CircuitState::HalfOpen;
        probes_in_flight_ = 0;
    }
    if (state_ == CircuitState::HalfOpen) {
        if (probes_in_flight_ >= limits_.half_open_max_probes) return false;
        ++probes_in_flight_;
    }
    return true;
}

void CircuitBreaker::recordSuccess() {
    std::scoped_lock lock(mutex_);
    consecutive_failures_ = 0;
    probes_in_flight_ = 0;
    state_ = CircuitState::Closed;
    retry_at_ = {};
}

void CircuitBreaker::recordFailure(std::chrono::steady_clock::time_point now) {
    std::scoped_lock lock(mutex_);
    if (state_ == CircuitState::HalfOpen) {
        open(now);
        return;
    }
    if (state_ == CircuitState::Open) return;
    ++consecutive_failures_;
    if (consecutive_failures_ >= limits_.failure_threshold) open(now);
}

void CircuitBreaker::recordNeutral() noexcept {
    std::scoped_lock lock(mutex_);
    if (state_ == CircuitState::HalfOpen && probes_in_flight_ > 0) --probes_in_flight_;
}

CircuitBreakerSnapshot CircuitBreaker::snapshot() const {
    std::scoped_lock lock(mutex_);
    return {.state = state_,
            .consecutive_failures = consecutive_failures_,
            .probes_in_flight = probes_in_flight_,
            .retry_at = retry_at_};
}

CircuitState CircuitBreaker::state() const {
    return snapshot().state;
}

void CircuitBreaker::open(std::chrono::steady_clock::time_point now) noexcept {
    state_ = CircuitState::Open;
    consecutive_failures_ = 0;
    probes_in_flight_ = 0;
    retry_at_ = now + limits_.cooldown;
}

}  // namespace pulsegate::http
