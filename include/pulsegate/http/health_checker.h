#pragma once

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "pulsegate/http/reverse_proxy.h"
#include "pulsegate/http/upstream_health.h"

namespace pulsegate::http {

struct ProbeTransport;

struct HealthCheckConfig {
    std::string path{"/healthz"};
    std::chrono::milliseconds interval{std::chrono::seconds(2)};
    std::chrono::milliseconds timeout{std::chrono::milliseconds(500)};
    std::size_t max_concurrent_probes{4};
};

class HealthChecker : public std::enable_shared_from_this<HealthChecker> {
   public:
    HealthChecker(net::asio::any_io_executor executor, HealthCheckConfig config,
                  std::vector<UpstreamEndpoint> endpoints,
                  std::shared_ptr<HealthStateStore> health);

    void start();
    void stop();

   private:
    net::Awaitable<void> run();
    net::Awaitable<void> probeAndPublish(UpstreamEndpoint endpoint, std::size_t probe_id);
    net::Awaitable<bool> probe(const UpstreamEndpoint& endpoint,
                               std::shared_ptr<ProbeTransport> transport);
    void startInStrand();
    void stopInStrand();

    net::asio::strand<net::asio::any_io_executor> strand_;
    net::asio::steady_timer interval_timer_;
    HealthCheckConfig config_;
    std::vector<UpstreamEndpoint> endpoints_;
    std::shared_ptr<HealthStateStore> health_;
    bool started_{false};
    bool stopping_{false};
    std::size_t active_probes_{0};
    std::size_t next_endpoint_{0};
    std::size_t next_probe_id_{1};
    std::unordered_map<std::size_t, std::function<void()>> active_cancellations_;
};

}  // namespace pulsegate::http
