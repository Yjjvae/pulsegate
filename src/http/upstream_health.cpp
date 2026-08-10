#include "pulsegate/http/upstream_health.h"

#include <stdexcept>
#include <utility>

namespace pulsegate::http {

HealthStateStore::HealthStateStore(HealthThresholds thresholds) : thresholds_(thresholds) {
    if (thresholds_.healthy_threshold == 0 || thresholds_.unhealthy_threshold == 0) {
        throw std::invalid_argument("health thresholds must be positive");
    }
    snapshot_.store(std::make_shared<HealthSnapshot>(), std::memory_order_release);
}

void HealthStateStore::addEndpoint(EndpointId id, bool initially_healthy) {
    if (id.empty()) {
        throw std::invalid_argument("health endpoint id must not be empty");
    }
    auto next = std::make_shared<HealthSnapshot>(*snapshot());
    next->endpoints.emplace(std::move(id),
                            EndpointHealth{.healthy = initially_healthy,
                                           .changed_at = std::chrono::steady_clock::now()});
    snapshot_.store(std::move(next), std::memory_order_release);
}

void HealthStateStore::recordSuccess(const EndpointId& id) {
    record(id, true);
}

void HealthStateStore::recordFailure(const EndpointId& id) {
    record(id, false);
}

std::shared_ptr<const HealthSnapshot> HealthStateStore::snapshot() const noexcept {
    return snapshot_.load(std::memory_order_acquire);
}

bool HealthStateStore::isHealthy(const EndpointId& id) const noexcept {
    const auto current = snapshot();
    const auto found = current->endpoints.find(id);
    return found != current->endpoints.end() && found->second.healthy;
}

void HealthStateStore::record(const EndpointId& id, bool success) {
    auto current = snapshot_.load(std::memory_order_acquire);
    for (;;) {
        auto next = std::make_shared<HealthSnapshot>(*current);
        auto found = next->endpoints.find(id);
        if (found == next->endpoints.end()) {
            return;
        }
        auto& state = found->second;
        if (success) {
            state.consecutive_failures = 0;
            ++state.consecutive_successes;
            if (!state.healthy && state.consecutive_successes >= thresholds_.healthy_threshold) {
                state.healthy = true;
                state.changed_at = std::chrono::steady_clock::now();
            }
        } else {
            state.consecutive_successes = 0;
            ++state.consecutive_failures;
            if (state.healthy && state.consecutive_failures >= thresholds_.unhealthy_threshold) {
                state.healthy = false;
                state.changed_at = std::chrono::steady_clock::now();
            }
        }
        if (snapshot_.compare_exchange_weak(current, std::move(next), std::memory_order_release,
                                            std::memory_order_acquire)) {
            return;
        }
    }
}

}  // namespace pulsegate::http
