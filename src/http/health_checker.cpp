#include "pulsegate/http/health_checker.h"

#include <algorithm>
#include <array>
#include <boost/asio/connect.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>
#include <stdexcept>
#include <utility>

#include "pulsegate/runtime/coroutine_guard.h"

namespace pulsegate::http {
struct ProbeTransport : std::enable_shared_from_this<ProbeTransport> {
    explicit ProbeTransport(net::asio::any_io_executor executor)
        : resolver(executor), socket(executor), timer(executor) {}
    net::tcp::resolver resolver;
    net::tcp::socket socket;
    net::asio::steady_timer timer;
    bool timed_out{false};

    void cancel() {
        timer.cancel();
        resolver.cancel();
        net::ErrorCode ignored;
        socket.cancel(ignored);
        socket.close(ignored);
    }
};

namespace {

std::string endpointId(const UpstreamEndpoint& endpoint) {
    return endpoint.host + ":" + endpoint.service;
}

}  // namespace

HealthChecker::HealthChecker(net::asio::any_io_executor executor, HealthCheckConfig config,
                             std::vector<UpstreamEndpoint> endpoints,
                             std::shared_ptr<HealthStateStore> health)
    : strand_(net::asio::make_strand(std::move(executor))),
      interval_timer_(strand_),
      config_(std::move(config)),
      endpoints_(std::move(endpoints)),
      health_(std::move(health)) {
    if (!health_ || config_.path.empty() || config_.interval <= std::chrono::milliseconds::zero() ||
        config_.timeout <= std::chrono::milliseconds::zero() ||
        config_.max_concurrent_probes == 0) {
        throw std::invalid_argument("health checker configuration is invalid");
    }
}

void HealthChecker::start() {
    const auto self = shared_from_this();
    net::asio::dispatch(strand_, [self] { self->startInStrand(); });
}

void HealthChecker::stop() {
    const auto self = shared_from_this();
    net::asio::dispatch(strand_, [self] { self->stopInStrand(); });
}

void HealthChecker::startInStrand() {
    if (started_ || stopping_) return;
    started_ = true;
    const auto self = shared_from_this();
    runtime::spawnGuarded(strand_, "health_checker", self->run(),
                          [self](std::string_view, std::exception_ptr) {});
}

void HealthChecker::stopInStrand() {
    if (stopping_) return;
    stopping_ = true;
    interval_timer_.cancel();
    for (const auto& [_, cancel] : active_cancellations_) cancel();
}

net::Awaitable<void> HealthChecker::run() {
    while (!stopping_) {
        const auto probes_this_round = std::min(endpoints_.size(), config_.max_concurrent_probes);
        for (std::size_t index = 0; index < probes_this_round && !stopping_; ++index) {
            if (active_probes_ >= config_.max_concurrent_probes) break;
            const auto endpoint = endpoints_[next_endpoint_];
            next_endpoint_ = (next_endpoint_ + 1) % endpoints_.size();
            const auto probe_id = next_probe_id_++;
            ++active_probes_;
            const auto self = shared_from_this();
            runtime::spawnGuarded(strand_, "health_checker.probe",
                                  self->probeAndPublish(endpoint, probe_id),
                                  [self](std::string_view, std::exception_ptr) {});
        }
        interval_timer_.expires_after(config_.interval);
        net::ErrorCode error;
        co_await interval_timer_.async_wait(net::asio::redirect_error(net::use_awaitable, error));
        if (error || stopping_) co_return;
    }
}

net::Awaitable<void> HealthChecker::probeAndPublish(UpstreamEndpoint endpoint,
                                                    std::size_t probe_id) {
    auto transport = std::make_shared<ProbeTransport>(strand_);
    active_cancellations_.emplace(probe_id, [weak = std::weak_ptr<ProbeTransport>(transport)] {
        if (const auto locked = weak.lock()) locked->cancel();
    });
    const bool ok = co_await probe(endpoint, std::move(transport));
    active_cancellations_.erase(probe_id);
    if (!stopping_) {
        if (ok)
            health_->recordSuccess(endpointId(endpoint));
        else
            health_->recordFailure(endpointId(endpoint));
    }
    --active_probes_;
}

net::Awaitable<bool> HealthChecker::probe(const UpstreamEndpoint& endpoint,
                                          std::shared_ptr<ProbeTransport> transport) {
    transport->timer.expires_after(config_.timeout);
    transport->timer.async_wait([transport](const net::ErrorCode& error) {
        if (!error) {
            transport->timed_out = true;
            transport->resolver.cancel();
            net::ErrorCode ignored;
            transport->socket.cancel(ignored);
            transport->socket.close(ignored);
        }
    });
    net::ErrorCode error;
    const auto resolved = co_await transport->resolver.async_resolve(
        endpoint.host, endpoint.service, net::asio::redirect_error(net::use_awaitable, error));
    if (!error) {
        co_await net::asio::async_connect(transport->socket, resolved,
                                          net::asio::redirect_error(net::use_awaitable, error));
    }
    if (!error) {
        const auto request = "GET " + config_.path + " HTTP/1.1\r\nHost: " + endpoint.host +
                             "\r\nConnection: close\r\n\r\n";
        co_await net::asio::async_write(transport->socket, net::asio::buffer(request),
                                        net::asio::redirect_error(net::use_awaitable, error));
    }
    std::array<char, 64> bytes{};
    std::size_t count = 0;
    if (!error) {
        count = co_await transport->socket.async_read_some(
            net::asio::buffer(bytes), net::asio::redirect_error(net::use_awaitable, error));
    }
    transport->timer.cancel();
    net::ErrorCode ignored;
    transport->socket.close(ignored);
    if (error || transport->timed_out) co_return false;
    const std::string_view status(bytes.data(), count);
    co_return status.starts_with("HTTP/1.1 2") || status.starts_with("HTTP/1.0 2");
}

}  // namespace pulsegate::http
