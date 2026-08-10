#include "pulsegate/net/deadline.h"

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <chrono>
#include <memory>

namespace {

using namespace std::chrono_literals;

TEST(DeadlineTest, DisarmSuppressesPendingExpiry) {
    boost::asio::io_context context;
    auto deadline = std::make_shared<pulsegate::net::Deadline>(context.get_executor());
    bool expired = false;

    deadline->arm(2ms, [&expired] { expired = true; });
    deadline->disarm();
    context.run_for(10ms);

    EXPECT_FALSE(expired);
}

TEST(DeadlineTest, ReArmingSuppressesStaleTimerCallback) {
    boost::asio::io_context context;
    auto deadline = std::make_shared<pulsegate::net::Deadline>(context.get_executor());
    int expired = 0;

    deadline->arm(20ms, [&expired] { ++expired; });
    deadline->arm(2ms, [&expired] { ++expired; });
    context.run_for(30ms);

    EXPECT_EQ(expired, 1);
}

}  // namespace
