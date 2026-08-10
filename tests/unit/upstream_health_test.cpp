#include "pulsegate/http/upstream_health.h"

#include <gtest/gtest.h>

namespace pulsegate::http {
namespace {

TEST(HealthStateStoreTest, AppliesSuccessAndFailureThresholdsToSnapshots) {
    HealthStateStore health({.healthy_threshold = 2, .unhealthy_threshold = 3});
    health.addEndpoint("orders", true);
    const auto first_snapshot = health.snapshot();

    health.recordFailure("orders");
    health.recordFailure("orders");
    EXPECT_TRUE(health.isHealthy("orders"));
    health.recordFailure("orders");
    EXPECT_FALSE(health.isHealthy("orders"));
    EXPECT_TRUE(first_snapshot->endpoints.at("orders").healthy);

    health.recordSuccess("orders");
    EXPECT_FALSE(health.isHealthy("orders"));
    health.recordSuccess("orders");
    EXPECT_TRUE(health.isHealthy("orders"));
}

TEST(HealthStateStoreTest, RejectsInvalidConfigurationAndKeepsUnknownEndpointSafe) {
    EXPECT_THROW((HealthStateStore({.healthy_threshold = 0, .unhealthy_threshold = 1})),
                 std::invalid_argument);
    HealthStateStore health;
    EXPECT_FALSE(health.isHealthy("missing"));
    health.recordFailure("missing");
    EXPECT_TRUE(health.snapshot()->endpoints.empty());
}

}  // namespace
}  // namespace pulsegate::http
