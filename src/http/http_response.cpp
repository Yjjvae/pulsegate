#include "pulsegate/http/http_response.h"

#include <stdexcept>

namespace pulsegate::http {

std::string HttpResponse::serialize(bool head_request) const {
    if (status_code < 100 || status_code > 999 || reason.empty() || !isValidHeaderValue(reason)) {
        throw std::invalid_argument("invalid HTTP response status line");
    }

    std::string response;
    response.reserve(96 + body.size());
    response.append("HTTP/1.1 ");
    response.append(std::to_string(status_code));
    response.push_back(' ');
    response.append(reason);
    response.append("\r\n");

    for (const auto& header : headers.entries()) {
        if (header.name == "content-length" || header.name == "connection") {
            continue;
        }
        if (!isValidHeaderName(header.name) || !isValidHeaderValue(header.value)) {
            throw std::invalid_argument("invalid HTTP response header");
        }
        response.append(header.name);
        response.append(": ");
        response.append(header.value);
        response.append("\r\n");
    }

    response.append("Content-Length: ");
    response.append(std::to_string(body.size()));
    response.append("\r\nConnection: ");
    response.append(close_connection ? "close" : "keep-alive");
    response.append("\r\n\r\n");
    if (!head_request) {
        response.append(body);
    }
    return response;
}

}  // namespace pulsegate::http
