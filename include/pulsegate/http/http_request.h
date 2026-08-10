#pragma once

#include <string>

#include "pulsegate/http/headers.h"

namespace pulsegate::http {

enum class HttpMethod { Get, Head, Post, Put, Delete, Options, Unknown };

struct HttpRequest {
    HttpMethod method{HttpMethod::Unknown};
    std::string target;
    int version_major{1};
    int version_minor{1};
    Headers headers;
    std::string body;

    [[nodiscard]] bool keepAlive() const;
};

[[nodiscard]] HttpMethod parseHttpMethod(std::string_view method) noexcept;

}  // namespace pulsegate::http
