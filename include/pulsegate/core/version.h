#pragma once

#include <string_view>

namespace pulsegate::core {

inline constexpr std::string_view kProjectName{"PulseGate"};
inline constexpr std::string_view kVersion{"0.9.0"};

[[nodiscard]] std::string_view version() noexcept;

}  // namespace pulsegate::core
