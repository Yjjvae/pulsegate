#pragma once

#include <string_view>

namespace pulsegate::core {

inline constexpr std::string_view kProjectName{"PulseGate"};
inline constexpr std::string_view kVersion{"0.0.1"};

[[nodiscard]] std::string_view version() noexcept;

}  // namespace pulsegate::core
