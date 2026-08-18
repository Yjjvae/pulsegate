#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace pulsegate::runtime {

using CoroutineErrorHandler =
    std::function<void(std::string_view operation, std::exception_ptr error)>;

// Every top-level coroutine has an exception sink. Network errors should be
// converted to error_code before reaching here; this is for programming and
// allocation failures that must not silently terminate the process.
template <typename Executor>
void spawnGuarded(const Executor& executor, std::string operation,
                  boost::asio::awaitable<void> operation_awaitable,
                  CoroutineErrorHandler on_error) {
    boost::asio::co_spawn(executor, std::move(operation_awaitable),
                          [operation = std::move(operation),
                           on_error = std::move(on_error)](std::exception_ptr error) mutable {
                              if (error && on_error) {
                                  on_error(operation, error);
                              }
                          });
}

}  // namespace pulsegate::runtime
