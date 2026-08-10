#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace pulsegate::http {

class HttpSession;

using SessionId = std::uint64_t;

enum class SessionState { Created, Running, Draining, Closing, Closed };

enum class StopReason {
    None,
    PeerClosed,
    ProtocolError,
    HeaderTimeout,
    BodyTimeout,
    IdleTimeout,
    ServerShutdown,
    ResourceLimit,
    InternalError,
};

[[nodiscard]] const char* toString(StopReason reason) noexcept;

// The mutex makes registry operations safe for a future multi-threaded
// io_context. Entries are weak to avoid retaining finished sessions forever.
class SessionRegistry {
   public:
    explicit SessionRegistry(std::size_t maximum_sessions);

    bool tryAdd(SessionId id, const std::shared_ptr<HttpSession>& session);
    void recordRejected(StopReason reason);
    void remove(SessionId id, StopReason reason);
    void beginDrain();
    void forceCloseAll();

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t closedCount(StopReason reason) const;

   private:
    [[nodiscard]] std::vector<std::shared_ptr<HttpSession>> liveSessions();
    void pruneExpiredLocked();

    const std::size_t maximum_sessions_;
    mutable std::mutex mutex_;
    std::unordered_map<SessionId, std::weak_ptr<HttpSession>> sessions_;
    std::unordered_map<StopReason, std::size_t> closed_counts_;
    bool draining_{false};
};

}  // namespace pulsegate::http
