#include "pulsegate/http/upstream_pool.h"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_future.hpp>
#include <thread>

namespace pulsegate::http {
namespace {

TEST(UpstreamPoolPolicyTest, ReusesReturnedConnectionExclusively) {
    UpstreamPoolPolicy pool({.max_connections = 2, .max_idle_connections = 2, .max_reuses = 3});
    auto first = pool.acquire();
    ASSERT_TRUE(first.valid());
    const auto id = first.connectionId();
    EXPECT_EQ(pool.busyCount(), 1U);
    pool.releaseReusable(std::move(first));
    EXPECT_EQ(pool.busyCount(), 0U);
    EXPECT_EQ(pool.idleCount(), 1U);

    auto second = pool.acquire();
    EXPECT_TRUE(second.valid());
    EXPECT_EQ(second.connectionId(), id);
    EXPECT_EQ(pool.busyCount(), 1U);
    EXPECT_EQ(pool.idleCount(), 0U);
}

TEST(UpstreamConnectionTest, StartsClosedAndRetainsItsEndpointIdentity) {
    boost::asio::io_context context;
    UpstreamConnection connection(context.get_executor(),
                                  UpstreamEndpoint{.host = "backend.local", .service = "8081"});
    EXPECT_EQ(connection.endpoint().host, "backend.local");
    EXPECT_EQ(connection.endpoint().service, "8081");
    EXPECT_FALSE(connection.isOpen());
}

TEST(UpstreamPoolPolicyTest, EnforcesCapacityAndDiscardNeverReturnsIdle) {
    UpstreamPoolPolicy pool({.max_connections = 1, .max_idle_connections = 1, .max_reuses = 3});
    auto lease = pool.acquire();
    ASSERT_TRUE(lease.valid());
    EXPECT_FALSE(pool.acquire().valid());
    pool.discard(std::move(lease), DiscardReason::ProtocolError);
    EXPECT_EQ(pool.busyCount(), 0U);
    EXPECT_EQ(pool.idleCount(), 0U);
    EXPECT_TRUE(pool.acquire().valid());
}

TEST(UpstreamPoolPolicyTest, StopsReusingAfterConfiguredReuseLimit) {
    UpstreamPoolPolicy pool({.max_connections = 1, .max_idle_connections = 1, .max_reuses = 1});
    auto first = pool.acquire();
    ASSERT_TRUE(first.valid());
    const auto id = first.connectionId();
    pool.releaseReusable(std::move(first));
    EXPECT_EQ(pool.idleCount(), 0U);
    auto second = pool.acquire();
    EXPECT_TRUE(second.valid());
    EXPECT_NE(second.connectionId(), id);
}

TEST(UpstreamPoolPolicyTest, PrunesConnectionsThatExceedTheIdleTimeout) {
    UpstreamPoolPolicy pool({.max_connections = 1,
                             .max_idle_connections = 1,
                             .max_reuses = 3,
                             .idle_timeout = std::chrono::milliseconds(1)});
    auto lease = pool.acquire();
    ASSERT_TRUE(lease.valid());
    pool.releaseReusable(std::move(lease));
    ASSERT_EQ(pool.idleCount(), 1U);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    pool.pruneIdle();
    EXPECT_EQ(pool.idleCount(), 0U);
}

TEST(UpstreamPoolTest, AsyncAcquireReturnsLeaseAndReportsCapacityOnCallerExecutor) {
    boost::asio::io_context context;
    auto pool = std::make_shared<UpstreamPool>(
        context.get_executor(),
        PoolLimits{.max_connections = 1, .max_idle_connections = 1, .max_waiters = 0});
    auto future = boost::asio::co_spawn(
        context,
        [pool]() -> net::Awaitable<std::pair<net::ErrorCode, net::ErrorCode>> {
            net::ErrorCode first_error;
            auto first = co_await pool->asyncAcquire(
                net::asio::redirect_error(net::use_awaitable, first_error));
            net::ErrorCode second_error;
            [[maybe_unused]] auto second = co_await pool->asyncAcquire(
                net::asio::redirect_error(net::use_awaitable, second_error));
            pool->discard(std::move(first), DiscardReason::LeaseAbandoned);
            pool->stop();
            co_return std::pair{first_error, second_error};
        },
        boost::asio::use_future);
    context.run();
    const auto [first, second] = future.get();
    EXPECT_FALSE(first);
    EXPECT_EQ(second, boost::asio::error::no_buffer_space);
}

TEST(UpstreamPoolTest, ForgottenLeaseIsDiscardedRatherThanReused) {
    boost::asio::io_context context;
    auto pool = std::make_shared<UpstreamPool>(
        context.get_executor(), PoolLimits{.max_connections = 1, .max_idle_connections = 1});
    auto future = boost::asio::co_spawn(
        context,
        [pool]() -> net::Awaitable<std::pair<std::uint64_t, std::uint64_t>> {
            net::ErrorCode error;
            std::uint64_t first_id = 0;
            {
                auto first = co_await pool->asyncAcquire(
                    net::asio::redirect_error(net::use_awaitable, error));
                EXPECT_FALSE(error);
                first_id = first.connectionId();
            }
            // Let the destructor's discard post run before the next acquire.
            co_await net::asio::post(net::use_awaitable);
            auto second =
                co_await pool->asyncAcquire(net::asio::redirect_error(net::use_awaitable, error));
            EXPECT_FALSE(error);
            const auto second_id = second.connectionId();
            pool->discard(std::move(second), DiscardReason::LeaseAbandoned);
            pool->stop();
            co_return std::pair{first_id, second_id};
        },
        boost::asio::use_future);
    context.run();
    const auto [first, second] = future.get();
    EXPECT_NE(first, second);
}

TEST(UpstreamPoolTest, StopCompletesQueuedAcquireWithOperationAborted) {
    boost::asio::io_context context;
    auto pool = std::make_shared<UpstreamPool>(
        context.get_executor(), PoolLimits{.max_connections = 1,
                                           .max_idle_connections = 1,
                                           .acquire_timeout = std::chrono::seconds(5),
                                           .idle_timeout = std::chrono::seconds(5)});
    auto first_future = boost::asio::co_spawn(
        context,
        [pool]() -> net::Awaitable<UpstreamLease> {
            net::ErrorCode error;
            co_return co_await pool->asyncAcquire(
                net::asio::redirect_error(net::use_awaitable, error));
        },
        boost::asio::use_future);
    context.run_for(std::chrono::milliseconds(10));
    auto first = first_future.get();
    ASSERT_TRUE(first.valid());

    context.restart();
    auto waiter = boost::asio::co_spawn(
        context,
        [pool]() -> net::Awaitable<net::ErrorCode> {
            net::ErrorCode error;
            [[maybe_unused]] auto lease =
                co_await pool->asyncAcquire(net::asio::redirect_error(net::use_awaitable, error));
            co_return error;
        },
        boost::asio::use_future);
    auto stopper = boost::asio::co_spawn(
        context,
        [pool]() -> net::Awaitable<void> {
            net::asio::steady_timer timer(co_await net::asio::this_coro::executor);
            timer.expires_after(std::chrono::milliseconds(1));
            co_await timer.async_wait(net::use_awaitable);
            pool->stop();
        },
        boost::asio::use_future);
    context.run();
    stopper.get();
    EXPECT_EQ(waiter.get(), boost::asio::error::operation_aborted);
    first = {};
}

}  // namespace
}  // namespace pulsegate::http
