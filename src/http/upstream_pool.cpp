#include "pulsegate/http/upstream_pool.h"

#include <stdexcept>
#include <utility>

namespace pulsegate::http {

UpstreamLease::UpstreamLease(UpstreamLease&& other) noexcept
    : connection_(std::move(other.connection_)),
      owner_(std::move(other.owner_)),
      explicitly_returned_(other.explicitly_returned_) {
    other.explicitly_returned_ = true;
}

UpstreamLease& UpstreamLease::operator=(UpstreamLease&& other) noexcept {
    if (this == &other) return *this;
    abandon();
    connection_ = std::move(other.connection_);
    owner_ = std::move(other.owner_);
    explicitly_returned_ = other.explicitly_returned_;
    other.explicitly_returned_ = true;
    return *this;
}

UpstreamLease::~UpstreamLease() {
    abandon();
}

void UpstreamLease::attachOwner(std::weak_ptr<UpstreamPool> owner) noexcept {
    owner_ = std::move(owner);
}

void UpstreamLease::markReturned() noexcept {
    explicitly_returned_ = true;
    owner_.reset();
}

void UpstreamLease::abandon() noexcept {
    if (!connection_ || explicitly_returned_) return;
    explicitly_returned_ = true;
    if (const auto owner = owner_.lock()) {
        owner->discardAbandoned(std::move(connection_));
    } else {
        connection_.reset();
    }
    owner_.reset();
}

UpstreamPoolPolicy::UpstreamPoolPolicy(PoolLimits limits) : limits_(limits) {
    if (limits_.max_connections == 0 || limits_.max_idle_connections == 0 ||
        limits_.max_idle_connections > limits_.max_connections || limits_.max_reuses == 0 ||
        limits_.idle_timeout <= std::chrono::milliseconds::zero() ||
        limits_.max_lifetime <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("upstream pool limits are invalid");
    }
}

UpstreamLease UpstreamPoolPolicy::acquire() {
    const auto now = std::chrono::steady_clock::now();
    while (!idle_.empty()) {
        auto connection = std::move(idle_.back());
        idle_.pop_back();
        if (reusable(*connection)) {
            connection->last_used_at = now;
            ++busy_;
            return UpstreamLease(std::move(connection));
        }
    }
    if (busy_ + idle_.size() >= limits_.max_connections) {
        return {};
    }
    auto connection =
        std::make_shared<UpstreamConnectionRecord>(UpstreamConnectionRecord{.id = next_id_++,
                                                                            .reuse_count = 0,
                                                                            .created_at = now,
                                                                            .last_used_at = now,
                                                                            .open = true,
                                                                            .transport = {}});
    ++busy_;
    return UpstreamLease(std::move(connection));
}

void UpstreamPoolPolicy::releaseReusable(UpstreamLease lease) {
    if (!lease.valid()) return;
    --busy_;
    auto connection = std::move(lease.connection_);
    ++connection->reuse_count;
    connection->last_used_at = std::chrono::steady_clock::now();
    if (reusable(*connection) && idle_.size() < limits_.max_idle_connections) {
        idle_.push_back(std::move(connection));
    }
}

void UpstreamPoolPolicy::discard(UpstreamLease lease, DiscardReason) {
    if (!lease.valid()) return;
    --busy_;
    lease.connection_->open = false;
}

void UpstreamPoolPolicy::pruneIdle() {
    std::erase_if(idle_, [this](const auto& connection) { return !reusable(*connection); });
}

std::size_t UpstreamPoolPolicy::idleCount() const noexcept {
    return idle_.size();
}
std::size_t UpstreamPoolPolicy::busyCount() const noexcept {
    return busy_;
}
std::size_t UpstreamPoolPolicy::createdCount() const noexcept {
    return next_id_ - 1;
}

bool UpstreamPoolPolicy::reusable(const UpstreamConnectionRecord& connection) const noexcept {
    return connection.open && connection.reuse_count < limits_.max_reuses &&
           std::chrono::steady_clock::now() - connection.created_at < limits_.max_lifetime &&
           std::chrono::steady_clock::now() - connection.last_used_at < limits_.idle_timeout;
}

UpstreamPool::UpstreamPool(net::asio::any_io_executor executor, PoolLimits limits)
    : strand_(net::asio::make_strand(std::move(executor))),
      policy_(limits),
      limits_(limits),
      cleanup_timer_(strand_) {}

UpstreamPool::UpstreamPool(net::asio::any_io_executor executor, UpstreamEndpoint endpoint,
                           PoolLimits limits)
    : strand_(net::asio::make_strand(std::move(executor))),
      policy_(limits),
      limits_(limits),
      endpoint_(std::move(endpoint)),
      cleanup_timer_(strand_) {}

void UpstreamPool::releaseReusable(UpstreamLease lease) {
    const auto self = shared_from_this();
    net::asio::post(strand_, [self, lease = std::move(lease)]() mutable {
        lease.markReturned();
        self->policy_.releaseReusable(std::move(lease));
        self->satisfyOneWaiter();
    });
}

void UpstreamPool::completeWaiter(const std::shared_ptr<Waiter>& waiter, net::ErrorCode error,
                                  UpstreamLease lease) {
    if (waiter->done) return;
    waiter->done = true;
    waiter->timer->cancel();
    std::erase(waiters_, waiter);
    waiter->complete(error, std::move(lease));
}

void UpstreamPool::satisfyOneWaiter() {
    while (!waiters_.empty()) {
        const auto waiter = waiters_.front();
        auto lease = policy_.acquire();
        if (!lease.valid()) return;
        attachTransport(lease);
        lease.attachOwner(weak_from_this());
        completeWaiter(waiter, {}, std::move(lease));
        return;
    }
}

void UpstreamPool::discard(UpstreamLease lease, DiscardReason reason) {
    const auto self = shared_from_this();
    net::asio::post(strand_, [self, lease = std::move(lease), reason]() mutable {
        if (lease.connection_ && lease.connection_->transport) {
            lease.connection_->transport->cancelAndClose();
        }
        lease.markReturned();
        self->policy_.discard(std::move(lease), reason);
    });
}

void UpstreamPool::discardAbandoned(std::shared_ptr<UpstreamConnectionRecord> connection) {
    const auto self = shared_from_this();
    net::asio::post(strand_, [self, connection = std::move(connection)]() mutable {
        if (connection->transport) connection->transport->cancelAndClose();
        self->policy_.discard(UpstreamLease(std::move(connection)), DiscardReason::LeaseAbandoned);
    });
}

void UpstreamPool::attachTransport(UpstreamLease& lease) {
    if (!endpoint_ || !lease.connection_ || lease.connection_->transport) return;
    lease.connection_->transport = std::make_shared<UpstreamConnection>(strand_, *endpoint_);
}

void UpstreamPool::stop() {
    const auto self = shared_from_this();
    net::asio::post(strand_, [self] {
        if (self->stopping_) return;
        self->stopping_ = true;
        self->cleanup_timer_.cancel();
        while (!self->waiters_.empty()) {
            self->completeWaiter(self->waiters_.front(),
                                 net::ErrorCode(net::asio::error::operation_aborted), {});
        }
    });
}

void UpstreamPool::scheduleCleanup() {
    if (stopping_ || cleanup_scheduled_) return;
    cleanup_scheduled_ = true;
    cleanup_timer_.expires_after(limits_.idle_timeout);
    cleanup_timer_.async_wait([weak = weak_from_this()](const net::ErrorCode& error) {
        if (error) return;
        if (const auto self = weak.lock()) {
            self->cleanup_scheduled_ = false;
            self->policy_.pruneIdle();
            self->scheduleCleanup();
        }
    });
}

}  // namespace pulsegate::http
