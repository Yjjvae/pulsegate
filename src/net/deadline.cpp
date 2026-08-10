#include "pulsegate/net/deadline.h"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/error.hpp>

namespace pulsegate::net {

Deadline::Deadline(asio::any_io_executor executor) : timer_(std::move(executor)) {}

void Deadline::armImpl(std::chrono::steady_clock::duration timeout,
                       std::function<void()> on_expire) {
    const auto ticket = ++generation_;
    timer_.expires_after(timeout);
    timer_.async_wait([self = shared_from_this(), ticket,
                       on_expire = std::move(on_expire)](const ErrorCode& error) mutable {
        if (!error && ticket == self->generation_) {
            on_expire();
        }
    });
}

void Deadline::disarm() {
    ++generation_;
    timer_.cancel();
}

}  // namespace pulsegate::net
