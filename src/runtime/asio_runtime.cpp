#include "pulsegate/runtime/asio_runtime.h"

#include <iostream>
#include <stdexcept>
#include <utility>

namespace pulsegate::runtime {

AsioRuntime::AsioRuntime(std::size_t thread_count, RuntimeErrorHandler on_error)
    : thread_count_(thread_count), on_error_(std::move(on_error)) {
    if (thread_count_ == 0) {
        throw std::invalid_argument("AsioRuntime requires at least one worker thread");
    }
}

AsioRuntime::~AsioRuntime() {
    requestStop();
    join();
}

boost::asio::io_context& AsioRuntime::context() noexcept {
    return context_;
}

std::size_t AsioRuntime::threadCount() const noexcept {
    return thread_count_;
}

void AsioRuntime::start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        throw std::logic_error("AsioRuntime has already been started");
    }

    work_guard_.emplace(boost::asio::make_work_guard(context_));
    workers_.reserve(thread_count_);
    for (std::size_t worker_index = 0; worker_index < thread_count_; ++worker_index) {
        workers_.emplace_back([this, worker_index] { runWorker(worker_index); });
    }
}

void AsioRuntime::requestStop() {
    if (!started_.load()) {
        return;
    }
    if (!stop_requested_.exchange(true)) {
        work_guard_.reset();
    }
}

void AsioRuntime::join() {
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void AsioRuntime::runWorker(std::size_t worker_index) {
    try {
        context_.run();
    } catch (...) {
        if (on_error_) {
            on_error_(worker_index, std::current_exception());
        } else {
            std::cerr << "AsioRuntime worker " << worker_index << " terminated by an exception\n";
        }
        requestStop();
    }
}

}  // namespace pulsegate::runtime
