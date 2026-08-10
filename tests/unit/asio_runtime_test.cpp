#include "pulsegate/runtime/asio_runtime.h"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <boost/asio/post.hpp>
#include <chrono>
#include <future>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

namespace {

using namespace std::chrono_literals;

TEST(AsioRuntimeTest, RejectsZeroWorkerConfiguration) {
    EXPECT_THROW(pulsegate::runtime::AsioRuntime(0), std::invalid_argument);
}

TEST(AsioRuntimeTest, RunsWorkOnEachConfiguredWorkerAndJoins) {
    constexpr std::size_t kWorkers = 4;
    pulsegate::runtime::AsioRuntime runtime(kWorkers);
    std::barrier rendezvous(static_cast<std::ptrdiff_t>(kWorkers));
    std::promise<void> completed;
    auto completion = completed.get_future();
    std::mutex mutex;
    std::set<std::thread::id> worker_ids;
    std::atomic_size_t passed_barrier{0};

    for (std::size_t index = 0; index < kWorkers; ++index) {
        boost::asio::post(runtime.context(), [&] {
            {
                std::scoped_lock lock(mutex);
                worker_ids.insert(std::this_thread::get_id());
            }
            rendezvous.arrive_and_wait();
            if (passed_barrier.fetch_add(1, std::memory_order_relaxed) + 1 == kWorkers) {
                completed.set_value();
            }
        });
    }

    runtime.start();
    EXPECT_EQ(completion.wait_for(1s), std::future_status::ready);
    runtime.requestStop();
    runtime.join();
    EXPECT_EQ(worker_ids.size(), kWorkers);
}

TEST(AsioRuntimeTest, RejectsASecondStartAndStopsNaturally) {
    pulsegate::runtime::AsioRuntime runtime(1);
    runtime.start();
    EXPECT_THROW(runtime.start(), std::logic_error);
    runtime.requestStop();
    runtime.join();
}

TEST(AsioRuntimeTest, ReportsUnhandledWorkerException) {
    std::promise<std::size_t> reported;
    auto result = reported.get_future();
    pulsegate::runtime::AsioRuntime runtime(
        1, [&reported](std::size_t index, std::exception_ptr) { reported.set_value(index); });
    boost::asio::post(runtime.context(), [] { throw std::runtime_error("test worker failure"); });

    runtime.start();
    EXPECT_EQ(result.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(result.get(), 0U);
    runtime.join();
}

}  // namespace
