#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

namespace pulsegate::http {

enum class LogLevel { Trace, Debug, Info, Warn, Error, Critical };

struct LoggerConfig {
    LogLevel level{LogLevel::Info};
    bool json{true};
    std::size_t queue_capacity{4096};
};

struct RequestLog {
    std::string request_id;
    std::string method;
    std::string target;
    int status{0};
    std::chrono::microseconds duration{};
    std::size_t bytes_in{0};
    std::size_t bytes_out{0};
    std::string upstream{};
    std::string cache_status{};
};

// The caller only acquires a mutex long enough to append to a bounded queue.
// The sink runs on this object's dedicated thread, never on an Asio worker.
class AsyncLogger {
   public:
    using Sink = std::function<void(std::string_view)>;

    explicit AsyncLogger(LoggerConfig config = {}, Sink sink = {});
    ~AsyncLogger();
    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    [[nodiscard]] bool logAccess(RequestLog entry);
    [[nodiscard]] std::uint64_t dropped() const noexcept;
    void setLogging(LogLevel level, bool json) noexcept;

   private:
    void run(std::stop_token stop_token);
    [[nodiscard]] std::string format(const RequestLog& entry) const;

    LoggerConfig config_;
    Sink sink_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<RequestLog> queue_;
    std::jthread worker_;
    std::atomic<LogLevel> level_;
    std::atomic_bool json_;
    std::atomic_uint64_t dropped_{0};
};

// A low-cardinality, process-local Prometheus registry. Labels deliberately
// use method/status-class/route names, never request IDs, full targets, or IPs.
class MetricsRegistry {
   public:
    struct Histogram {
        std::uint64_t count{0};
        double sum{0.0};
        std::uint64_t le_1ms{0};
        std::uint64_t le_5ms{0};
        std::uint64_t le_25ms{0};
        std::uint64_t le_100ms{0};
        std::uint64_t le_1s{0};
    };

    void recordHttp(std::string_view method, int status, std::string_view route,
                    std::chrono::microseconds duration);
    void connectionAccepted();
    void connectionClosed();
    void connectionRejected(std::string_view reason);
    void recordUpstream(std::string_view upstream, std::string_view result,
                        std::chrono::microseconds connect_duration = {});
    void recordCache(std::string_view result);
    void recordRateLimitRejection(std::string_view route);
    void setCircuitState(std::string_view upstream, std::string_view state);
    void setRuntimeActiveCoroutines(std::size_t value) noexcept;
    void coroutineStarted() noexcept;
    void coroutineFinished() noexcept;
    void setUpstreamPoolWaiters(std::size_t value) noexcept;
    void setOutputBufferBytes(std::size_t value) noexcept;
    void addOutputBufferBytes(std::size_t value) noexcept;
    void removeOutputBufferBytes(std::size_t value) noexcept;
    void recordLogDrop() noexcept;
    [[nodiscard]] std::string renderPrometheus() const;

   private:
    static std::string escapedLabel(std::string_view value);
    mutable std::mutex mutex_;
    std::map<std::string, std::uint64_t> http_requests_;
    std::map<std::string, Histogram> http_duration_;
    std::map<std::string, std::uint64_t> rejected_connections_;
    std::map<std::string, std::uint64_t> upstream_requests_;
    std::map<std::string, Histogram> upstream_connect_duration_;
    std::map<std::string, std::uint64_t> cache_requests_;
    std::map<std::string, std::uint64_t> rate_limit_rejections_;
    std::map<std::string, std::string> circuit_states_;
    std::atomic_uint64_t active_connections_{0};
    std::atomic_uint64_t accepted_connections_{0};
    std::atomic_uint64_t runtime_active_coroutines_{0};
    std::atomic_uint64_t upstream_pool_waiters_{0};
    std::atomic_uint64_t output_buffer_bytes_{0};
    std::atomic_uint64_t logs_dropped_{0};
};

class Observability {
   public:
    explicit Observability(LoggerConfig config = {});
    [[nodiscard]] MetricsRegistry& metrics() noexcept;
    [[nodiscard]] AsyncLogger& logger() noexcept;
    [[nodiscard]] std::string renderPrometheus() const;
    void setLogging(LogLevel level, bool json) noexcept;

   private:
    MetricsRegistry metrics_;
    AsyncLogger logger_;
};

[[nodiscard]] LogLevel parseLogLevel(std::string_view value);

}  // namespace pulsegate::http
