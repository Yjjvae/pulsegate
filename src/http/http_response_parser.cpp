#include "pulsegate/http/http_response_parser.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <stdexcept>

namespace pulsegate::http {
namespace {

constexpr std::size_t kAbsoluteHeaderLimit = 64 * 1024;
constexpr std::size_t kAbsoluteBodyLimit = 16 * 1024 * 1024;

std::string_view trim(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    return value;
}

bool noBodyStatus(int status) noexcept {
    return status / 100 == 1 || status == 204 || status == 304;
}

}  // namespace

HttpResponseParser::HttpResponseParser(HttpResponseParserLimits limits, bool head_request)
    : limits_(limits), head_request_(head_request) {
    if (limits_.max_status_line_bytes == 0 || limits_.max_header_bytes == 0 ||
        limits_.max_header_count == 0 || limits_.max_body_bytes == 0 ||
        limits_.max_header_bytes > kAbsoluteHeaderLimit ||
        limits_.max_body_bytes > kAbsoluteBodyLimit) {
        throw std::invalid_argument("HTTP response parser limits are outside safe bounds");
    }
}

ResponseParseResult HttpResponseParser::parse(net::Buffer& input) {
    if (state_ == ResponseParseState::Complete) return ResponseParseResult::Complete;
    if (state_ == ResponseParseState::Error) return error_result_;
    for (;;) {
        if (state_ == ResponseParseState::FixedBody) {
            const auto remaining = expected_body_bytes_ - response_.body.size();
            const auto count = std::min(remaining, input.readableBytes());
            if (count != 0) {
                const auto result = appendBody(input.readableView().substr(0, count));
                if (result != ResponseParseResult::Complete) return fail(result);
                input.consume(count);
            }
            if (response_.body.size() == expected_body_bytes_) {
                state_ = ResponseParseState::Complete;
                return ResponseParseResult::Complete;
            }
            return ResponseParseResult::NeedMore;
        }
        if (state_ == ResponseParseState::ChunkBody) {
            if (input.readableBytes() < chunk_bytes_ + 2) return ResponseParseResult::NeedMore;
            const auto bytes = input.readableView();
            if (bytes[chunk_bytes_] != '\r' || bytes[chunk_bytes_ + 1] != '\n')
                return fail(ResponseParseResult::BadResponse);
            const auto result = appendBody(bytes.substr(0, chunk_bytes_));
            if (result != ResponseParseResult::Complete) return fail(result);
            input.consume(chunk_bytes_ + 2);
            state_ = ResponseParseState::ChunkSize;
            continue;
        }
        if (state_ == ResponseParseState::EofBody) {
            if (input.readableBytes() != 0) {
                const auto result = appendBody(input.readableView());
                if (result != ResponseParseResult::Complete) return fail(result);
                input.consume(input.readableBytes());
            }
            return ResponseParseResult::NeedMore;
        }

        const char* crlf = input.findCrlf();
        if (crlf == nullptr) {
            if ((state_ == ResponseParseState::StatusLine &&
                 input.readableBytes() > limits_.max_status_line_bytes) ||
                ((state_ == ResponseParseState::Headers ||
                  state_ == ResponseParseState::Trailers) &&
                 header_bytes_ + input.readableBytes() > limits_.max_header_bytes))
                return fail(ResponseParseResult::HeaderTooLarge);
            return ResponseParseResult::NeedMore;
        }
        const auto line_size = static_cast<std::size_t>(crlf - input.data());
        const auto line = input.readableView().substr(0, line_size);
        input.consume(line_size + 2);
        if (state_ == ResponseParseState::StatusLine) {
            if (line_size > limits_.max_status_line_bytes)
                return fail(ResponseParseResult::HeaderTooLarge);
            const auto result = parseStatusLine(line);
            if (result != ResponseParseResult::Complete) return fail(result);
            state_ = ResponseParseState::Headers;
            header_bytes_ = line_size + 2;
            continue;
        }
        if (state_ == ResponseParseState::ChunkSize) {
            const auto extension = line.find(';');
            auto value = line.substr(0, extension);
            if (value.empty() || value.size() > 16) return fail(ResponseParseResult::BadResponse);
            std::size_t parsed{};
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), parsed, 16);
            if (error != std::errc{} || end != value.data() + value.size())
                return fail(ResponseParseResult::BadResponse);
            chunk_bytes_ = parsed;
            state_ = parsed == 0 ? ResponseParseState::Trailers : ResponseParseState::ChunkBody;
            continue;
        }
        header_bytes_ += line_size + 2;
        if (header_bytes_ > limits_.max_header_bytes)
            return fail(ResponseParseResult::HeaderTooLarge);
        if (state_ == ResponseParseState::Trailers) {
            if (line.empty()) {
                state_ = ResponseParseState::Complete;
                return ResponseParseResult::Complete;
            }
            if (!isValidHeaderName(line.substr(0, line.find(':'))))
                return fail(ResponseParseResult::BadResponse);
            continue;
        }
        if (line.empty()) {
            const auto result = finishHeaders();
            if (result != ResponseParseResult::Complete) return fail(result);
            if (state_ == ResponseParseState::Complete) return ResponseParseResult::Complete;
            continue;
        }
        const auto result = parseHeader(line);
        if (result != ResponseParseResult::Complete) return fail(result);
    }
}

ResponseParseResult HttpResponseParser::finishOnEof() {
    if (state_ == ResponseParseState::EofBody) {
        state_ = ResponseParseState::Complete;
        return ResponseParseResult::Complete;
    }
    if (state_ == ResponseParseState::Complete) return ResponseParseResult::Complete;
    return fail(ResponseParseResult::PrematureEof);
}

bool HttpResponseParser::headerComplete() const noexcept {
    return state_ != ResponseParseState::StatusLine && state_ != ResponseParseState::Headers &&
           state_ != ResponseParseState::Error;
}
bool HttpResponseParser::messageComplete() const noexcept {
    return state_ == ResponseParseState::Complete;
}
bool HttpResponseParser::reusable() const noexcept {
    return messageComplete() && !eof_delimited_ && !response_.close_connection;
}
bool HttpResponseParser::chunked() const noexcept {
    return chunked_;
}
const HttpResponse& HttpResponseParser::response() const noexcept {
    return response_;
}
ResponseParseState HttpResponseParser::state() const noexcept {
    return state_;
}

HttpResponse HttpResponseParser::takeResponse() {
    if (!messageComplete()) throw std::logic_error("cannot take an incomplete HTTP response");
    return std::move(response_);
}

void HttpResponseParser::reset(bool head_request) {
    state_ = ResponseParseState::StatusLine;
    response_ = {};
    expected_body_bytes_ = 0;
    chunk_bytes_ = 0;
    header_bytes_ = 0;
    header_count_ = 0;
    head_request_ = head_request;
    chunked_ = false;
    eof_delimited_ = false;
    error_result_ = ResponseParseResult::BadResponse;
}

ResponseParseResult HttpResponseParser::parseStatusLine(std::string_view line) {
    if (!line.starts_with("HTTP/1.")) return ResponseParseResult::BadResponse;
    const auto first = line.find(' ');
    if (first == std::string_view::npos || line.size() < first + 5)
        return ResponseParseResult::BadResponse;
    int status{};
    const auto status_text = line.substr(first + 1, 3);
    const auto [end, error] =
        std::from_chars(status_text.data(), status_text.data() + status_text.size(), status);
    if (error != std::errc{} || end != status_text.data() + status_text.size() ||
        line[first + 4] != ' ')
        return ResponseParseResult::BadResponse;
    response_.status_code = status;
    response_.reason = std::string(line.substr(first + 5));
    return response_.reason.empty() || !isValidHeaderValue(response_.reason)
               ? ResponseParseResult::BadResponse
               : ResponseParseResult::Complete;
}

ResponseParseResult HttpResponseParser::parseHeader(std::string_view line) {
    if (++header_count_ > limits_.max_header_count) return ResponseParseResult::HeaderTooLarge;
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) return ResponseParseResult::BadResponse;
    const auto name = line.substr(0, colon);
    const auto value = trim(line.substr(colon + 1));
    if (!isValidHeaderName(name) || !isValidHeaderValue(value))
        return ResponseParseResult::BadResponse;
    response_.headers.add(std::string(name), std::string(value));
    return ResponseParseResult::Complete;
}

ResponseParseResult HttpResponseParser::finishHeaders() {
    const auto transfers = response_.headers.values("transfer-encoding");
    const auto lengths = response_.headers.values("content-length");
    if (!transfers.empty() && !lengths.empty()) return ResponseParseResult::BadResponse;
    if (transfers.size() > 1 ||
        (!transfers.empty() && !containsToken(transfers.front(), "chunked")))
        return ResponseParseResult::UnsupportedTransferEncoding;
    // 100/102/103 are informational responses before the actual final
    // response. 101 would switch protocols, which this HTTP proxy rejects.
    if (response_.status_code / 100 == 1 && response_.status_code != 101) {
        response_ = {};
        expected_body_bytes_ = 0;
        header_count_ = 0;
        header_bytes_ = 0;
        state_ = ResponseParseState::StatusLine;
        return ResponseParseResult::Complete;
    }
    if (response_.status_code == 101) {
        return ResponseParseResult::UnsupportedTransferEncoding;
    }
    if (head_request_ || noBodyStatus(response_.status_code)) {
        state_ = ResponseParseState::Complete;
        return ResponseParseResult::Complete;
    }
    if (!transfers.empty()) {
        chunked_ = true;
        state_ = ResponseParseState::ChunkSize;
        return ResponseParseResult::Complete;
    }
    if (lengths.empty()) {
        eof_delimited_ = true;
        response_.close_connection = true;
        state_ = ResponseParseState::EofBody;
        return ResponseParseResult::Complete;
    }
    if (lengths.size() != 1 || lengths.front().empty()) return ResponseParseResult::BadResponse;
    std::size_t length{};
    const auto value = lengths.front();
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), length);
    if (error != std::errc{} || end != value.data() + value.size())
        return ResponseParseResult::BadResponse;
    if (length > limits_.max_body_bytes) return ResponseParseResult::BodyTooLarge;
    expected_body_bytes_ = length;
    state_ = length == 0 ? ResponseParseState::Complete : ResponseParseState::FixedBody;
    return ResponseParseResult::Complete;
}

ResponseParseResult HttpResponseParser::appendBody(std::string_view bytes) {
    if (bytes.size() > limits_.max_body_bytes - response_.body.size())
        return ResponseParseResult::BodyTooLarge;
    response_.body.append(bytes);
    return ResponseParseResult::Complete;
}

ResponseParseResult HttpResponseParser::fail(ResponseParseResult result) {
    state_ = ResponseParseState::Error;
    error_result_ = result;
    return result;
}

}  // namespace pulsegate::http
