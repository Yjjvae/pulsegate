#pragma once

#include <string>

namespace pulsegate::http {

struct UpstreamEndpoint {
    std::string host;
    std::string service;
};

}  // namespace pulsegate::http
