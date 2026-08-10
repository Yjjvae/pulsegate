#include "pulsegate/http/http_parser.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "pulsegate/http/http_response.h"

namespace {

using pulsegate::http::HttpMethod;
using pulsegate::http::HttpParser;
using pulsegate::http::HttpParserLimits;
using pulsegate::http::HttpResponse;
using pulsegate::http::ParseResult;
using pulsegate::net::Buffer;

constexpr std::string_view kValidGet = "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";

TEST(HttpParserTest, ParsesCompleteGetInOneInput) {
    Buffer input;
    HttpParser parser;
    input.append(kValidGet);

    ASSERT_EQ(parser.parse(input), ParseResult::Complete);
    const auto& request = parser.request();
    EXPECT_EQ(request.method, HttpMethod::Get);
    EXPECT_EQ(request.target, "/health");
    ASSERT_TRUE(request.headers.get("host").has_value());
    EXPECT_EQ(*request.headers.get("host"), "localhost");
    EXPECT_TRUE(request.keepAlive());
}

TEST(HttpParserTest, CompletesWhenFedOneByteAtATime) {
    Buffer input;
    HttpParser parser;
    ParseResult result = ParseResult::NeedMore;

    for (const char byte : kValidGet) {
        input.append(std::string_view(&byte, 1));
        result = parser.parse(input);
    }

    EXPECT_EQ(result, ParseResult::Complete);
    EXPECT_EQ(parser.request().target, "/health");
}

TEST(HttpParserTest, HandlesCrlfAcrossBufferWrites) {
    Buffer input(8, 128);
    HttpParser parser;
    input.append("GET / HTTP/1.1\r");
    EXPECT_EQ(parser.parse(input), ParseResult::NeedMore);
    input.append("\nHost: local\r");
    EXPECT_EQ(parser.parse(input), ParseResult::NeedMore);
    input.append("\n\r");
    EXPECT_EQ(parser.parse(input), ParseResult::NeedMore);
    input.append("\n");

    EXPECT_EQ(parser.parse(input), ParseResult::Complete);
}

TEST(HttpParserTest, NormalizesHeaderNamesForLookup) {
    Buffer input;
    HttpParser parser;
    input.append("GET / HTTP/1.1\r\nHoSt: localhost\r\nX-MiXeD: Value\r\n\r\n");

    ASSERT_EQ(parser.parse(input), ParseResult::Complete);
    ASSERT_TRUE(parser.request().headers.get("x-mixed").has_value());
    EXPECT_EQ(*parser.request().headers.get("X-MIXED"), "Value");
}

TEST(HttpParserTest, RejectsHttp11RequestWithoutHost) {
    Buffer input;
    HttpParser parser;
    input.append("GET / HTTP/1.1\r\n\r\n");

    EXPECT_EQ(parser.parse(input), ParseResult::BadRequest);
}

TEST(HttpParserTest, RejectsMalformedRequestLine) {
    Buffer input;
    HttpParser parser;
    input.append("GET /\r\nHost: localhost\r\n\r\n");

    EXPECT_EQ(parser.parse(input), ParseResult::BadRequest);
}

TEST(HttpParserTest, RejectsHeaderPastConfiguredLimit) {
    Buffer input;
    HttpParserLimits limits;
    limits.max_request_line_bytes = 32;
    limits.max_header_bytes = 48;
    limits.max_header_line_bytes = 32;
    HttpParser parser(limits);
    input.append("GET / HTTP/1.1\r\nHost: localhost\r\nX-Long: 12345678901234567890\r\n\r\n");

    EXPECT_EQ(parser.parse(input), ParseResult::HeaderTooLarge);
}

TEST(HttpParserTest, WaitsForTheEntireContentLengthBody) {
    Buffer input;
    HttpParser parser;
    input.append("POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nabc");

    EXPECT_EQ(parser.parse(input), ParseResult::NeedMore);
    input.append("de");
    ASSERT_EQ(parser.parse(input), ParseResult::Complete);
    EXPECT_EQ(parser.request().body, "abcde");
}

TEST(HttpParserTest, RejectsBodyOverConfiguredLimit) {
    Buffer input;
    HttpParserLimits limits;
    limits.max_body_bytes = 4;
    HttpParser parser(limits);
    input.append("POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nabcde");

    EXPECT_EQ(parser.parse(input), ParseResult::BodyTooLarge);
}

TEST(HttpParserTest, RejectsConflictingContentLength) {
    Buffer input;
    HttpParser parser;
    input.append(
        "POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1\r\n"
        "Content-Length: 2\r\n\r\na");

    EXPECT_EQ(parser.parse(input), ParseResult::BadRequest);
}

TEST(HttpParserTest, RejectsUnsupportedTransferEncoding) {
    Buffer input;
    HttpParser parser;
    input.append("POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n");

    EXPECT_EQ(parser.parse(input), ParseResult::UnsupportedTransferEncoding);
}

TEST(HttpParserTest, PreservesPipelinedBytesForTheNextRequest) {
    Buffer input;
    HttpParser parser;
    input.append(
        "GET /one HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /two HTTP/1.1\r\nHost: localhost\r\n\r\n");

    ASSERT_EQ(parser.parse(input), ParseResult::Complete);
    EXPECT_EQ(parser.takeRequest().target, "/one");
    parser.reset();
    ASSERT_EQ(parser.parse(input), ParseResult::Complete);
    EXPECT_EQ(parser.takeRequest().target, "/two");
    EXPECT_EQ(input.readableBytes(), 0U);
}

TEST(HttpParserTest, RejectsBareNewlineInHeaderValue) {
    Buffer input;
    HttpParser parser;
    input.append("GET / HTTP/1.1\r\nHost: localhost\r\nX-Test: good\nbad\r\n\r\n");

    EXPECT_EQ(parser.parse(input), ParseResult::BadRequest);
}

TEST(HttpParserTest, AppliesHttp10AndHttp11KeepAliveRules) {
    Buffer input;
    HttpParser parser;
    input.append("GET / HTTP/1.0\r\n\r\n");
    ASSERT_EQ(parser.parse(input), ParseResult::Complete);
    EXPECT_FALSE(parser.request().keepAlive());

    parser.takeRequest();
    parser.reset();
    input.append("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
    ASSERT_EQ(parser.parse(input), ParseResult::Complete);
    EXPECT_TRUE(parser.request().keepAlive());
}

TEST(HttpResponseTest, SerializesOwnedResponseAndSuppressesHeadBody) {
    HttpResponse response;
    response.headers.add("X-Request-Id", "abc");
    response.body = "hello";

    const auto get_bytes = response.serialize(false);
    EXPECT_NE(get_bytes.find("Content-Length: 5\r\n"), std::string::npos);
    EXPECT_NE(get_bytes.find("x-request-id: abc\r\n"), std::string::npos);
    EXPECT_TRUE(get_bytes.ends_with("\r\n\r\nhello"));

    const auto head_bytes = response.serialize(true);
    EXPECT_NE(head_bytes.find("Content-Length: 5\r\n"), std::string::npos);
    EXPECT_TRUE(head_bytes.ends_with("\r\n\r\n"));
}

TEST(HttpResponseTest, RejectsHeaderInjection) {
    HttpResponse response;
    response.headers.add("X-Test", "safe\r\nInjected: yes");

    EXPECT_THROW(static_cast<void>(response.serialize(false)), std::invalid_argument);
}

}  // namespace
