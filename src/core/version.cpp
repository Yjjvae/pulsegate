#include "pulsegate/core/version.h"

namespace pulsegate::core {

std::string_view version() noexcept {
    return kVersion;
}

}  // namespace pulsegate::core
