#include "pulsegate/http/http_request.h"

namespace pulsegate::http {

HttpMethod parseHttpMethod(std::string_view method) noexcept {
    if (method == "GET") {
        return HttpMethod::Get;
    }
    if (method == "HEAD") {
        return HttpMethod::Head;
    }
    if (method == "POST") {
        return HttpMethod::Post;
    }
    if (method == "PUT") {
        return HttpMethod::Put;
    }
    if (method == "DELETE") {
        return HttpMethod::Delete;
    }
    if (method == "OPTIONS") {
        return HttpMethod::Options;
    }
    return HttpMethod::Unknown;
}

bool HttpRequest::keepAlive() const {
    const auto connection = headers.values("connection");
    for (const auto value : connection) {
        if (containsToken(value, "close")) {
            return false;
        }
    }

    if (version_major == 1 && version_minor == 0) {
        for (const auto value : connection) {
            if (containsToken(value, "keep-alive")) {
                return true;
            }
        }
        return false;
    }
    return true;
}

}  // namespace pulsegate::http
