#include "pulsegate/http/observability.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <string>

namespace pulsegate::http {
namespace {

TEST(AsyncLoggerTest, UsesBoundedQueueAndCountsDroppedAccessLogs) {
    std::promise<void> sink_started;
    std::promise<void> release_sink;
    const auto release = release_sink.get_future().share();
    std::atomic_bool first{true};
    AsyncLogger logger({.level = LogLevel::Info, .json = true, .queue_capacity = 1},
                       [&first, &sink_started, release](std::string_view) mutable {
                           if (first.exchange(false)) {
                               sink_started.set_value();
                               release.wait();
                           }
                       });

    EXPECT_TRUE(logger.logAccess({.request_id = "first", .method = "GET", .target = "/livez"}));
    sink_started.get_future().wait();
    EXPECT_TRUE(logger.logAccess({.request_id = "second", .method = "GET", .target = "/readyz"}));
    EXPECT_FALSE(logger.logAccess({.request_id = "third", .method = "GET", .target = "/metrics"}));
    EXPECT_EQ(logger.dropped(), 1U);
    release_sink.set_value();
}

TEST(MetricsRegistryTest, RendersLowCardinalityCountersHistogramsAndGauges) {
    MetricsRegistry metrics;
    metrics.connectionAccepted();
    metrics.recordHttp("GET", 200, "healthz", std::chrono::microseconds(800));
    metrics.recordHttp("GET", 404, "unmatched", std::chrono::milliseconds(2));
    metrics.connectionRejected("resource_limit");
    metrics.recordUpstream("catalog", "success", std::chrono::milliseconds(4));
    metrics.recordCache("hit");
    metrics.recordRateLimitRejection("api");
    metrics.setCircuitState("catalog", "open");
    metrics.setRuntimeActiveCoroutines(3);
    metrics.setUpstreamPoolWaiters(2);
    metrics.setOutputBufferBytes(128);
    metrics.recordLogDrop();

    const auto rendered = metrics.renderPrometheus();
    EXPECT_NE(rendered.find("pulsegate_http_requests_total{method=\"GET\",status_class=\"2xx\","
                            "route=\"healthz\"} 1"),
              std::string::npos);
    EXPECT_NE(rendered.find("pulsegate_http_request_duration_seconds_bucket{method=\"GET\",status_"
                            "class=\"2xx\",route=\"healthz\",le=\"0.001\"} 1"),
              std::string::npos);
    EXPECT_NE(rendered.find("pulsegate_rejected_connections_total{reason=\"resource_limit\"} 1"),
              std::string::npos);
    EXPECT_NE(rendered.find(
                  "pulsegate_upstream_requests_total{upstream=\"catalog\",result=\"success\"} 1"),
              std::string::npos);
    EXPECT_NE(rendered.find("pulsegate_cache_requests_total{result=\"hit\"} 1"), std::string::npos);
    EXPECT_NE(rendered.find("pulsegate_rate_limit_rejections_total{route=\"api\"} 1"),
              std::string::npos);
    EXPECT_NE(rendered.find("pulsegate_circuit_state{upstream=\"catalog\",state=\"open\"} 1"),
              std::string::npos);
    EXPECT_NE(rendered.find("pulsegate_active_connections 1"), std::string::npos);
    EXPECT_NE(rendered.find("pulsegate_logs_dropped_total 1"), std::string::npos);
}

}  // namespace
}  // namespace pulsegate::http
