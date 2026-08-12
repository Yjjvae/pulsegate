#include "pulsegate/http/observability.h"

#include <algorithm>
#include <array>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace pulsegate::http {
namespace {

std::string jsonEscape(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        switch (character) {
            case '"':
                result.append("\\\"");
                break;
            case '\\':
                result.append("\\\\");
                break;
            case '\n':
                result.append("\\n");
                break;
            case '\r':
                result.append("\\r");
                break;
            case '\t':
                result.append("\\t");
                break;
            default:
                if (static_cast<unsigned char>(character) < 0x20U)
                    result.append("?");
                else
                    result.push_back(character);
        }
    }
    return result;
}

void renderHistogram(std::string& output, std::string_view name, std::string_view labels,
                     const MetricsRegistry::Histogram& histogram) {
    const std::array<std::pair<std::string_view, std::uint64_t>, 5> buckets{{
        {"0.001", histogram.le_1ms},
        {"0.005", histogram.le_5ms},
        {"0.025", histogram.le_25ms},
        {"0.1", histogram.le_100ms},
        {"1", histogram.le_1s},
    }};
    for (const auto& [limit, count] : buckets) {
        output.append(name)
            .append("_bucket{")
            .append(labels)
            .append(",le=\"")
            .append(limit)
            .append("\"} ")
            .append(std::to_string(count))
            .push_back('\n');
    }
    output.append(name)
        .append("_bucket{")
        .append(labels)
        .append(",le=\"+Inf\"} ")
        .append(std::to_string(histogram.count))
        .push_back('\n');
    output.append(name).append("_sum{").append(labels).append("} ");
    std::ostringstream sum;
    sum << std::setprecision(12) << histogram.sum;
    output.append(sum.str()).push_back('\n');
    output.append(name)
        .append("_count{")
        .append(labels)
        .append("} ")
        .append(std::to_string(histogram.count))
        .push_back('\n');
}

void updateHistogram(MetricsRegistry::Histogram& histogram, std::chrono::microseconds duration) {
    const double seconds = static_cast<double>(duration.count()) / 1'000'000.0;
    ++histogram.count;
    histogram.sum += seconds;
    if (seconds <= 0.001) ++histogram.le_1ms;
    if (seconds <= 0.005) ++histogram.le_5ms;
    if (seconds <= 0.025) ++histogram.le_25ms;
    if (seconds <= 0.1) ++histogram.le_100ms;
    if (seconds <= 1.0) ++histogram.le_1s;
}

}  // namespace

AsyncLogger::AsyncLogger(LoggerConfig config, Sink sink)
    : config_(std::move(config)),
      sink_(std::move(sink)),
      level_(config_.level),
      json_(config_.json) {
    if (config_.queue_capacity == 0)
        throw std::invalid_argument("log queue capacity must be positive");
    if (!sink_) sink_ = [](std::string_view line) { std::clog << line << '\n'; };
    worker_ = std::jthread([this](std::stop_token stop_token) { run(stop_token); });
}

AsyncLogger::~AsyncLogger() {
    worker_.request_stop();
    wake_.notify_all();
}

bool AsyncLogger::logAccess(RequestLog entry) {
    if (static_cast<int>(level_.load(std::memory_order_relaxed)) >
        static_cast<int>(LogLevel::Info)) {
        return true;
    }
    std::scoped_lock lock(mutex_);
    if (queue_.size() >= config_.queue_capacity) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    queue_.push_back(std::move(entry));
    wake_.notify_one();
    return true;
}

std::uint64_t AsyncLogger::dropped() const noexcept {
    return dropped_.load(std::memory_order_relaxed);
}

void AsyncLogger::setLogging(LogLevel level, bool json) noexcept {
    level_.store(level, std::memory_order_relaxed);
    json_.store(json, std::memory_order_relaxed);
}

void AsyncLogger::run(std::stop_token stop_token) {
    for (;;) {
        RequestLog entry;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&stop_token, this] {
                return stop_token.stop_requested() || !queue_.empty();
            });
            if (queue_.empty()) {
                if (stop_token.stop_requested()) return;
                continue;
            }
            entry = std::move(queue_.front());
            queue_.pop_front();
        }
        sink_(format(entry));
    }
}

std::string AsyncLogger::format(const RequestLog& entry) const {
    if (!json_.load(std::memory_order_relaxed)) {
        return "level=info request_id=" + entry.request_id + " method=" + entry.method +
               " target=" + entry.target + " status=" + std::to_string(entry.status) +
               " duration_us=" + std::to_string(entry.duration.count()) +
               " bytes_in=" + std::to_string(entry.bytes_in) +
               " bytes_out=" + std::to_string(entry.bytes_out) + " upstream=" + entry.upstream +
               " cache=" + entry.cache_status;
    }
    return "{\"level\":\"info\",\"request_id\":\"" + jsonEscape(entry.request_id) +
           "\",\"method\":\"" + jsonEscape(entry.method) + "\",\"target\":\"" +
           jsonEscape(entry.target) + "\",\"status\":" + std::to_string(entry.status) +
           ",\"duration_us\":" + std::to_string(entry.duration.count()) +
           ",\"bytes_in\":" + std::to_string(entry.bytes_in) +
           ",\"bytes_out\":" + std::to_string(entry.bytes_out) + ",\"upstream\":\"" +
           jsonEscape(entry.upstream) + "\",\"cache\":\"" + jsonEscape(entry.cache_status) + "\"}";
}

void MetricsRegistry::recordHttp(std::string_view method, int status, std::string_view route,
                                 std::chrono::microseconds duration) {
    const auto labels = "method=\"" + escapedLabel(method) + "\",status_class=\"" +
                        std::to_string(std::clamp(status / 100, 0, 9)) + "xx\",route=\"" +
                        escapedLabel(route) + "\"";
    std::scoped_lock lock(mutex_);
    ++http_requests_[labels];
    updateHistogram(http_duration_[labels], duration);
}

void MetricsRegistry::connectionAccepted() {
    accepted_connections_.fetch_add(1, std::memory_order_relaxed);
    active_connections_.fetch_add(1, std::memory_order_relaxed);
}
void MetricsRegistry::connectionClosed() {
    active_connections_.fetch_sub(1, std::memory_order_relaxed);
}
void MetricsRegistry::connectionRejected(std::string_view reason) {
    std::scoped_lock lock(mutex_);
    ++rejected_connections_["reason=\"" + escapedLabel(reason) + "\""];
}
void MetricsRegistry::recordUpstream(std::string_view upstream, std::string_view result,
                                     std::chrono::microseconds connect_duration) {
    const auto labels =
        "upstream=\"" + escapedLabel(upstream) + "\",result=\"" + escapedLabel(result) + "\"";
    std::scoped_lock lock(mutex_);
    ++upstream_requests_[labels];
    if (connect_duration.count() > 0)
        updateHistogram(upstream_connect_duration_[labels], connect_duration);
}
void MetricsRegistry::recordCache(std::string_view result) {
    std::scoped_lock lock(mutex_);
    ++cache_requests_["result=\"" + escapedLabel(result) + "\""];
}
void MetricsRegistry::recordRateLimitRejection(std::string_view route) {
    std::scoped_lock lock(mutex_);
    ++rate_limit_rejections_["route=\"" + escapedLabel(route) + "\""];
}
void MetricsRegistry::setCircuitState(std::string_view upstream, std::string_view state) {
    std::scoped_lock lock(mutex_);
    circuit_states_[std::string(upstream)] = std::string(state);
}
void MetricsRegistry::setRuntimeActiveCoroutines(std::size_t value) noexcept {
    runtime_active_coroutines_.store(value);
}
void MetricsRegistry::coroutineStarted() noexcept {
    runtime_active_coroutines_.fetch_add(1, std::memory_order_relaxed);
}
void MetricsRegistry::coroutineFinished() noexcept {
    runtime_active_coroutines_.fetch_sub(1, std::memory_order_relaxed);
}
void MetricsRegistry::setUpstreamPoolWaiters(std::size_t value) noexcept {
    upstream_pool_waiters_.store(value);
}
void MetricsRegistry::setOutputBufferBytes(std::size_t value) noexcept {
    output_buffer_bytes_.store(value);
}
void MetricsRegistry::addOutputBufferBytes(std::size_t value) noexcept {
    output_buffer_bytes_.fetch_add(value, std::memory_order_relaxed);
}
void MetricsRegistry::removeOutputBufferBytes(std::size_t value) noexcept {
    output_buffer_bytes_.fetch_sub(value, std::memory_order_relaxed);
}
void MetricsRegistry::recordLogDrop() noexcept {
    logs_dropped_.fetch_add(1, std::memory_order_relaxed);
}

std::string MetricsRegistry::renderPrometheus() const {
    std::string output;
    output.append(
        "# HELP pulsegate_http_requests_total Total HTTP requests.\n# TYPE "
        "pulsegate_http_requests_total counter\n");
    std::scoped_lock lock(mutex_);
    for (const auto& [labels, value] : http_requests_)
        output.append("pulsegate_http_requests_total{")
            .append(labels)
            .append("} ")
            .append(std::to_string(value))
            .push_back('\n');
    output.append(
        "# HELP pulsegate_http_request_duration_seconds HTTP request duration.\n# TYPE "
        "pulsegate_http_request_duration_seconds histogram\n");
    for (const auto& [labels, value] : http_duration_)
        renderHistogram(output, "pulsegate_http_request_duration_seconds", labels, value);
    output.append("# TYPE pulsegate_rejected_connections_total counter\n");
    for (const auto& [labels, value] : rejected_connections_)
        output.append("pulsegate_rejected_connections_total{")
            .append(labels)
            .append("} ")
            .append(std::to_string(value))
            .push_back('\n');
    output.append("# TYPE pulsegate_upstream_requests_total counter\n");
    for (const auto& [labels, value] : upstream_requests_)
        output.append("pulsegate_upstream_requests_total{")
            .append(labels)
            .append("} ")
            .append(std::to_string(value))
            .push_back('\n');
    output.append("# TYPE pulsegate_upstream_connect_duration_seconds histogram\n");
    for (const auto& [labels, value] : upstream_connect_duration_)
        renderHistogram(output, "pulsegate_upstream_connect_duration_seconds", labels, value);
    output.append("# TYPE pulsegate_cache_requests_total counter\n");
    for (const auto& [labels, value] : cache_requests_)
        output.append("pulsegate_cache_requests_total{")
            .append(labels)
            .append("} ")
            .append(std::to_string(value))
            .push_back('\n');
    output.append("# TYPE pulsegate_rate_limit_rejections_total counter\n");
    for (const auto& [labels, value] : rate_limit_rejections_)
        output.append("pulsegate_rate_limit_rejections_total{")
            .append(labels)
            .append("} ")
            .append(std::to_string(value))
            .push_back('\n');
    output.append("# TYPE pulsegate_circuit_state gauge\n");
    for (const auto& [upstream, state] : circuit_states_) {
        for (const auto candidate : {"closed", "open", "half_open"})
            output.append("pulsegate_circuit_state{upstream=\"")
                .append(escapedLabel(upstream))
                .append("\",state=\"")
                .append(candidate)
                .append("\"} ")
                .append(state == candidate ? "1\n" : "0\n");
    }
    output.append("# TYPE pulsegate_active_connections gauge\npulsegate_active_connections ")
        .append(std::to_string(active_connections_.load()))
        .push_back('\n');
    output
        .append(
            "# TYPE pulsegate_accepted_connections_total "
            "counter\npulsegate_accepted_connections_total ")
        .append(std::to_string(accepted_connections_.load()))
        .push_back('\n');
    output
        .append(
            "# TYPE pulsegate_runtime_active_coroutines "
            "gauge\npulsegate_runtime_active_coroutines ")
        .append(std::to_string(runtime_active_coroutines_.load()))
        .push_back('\n');
    output.append("# TYPE pulsegate_upstream_pool_waiters gauge\npulsegate_upstream_pool_waiters ")
        .append(std::to_string(upstream_pool_waiters_.load()))
        .push_back('\n');
    output.append("# TYPE pulsegate_output_buffer_bytes gauge\npulsegate_output_buffer_bytes ")
        .append(std::to_string(output_buffer_bytes_.load()))
        .push_back('\n');
    output.append("# TYPE pulsegate_logs_dropped_total counter\npulsegate_logs_dropped_total ")
        .append(std::to_string(logs_dropped_.load()))
        .push_back('\n');
    return output;
}

std::string MetricsRegistry::escapedLabel(std::string_view value) {
    return jsonEscape(value);
}

Observability::Observability(LoggerConfig config) : logger_(std::move(config)) {}
MetricsRegistry& Observability::metrics() noexcept {
    return metrics_;
}
AsyncLogger& Observability::logger() noexcept {
    return logger_;
}
std::string Observability::renderPrometheus() const {
    return metrics_.renderPrometheus();
}
void Observability::setLogging(LogLevel level, bool json) noexcept {
    logger_.setLogging(level, json);
}

LogLevel parseLogLevel(std::string_view value) {
    if (value == "trace") return LogLevel::Trace;
    if (value == "debug") return LogLevel::Debug;
    if (value == "info") return LogLevel::Info;
    if (value == "warn" || value == "warning") return LogLevel::Warn;
    if (value == "error") return LogLevel::Error;
    if (value == "critical") return LogLevel::Critical;
    throw std::invalid_argument(
        "logging.level must be trace, debug, info, warn, error, or critical");
}

}  // namespace pulsegate::http
