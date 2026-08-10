#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

namespace pulsegate::net {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using ErrorCode = boost::system::error_code;

template <typename T = void>
using Awaitable = asio::awaitable<T>;

inline constexpr auto use_awaitable = asio::use_awaitable;

}  // namespace pulsegate::net
