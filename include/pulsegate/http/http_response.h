#pragma once

#include <string>

#include "pulsegate/http/headers.h"

namespace pulsegate::http {

struct HttpResponse {
    int status_code{200};
    std::string reason{"OK"};
    Headers headers;
    std::string body;
    bool close_connection{false};
    // Set only by a streaming handler after it has written through the
    // Session-owned RequestContext output callback.
    bool already_written{false};

    [[nodiscard]] std::string serialize(bool head_request) const;
};

}  // namespace pulsegate::http
