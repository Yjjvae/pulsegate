#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "pulsegate/http/circuit_breaker.h"
#include "pulsegate/http/http_response_parser.h"
#include "pulsegate/http/router.h"
#include "pulsegate/http/upstream_endpoint.h"
#include "pulsegate/http/upstream_health.h"
#include "pulsegate/http/upstream_pool.h"
#include "pulsegate/net/deadline.h"

namespace pulsegate::http {

struct ProxyLimits {
    HttpResponseParserLimits response_parser{};
    std::size_t read_chunk_bytes{8192};
    std::size_t max_buffer_bytes{1024 * 1024 + 32 * 1024};
    std::chrono::milliseconds dns_timeout{2000};
    std::chrono::milliseconds connect_timeout{2000};
    std::chrono::milliseconds response_timeout{5000};
    std::chrono::milliseconds total_timeout{10000};
    HealthThresholds health_thresholds{};
    PoolLimits pool_limits{};
    CircuitBreakerLimits circuit_breaker{};
    // Bounds active proxy transactions across all endpoints of this proxy.
    std::size_t max_in_flight_requests{1024};
    std::chrono::seconds overload_retry_after{1};
    bool count_5xx_as_health_failure{true};
};

enum class ProxyState {
    Created,
    Resolving,
    Connecting,
    SendingRequest,
    ReadingResponseHeaders,
    ReadingResponseBody,
    Complete,
    Failed,
    Cancelled
};
enum class ProxyStopReason {
    None,
    DnsTimeout,
    ConnectTimeout,
    ResponseTimeout,
    TotalTimeout,
    DownstreamClosed,
    ServerShutdown,
    InternalError
};
enum class ProxyError {
    None,
    DnsFailure,
    ConnectFailure,
    ConnectTimeout,
    ResponseTimeout,
    UpstreamProtocol,
    UpstreamBodyTooLarge,
    NoHealthyUpstream,
    CircuitOpen,
    Overloaded,
    Cancelled,
    UnsupportedRequest
};

struct ProxyResult {
    ProxyError error{ProxyError::None};
    HttpResponse response;
    UpstreamEndpoint upstream;
    bool reusable{false};
    int upstream_status{0};
};

[[nodiscard]] std::string serializeUpstreamRequest(const HttpRequest& request,
                                                   const UpstreamEndpoint& upstream,
                                                   const RequestContext& context);
[[nodiscard]] HttpResponse makeProxyErrorResponse(ProxyError error);

// A single fully-buffered upstream transaction. All owned Asio objects share
// the downstream Session executor, which is a strand in the normal server path.
class ProxySession : public std::enable_shared_from_this<ProxySession> {
   public:
    ProxySession(net::asio::any_io_executor executor, UpstreamEndpoint upstream,
                 ProxyLimits limits = {}, UpstreamLease lease = {});

    net::Awaitable<ProxyResult> execute(RequestContext& context, HttpRequest request);
    // Streams a chunked downstream response after upstream headers are
    // available. On an error after headers, it returns an already-written
    // close response; an HTTP error body must never follow partial bytes.
    net::Awaitable<ProxyResult> forward(RequestContext& context, HttpRequest request);
    void cancel(ProxyStopReason reason);
    [[nodiscard]] UpstreamLease takeLease();
    [[nodiscard]] ProxyState state() const noexcept;
    [[nodiscard]] ProxyStopReason stopReason() const noexcept;

   private:
    net::Awaitable<bool> resolve();
    net::Awaitable<bool> connect();
    net::Awaitable<bool> sendRequest(const std::string& bytes);
    net::Awaitable<ProxyResult> readResponse(bool head_request);
    net::Awaitable<ProxyResult> forwardResponse(RequestContext& context);
    void cancelInExecutor(ProxyStopReason reason);
    void armPhase(std::chrono::milliseconds timeout, ProxyStopReason reason);
    void disarmTimers();
    [[nodiscard]] ProxyError errorForStopReason() const noexcept;

    net::asio::any_io_executor executor_;
    UpstreamEndpoint upstream_;
    ProxyLimits limits_;
    std::shared_ptr<UpstreamConnection> connection_;
    std::optional<UpstreamLease> lease_;
    std::shared_ptr<net::Deadline> phase_deadline_;
    std::shared_ptr<net::Deadline> total_deadline_;
    net::Buffer input_;
    HttpResponseParser parser_;
    net::tcp::resolver::results_type resolved_;
    ProxyState state_{ProxyState::Created};
    ProxyStopReason stop_reason_{ProxyStopReason::None};
};

// Router handler with lock-free round-robin selection and immutable health
// snapshots. Connection pooling is introduced alongside this stage.
class ReverseProxy {
   public:
    ReverseProxy(std::vector<UpstreamEndpoint> upstreams, ProxyLimits limits = {});
    net::Awaitable<HttpResponse> operator()(RequestContext& context, HttpRequest request) const;
    [[nodiscard]] std::shared_ptr<HealthStateStore> health() const noexcept;
    void stop() const;

   private:
    struct State {
        State(std::vector<UpstreamEndpoint> values, ProxyLimits configured_limits)
            : upstreams(std::move(values)), limits(std::move(configured_limits)) {}
        std::vector<UpstreamEndpoint> upstreams;
        ProxyLimits limits;
        std::shared_ptr<HealthStateStore> health;
        std::vector<std::shared_ptr<CircuitBreaker>> circuits;
        [[nodiscard]] std::shared_ptr<UpstreamPool> poolFor(std::size_t index,
                                                            net::asio::any_io_executor executor);
        mutable std::mutex pools_mutex;
        std::vector<std::shared_ptr<UpstreamPool>> pools;
        std::atomic<std::size_t> next{0};
        std::atomic<std::size_t> in_flight{0};
    };
    std::shared_ptr<State> state_;
};

}  // namespace pulsegate::http
