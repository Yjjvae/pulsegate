#include "pulsegate/http/health_checker.h"

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>

namespace pulsegate::http {
namespace {

TEST(HealthCheckerTest, StopCancelsTheSchedulingTimerWithoutStartingNewProbes) {
    boost::asio::io_context context;
    auto health = std::make_shared<HealthStateStore>();
    auto checker =
        std::make_shared<HealthChecker>(context.get_executor(),
                                        HealthCheckConfig{.path = "/healthz",
                                                          .interval = std::chrono::seconds(1),
                                                          .timeout = std::chrono::milliseconds(100),
                                                          .max_concurrent_probes = 1},
                                        std::vector<UpstreamEndpoint>{}, health);

    checker->start();
    checker->stop();
    context.run();
    SUCCEED();
}

}  // namespace
}  // namespace pulsegate::http
