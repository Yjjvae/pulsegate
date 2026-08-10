#pragma once

#include <algorithm>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <vector>

#include "pulsegate/http/upstream_connection.h"
#include "pulsegate/net/asio_types.h"

namespace pulsegate::http {

class UpstreamPool;
class ProxySession;

enum class DiscardReason { ProtocolError, Timeout, Cancelled, LeaseAbandoned, PoolStopping };

struct PoolLimits {
    std::size_t max_connections{16};
    std::size_t max_idle_connections{8};
    std::size_t max_reuses{100};
    std::size_t max_waiters{64};
    std::chrono::milliseconds acquire_timeout{std::chrono::seconds(1)};
    std::chrono::milliseconds idle_timeout{std::chrono::minutes(1)};
    std::chrono::milliseconds max_lifetime{std::chrono::minutes(5)};
};

// Socket ownership stays private to the pool implementation. This record is
// intentionally small so policy can be unit-tested without opening TCP ports.
struct UpstreamConnectionRecord {
    std::uint64_t id{0};
    std::size_t reuse_count{0};
    std::chrono::steady_clock::time_point created_at{};
    std::chrono::steady_clock::time_point last_used_at{};
    bool open{true};
    std::shared_ptr<UpstreamConnection> transport;
};

class UpstreamLease {
   public:
    UpstreamLease() = default;
    UpstreamLease(const UpstreamLease&) = delete;
    UpstreamLease& operator=(const UpstreamLease&) = delete;
    UpstreamLease(UpstreamLease&& other) noexcept;
    UpstreamLease& operator=(UpstreamLease&& other) noexcept;
    ~UpstreamLease();

    [[nodiscard]] bool valid() const noexcept {
        return static_cast<bool>(connection_);
    }
    [[nodiscard]] std::uint64_t connectionId() const noexcept {
        return connection_ ? connection_->id : 0;
    }

   private:
    friend class UpstreamPoolPolicy;
    friend class UpstreamPool;
    friend class ProxySession;
    explicit UpstreamLease(std::shared_ptr<UpstreamConnectionRecord> connection)
        : connection_(std::move(connection)) {}
    void attachOwner(std::weak_ptr<UpstreamPool> owner) noexcept;
    void markReturned() noexcept;
    void abandon() noexcept;
    std::shared_ptr<UpstreamConnectionRecord> connection_;
    std::weak_ptr<UpstreamPool> owner_;
    bool explicitly_returned_{false};
};

// The policy half of UpstreamPool. Its methods must execute on the eventual
// Pool strand; separating policy lets tests prove reuse/discard conditions.
class UpstreamPoolPolicy {
   public:
    explicit UpstreamPoolPolicy(PoolLimits limits = {});

    [[nodiscard]] UpstreamLease acquire();
    void releaseReusable(UpstreamLease lease);
    void discard(UpstreamLease lease, DiscardReason reason);
    void pruneIdle();
    [[nodiscard]] std::size_t idleCount() const noexcept;
    [[nodiscard]] std::size_t busyCount() const noexcept;
    [[nodiscard]] std::size_t createdCount() const noexcept;

   private:
    [[nodiscard]] bool reusable(const UpstreamConnectionRecord& connection) const noexcept;
    PoolLimits limits_;
    std::vector<std::shared_ptr<UpstreamConnectionRecord>> idle_;
    std::size_t busy_{0};
    std::uint64_t next_id_{1};
};

class UpstreamPool : public std::enable_shared_from_this<UpstreamPool> {
   public:
    UpstreamPool(net::asio::any_io_executor executor, PoolLimits limits = {});
    UpstreamPool(net::asio::any_io_executor executor, UpstreamEndpoint endpoint,
                 PoolLimits limits = {});

    template <typename CompletionToken>
    auto asyncAcquire(CompletionToken&& token) {
        return net::asio::async_initiate<CompletionToken, void(net::ErrorCode, UpstreamLease)>(
            [self = shared_from_this()](auto completion_handler) {
                using Handler = std::decay_t<decltype(completion_handler)>;
                auto handler = std::make_shared<Handler>(std::move(completion_handler));
                const auto reply_executor = net::asio::get_associated_executor(*handler);
                net::asio::post(self->strand_, [self, handler, reply_executor]() mutable {
                    self->scheduleCleanup();
                    if (self->stopping_) {
                        net::asio::post(reply_executor, [handler]() mutable {
                            (*handler)(net::ErrorCode(net::asio::error::operation_aborted),
                                       UpstreamLease{});
                        });
                        return;
                    }
                    auto acquired_lease = self->policy_.acquire();
                    if (acquired_lease.valid()) {
                        self->attachTransport(acquired_lease);
                        acquired_lease.attachOwner(self);
                        net::asio::post(reply_executor,
                                        [handler, lease = std::move(acquired_lease)]() mutable {
                                            (*handler)(net::ErrorCode{}, std::move(lease));
                                        });
                        return;
                    }
                    if (self->waiters_.size() >= self->limits_.max_waiters) {
                        net::asio::post(reply_executor, [handler]() mutable {
                            (*handler)(net::ErrorCode(net::asio::error::no_buffer_space),
                                       UpstreamLease{});
                        });
                        return;
                    }
                    auto waiter = std::make_shared<Waiter>();
                    waiter->complete = [handler, reply_executor](
                                           net::ErrorCode error,
                                           UpstreamLease completed_lease) mutable {
                        net::asio::post(
                            reply_executor,
                            [handler, error, lease = std::move(completed_lease)]() mutable {
                                (*handler)(error, std::move(lease));
                            });
                    };
                    waiter->timer = std::make_unique<net::asio::steady_timer>(self->strand_);
                    waiter->timer->expires_after(self->limits_.acquire_timeout);
                    waiter->timer->async_wait([weak = std::weak_ptr<UpstreamPool>(self),
                                               waiter](const net::ErrorCode& error) {
                        if (!error) {
                            if (auto pool = weak.lock()) {
                                pool->completeWaiter(waiter,
                                                     net::ErrorCode(net::asio::error::timed_out),
                                                     UpstreamLease{});
                            }
                        }
                    });
                    self->waiters_.push_back(std::move(waiter));
                });
            },
            token);
    }

    void releaseReusable(UpstreamLease lease);
    void discard(UpstreamLease lease, DiscardReason reason);
    void stop();

   private:
    friend class UpstreamLease;
    struct Waiter {
        std::function<void(net::ErrorCode, UpstreamLease)> complete;
        std::unique_ptr<net::asio::steady_timer> timer;
        bool done{false};
    };

    void completeWaiter(const std::shared_ptr<Waiter>& waiter, net::ErrorCode error,
                        UpstreamLease lease);
    void satisfyOneWaiter();
    void discardAbandoned(std::shared_ptr<UpstreamConnectionRecord> connection);
    void scheduleCleanup();
    void attachTransport(UpstreamLease& lease);
    net::asio::strand<net::asio::any_io_executor> strand_;
    UpstreamPoolPolicy policy_;
    PoolLimits limits_;
    std::optional<UpstreamEndpoint> endpoint_;
    net::asio::steady_timer cleanup_timer_;
    std::deque<std::shared_ptr<Waiter>> waiters_;
    bool stopping_{false};
    bool cleanup_scheduled_{false};
};

}  // namespace pulsegate::http
