#pragma once

#include <string_view>

namespace pulsegate::core {

inline constexpr std::string_view kProjectName{"PulseGate"};
inline constexpr std::string_view kVersion{"1.0.0-rc.1"};

[[nodiscard]] std::string_view version() noexcept;

}  // namespace pulsegate::core
