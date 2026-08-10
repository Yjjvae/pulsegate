#pragma once

#include <cstddef>

#include "pulsegate/http/http_response.h"
#include "pulsegate/net/buffer.h"

namespace pulsegate::http {

struct HttpResponseParserLimits {
    std::size_t max_status_line_bytes{8192};
    std::size_t max_header_bytes{16 * 1024};
    std::size_t max_header_count{100};
    std::size_t max_body_bytes{1024 * 1024};
};

enum class ResponseParseState {
    StatusLine,
    Headers,
    FixedBody,
    ChunkSize,
    ChunkBody,
    Trailers,
    EofBody,
    Complete,
    Error
};

enum class ResponseParseResult {
    NeedMore,
    Complete,
    BadResponse,
    HeaderTooLarge,
    BodyTooLarge,
    UnsupportedTransferEncoding,
    PrematureEof
};

// Incremental upstream response parser. It decodes chunked bodies into
// HttpResponse::body so downstream serialization always has a known length.
class HttpResponseParser {
   public:
    explicit HttpResponseParser(HttpResponseParserLimits limits = {}, bool head_request = false);

    ResponseParseResult parse(net::Buffer& input);
    // Call only after the upstream socket has reported EOF. EOF can delimit a
    // response body only when neither Content-Length nor chunked was supplied.
    ResponseParseResult finishOnEof();
    [[nodiscard]] bool headerComplete() const noexcept;
    [[nodiscard]] bool messageComplete() const noexcept;
    [[nodiscard]] bool reusable() const noexcept;
    [[nodiscard]] bool chunked() const noexcept;
    [[nodiscard]] const HttpResponse& response() const noexcept;
    [[nodiscard]] ResponseParseState state() const noexcept;
    HttpResponse takeResponse();
    void reset(bool head_request = false);

   private:
    ResponseParseResult parseStatusLine(std::string_view line);
    ResponseParseResult parseHeader(std::string_view line);
    ResponseParseResult finishHeaders();
    ResponseParseResult appendBody(std::string_view bytes);
    ResponseParseResult fail(ResponseParseResult result);

    HttpResponseParserLimits limits_;
    ResponseParseState state_{ResponseParseState::StatusLine};
    HttpResponse response_;
    std::size_t expected_body_bytes_{0};
    std::size_t chunk_bytes_{0};
    std::size_t header_bytes_{0};
    std::size_t header_count_{0};
    bool head_request_{false};
    bool chunked_{false};
    bool eof_delimited_{false};
    ResponseParseResult error_result_{ResponseParseResult::BadResponse};
};

}  // namespace pulsegate::http
