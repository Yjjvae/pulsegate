#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include "pulsegate/core/version.h"

TEST(CoreSmokeTest, ReportsProjectVersion) {
    EXPECT_EQ(pulsegate::core::version(), "0.6.0");
}

TEST(AsioSmokeTest, ExecutesPostedHandler) {
    boost::asio::io_context context;
    std::atomic_bool called{false};

    boost::asio::post(context, [&called] { called.store(true, std::memory_order_relaxed); });

    context.run();

    EXPECT_TRUE(called.load(std::memory_order_relaxed));
}
