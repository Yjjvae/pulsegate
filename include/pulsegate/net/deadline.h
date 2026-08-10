#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

#include "pulsegate/net/asio_types.h"

namespace pulsegate::net {

// All methods must run on one serialized executor. A generation ticket means
// a cancelled, old wait can never expire a newer socket operation.
class Deadline : public std::enable_shared_from_this<Deadline> {
   public:
    explicit Deadline(asio::any_io_executor executor);

    template <typename Rep, typename Period, typename OnExpire>
    void arm(std::chrono::duration<Rep, Period> timeout, OnExpire&& on_expire) {
        armImpl(std::chrono::duration_cast<std::chrono::steady_clock::duration>(timeout),
                std::function<void()>(std::forward<OnExpire>(on_expire)));
    }

    void disarm();

   private:
    void armImpl(std::chrono::steady_clock::duration timeout, std::function<void()> on_expire);

    asio::steady_timer timer_;
    std::uint64_t generation_{0};
};

}  // namespace pulsegate::net
