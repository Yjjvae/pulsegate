#include "pulsegate/http/http_parser.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace pulsegate::http {
namespace {

constexpr std::size_t kAbsoluteRequestLineLimit = 16 * 1024;
constexpr std::size_t kAbsoluteHeaderLimit = 64 * 1024;
constexpr std::size_t kAbsoluteHeaderCountLimit = 256;
constexpr std::size_t kAbsoluteBodyLimit = 16 * 1024 * 1024;

std::string_view trimOptionalWhitespace(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

bool isRequestTargetValid(std::string_view target) noexcept {
    return !target.empty() && std::none_of(target.begin(), target.end(), [](char character) {
        const auto value = static_cast<unsigned char>(character);
        return value <= 0x20U || value == 0x7FU;
    });
}

bool isMethodTokenValid(std::string_view method) noexcept {
    return isValidHeaderName(method);
}

}  // namespace

HttpParser::HttpParser(HttpParserLimits limits) : limits_(limits) {
    if (limits_.max_request_line_bytes == 0 || limits_.max_header_bytes == 0 ||
        limits_.max_header_count == 0 || limits_.max_header_line_bytes == 0 ||
        limits_.max_body_bytes == 0 || limits_.max_request_line_bytes > kAbsoluteRequestLineLimit ||
        limits_.max_header_bytes > kAbsoluteHeaderLimit ||
        limits_.max_header_count > kAbsoluteHeaderCountLimit ||
        limits_.max_body_bytes > kAbsoluteBodyLimit) {
        throw std::invalid_argument("HTTP parser limits are outside safe bounds");
    }
}

ParseResult HttpParser::parse(net::Buffer& input) {
    if (state_ == ParseState::Complete) {
        return ParseResult::Complete;
    }
    if (state_ == ParseState::Error) {
        return error_result_;
    }

    for (;;) {
        if (state_ == ParseState::Body) {
            if (input.readableBytes() < expected_body_bytes_) {
                return ParseResult::NeedMore;
            }
            request_.body = input.takeString(expected_body_bytes_);
            state_ = ParseState::Complete;
            return ParseResult::Complete;
        }

        const char* crlf = input.findCrlf();
        if (crlf == nullptr) {
            const auto pending = input.readableBytes();
            if (state_ == ParseState::RequestLine && pending > limits_.max_request_line_bytes) {
                return fail(ParseResult::HeaderTooLarge);
            }
            if (state_ == ParseState::Headers &&
                header_bytes_ + pending > limits_.max_header_bytes) {
                return fail(ParseResult::HeaderTooLarge);
            }
            return ParseResult::NeedMore;
        }

        const auto line_bytes = static_cast<std::size_t>(crlf - input.data());
        const auto line_with_terminator = line_bytes + 2;
        if (state_ == ParseState::RequestLine &&
            (line_bytes > limits_.max_request_line_bytes ||
             line_with_terminator > limits_.max_header_bytes)) {
            return fail(ParseResult::HeaderTooLarge);
        }
        if (state_ == ParseState::Headers &&
            (line_bytes > limits_.max_header_line_bytes ||
             line_with_terminator > limits_.max_header_bytes ||
             header_bytes_ > limits_.max_header_bytes - line_with_terminator)) {
            return fail(ParseResult::HeaderTooLarge);
        }

        const std::string_view line(input.data(), line_bytes);
        input.consume(line_bytes + 2);
        if (state_ == ParseState::RequestLine) {
            const auto result = parseRequestLine(line);
            if (result != ParseResult::Complete) {
                return fail(result);
            }
            state_ = ParseState::Headers;
            header_bytes_ = line_with_terminator;
            continue;
        }

        header_bytes_ += line_with_terminator;
        if (line.empty()) {
            const auto result = finishHeaders();
            if (result != ParseResult::Complete) {
                return fail(result);
            }
            if (expected_body_bytes_ == 0) {
                state_ = ParseState::Complete;
                return ParseResult::Complete;
            }
            state_ = ParseState::Body;
            continue;
        }

        const auto result = parseHeader(line);
        if (result != ParseResult::Complete) {
            return fail(result);
        }
    }
}

const HttpRequest& HttpParser::request() const noexcept {
    return request_;
}

ParseState HttpParser::state() const noexcept {
    return state_;
}

HttpRequest HttpParser::takeRequest() {
    if (state_ != ParseState::Complete) {
        throw std::logic_error("cannot take an incomplete HTTP request");
    }
    return std::move(request_);
}

void HttpParser::reset() {
    state_ = ParseState::RequestLine;
    request_ = {};
    expected_body_bytes_ = 0;
    header_bytes_ = 0;
    header_count_ = 0;
    error_result_ = ParseResult::BadRequest;
}

ParseResult HttpParser::parseRequestLine(std::string_view line) {
    const auto first_space = line.find(' ');
    if (first_space == std::string_view::npos) {
        return ParseResult::BadRequest;
    }
    const auto second_space = line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos ||
        line.find(' ', second_space + 1) != std::string_view::npos) {
        return ParseResult::BadRequest;
    }

    const auto method = line.substr(0, first_space);
    const auto target = line.substr(first_space + 1, second_space - first_space - 1);
    const auto version = line.substr(second_space + 1);
    if (!isMethodTokenValid(method) || !isRequestTargetValid(target)) {
        return ParseResult::BadRequest;
    }
    if (version != "HTTP/1.1" && version != "HTTP/1.0") {
        return ParseResult::BadRequest;
    }

    request_.method = parseHttpMethod(method);
    request_.target = target;
    request_.version_major = 1;
    request_.version_minor = version.back() - '0';
    return ParseResult::Complete;
}

ParseResult HttpParser::parseHeader(std::string_view line) {
    if (++header_count_ > limits_.max_header_count) {
        return ParseResult::HeaderTooLarge;
    }
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) {
        return ParseResult::BadRequest;
    }

    const auto name = line.substr(0, colon);
    const auto value = trimOptionalWhitespace(line.substr(colon + 1));
    if (!isValidHeaderName(name) || !isValidHeaderValue(value)) {
        return ParseResult::BadRequest;
    }
    request_.headers.add(std::string(name), std::string(value));
    return ParseResult::Complete;
}

ParseResult HttpParser::finishHeaders() {
    if (request_.version_major == 1 && request_.version_minor == 1 &&
        !request_.headers.contains("host")) {
        return ParseResult::BadRequest;
    }
    if (request_.headers.contains("transfer-encoding")) {
        return ParseResult::UnsupportedTransferEncoding;
    }

    const auto content_lengths = request_.headers.values("content-length");
    if (content_lengths.empty()) {
        expected_body_bytes_ = 0;
        return ParseResult::Complete;
    }
    if (content_lengths.size() != 1) {
        return ParseResult::BadRequest;
    }

    const auto value = content_lengths.front();
    if (value.empty()) {
        return ParseResult::BadRequest;
    }
    std::size_t parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size()) {
        return ParseResult::BadRequest;
    }
    if (parsed > limits_.max_body_bytes) {
        return ParseResult::BodyTooLarge;
    }
    expected_body_bytes_ = parsed;
    return ParseResult::Complete;
}

ParseResult HttpParser::fail(ParseResult result) {
    state_ = ParseState::Error;
    error_result_ = result;
    return result;
}

}  // namespace pulsegate::http
