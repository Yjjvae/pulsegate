#pragma once

#include <cstddef>

#include "pulsegate/http/http_request.h"
#include "pulsegate/net/buffer.h"

namespace pulsegate::http {

struct HttpParserLimits {
    std::size_t max_request_line_bytes{8192};
    std::size_t max_header_bytes{16 * 1024};
    std::size_t max_header_count{100};
    std::size_t max_header_line_bytes{8192};
    std::size_t max_body_bytes{1024 * 1024};
};

enum class ParseState { RequestLine, Headers, Body, Complete, Error };

enum class ParseResult {
    NeedMore,
    Complete,
    BadRequest,
    HeaderTooLarge,
    BodyTooLarge,
    UnsupportedTransferEncoding
};

class HttpParser {
   public:
    explicit HttpParser(HttpParserLimits limits = {});

    ParseResult parse(net::Buffer& input);
    [[nodiscard]] const HttpRequest& request() const noexcept;
    [[nodiscard]] ParseState state() const noexcept;
    HttpRequest takeRequest();
    void reset();

   private:
    ParseResult parseRequestLine(std::string_view line);
    ParseResult parseHeader(std::string_view line);
    ParseResult finishHeaders();
    ParseResult fail(ParseResult result);

    HttpParserLimits limits_;
    ParseState state_{ParseState::RequestLine};
    HttpRequest request_;
    std::size_t expected_body_bytes_{0};
    std::size_t header_bytes_{0};
    std::size_t header_count_{0};
    ParseResult error_result_{ParseResult::BadRequest};
};

}  // namespace pulsegate::http
