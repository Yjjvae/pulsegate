#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>

namespace pulsegate::http {

using EndpointId = std::string;

struct EndpointHealth {
    bool healthy{true};
    std::uint32_t consecutive_successes{0};
    std::uint32_t consecutive_failures{0};
    std::chrono::steady_clock::time_point changed_at{};
};

struct HealthSnapshot {
    std::unordered_map<EndpointId, EndpointHealth> endpoints;
};

struct HealthThresholds {
    std::uint32_t healthy_threshold{2};
    std::uint32_t unhealthy_threshold{3};
};

// Writers must be serialized by HealthChecker's strand. Readers obtain a
// stable immutable snapshot without locking the proxy hot path.
class HealthStateStore {
   public:
    explicit HealthStateStore(HealthThresholds thresholds = {});

    void addEndpoint(EndpointId id, bool initially_healthy = true);
    void recordSuccess(const EndpointId& id);
    void recordFailure(const EndpointId& id);
    [[nodiscard]] std::shared_ptr<const HealthSnapshot> snapshot() const noexcept;
    [[nodiscard]] bool isHealthy(const EndpointId& id) const noexcept;

   private:
    void record(const EndpointId& id, bool success);

    HealthThresholds thresholds_;
    std::atomic<std::shared_ptr<const HealthSnapshot>> snapshot_;
};

}  // namespace pulsegate::http
