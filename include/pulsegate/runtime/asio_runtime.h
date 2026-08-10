#pragma once

#include <atomic>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <cstddef>
#include <exception>
#include <functional>
#include <optional>
#include <thread>
#include <vector>

namespace pulsegate::runtime {

using RuntimeErrorHandler = std::function<void(std::size_t worker_index, std::exception_ptr)>;

// Owns one io_context and N worker threads. Business components arrange their
// own shutdown first, then requestStop() releases the work guard so run() can
// return naturally after cancellation and cleanup handlers finish.
class AsioRuntime {
   public:
    explicit AsioRuntime(std::size_t thread_count, RuntimeErrorHandler on_error = {});
    ~AsioRuntime();

    AsioRuntime(const AsioRuntime&) = delete;
    AsioRuntime& operator=(const AsioRuntime&) = delete;

    [[nodiscard]] boost::asio::io_context& context() noexcept;
    [[nodiscard]] std::size_t threadCount() const noexcept;

    void start();
    void requestStop();
    void join();

   private:
    using WorkGuard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;

    void runWorker(std::size_t worker_index);

    boost::asio::io_context context_;
    std::optional<WorkGuard> work_guard_;
    std::vector<std::jthread> workers_;
    std::size_t thread_count_;
    RuntimeErrorHandler on_error_;
    std::atomic_bool started_{false};
    std::atomic_bool stop_requested_{false};
};

}  // namespace pulsegate::runtime
