#pragma once

#include <type_traits>
#include <utility>

namespace pulsegate::runtime {

// Keeps asynchronous-operation cleanup on every coroutine exit path.
template <typename Function>
class ScopeExit {
   public:
    explicit ScopeExit(Function&& function) : function_(std::forward<Function>(function)) {}

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

    ScopeExit(ScopeExit&& other) noexcept(std::is_nothrow_move_constructible_v<Function>)
        : function_(std::move(other.function_)), active_(std::exchange(other.active_, false)) {}

    ~ScopeExit() {
        if (active_) {
            function_();
        }
    }

   private:
    Function function_;
    bool active_{true};
};

template <typename Function>
[[nodiscard]] ScopeExit<std::decay_t<Function>> makeScopeExit(Function&& function) {
    return ScopeExit<std::decay_t<Function>>(std::forward<Function>(function));
}

}  // namespace pulsegate::runtime
