#include "pulsegate/http/http_response_parser.h"

#include <gtest/gtest.h>

namespace pulsegate::http {
namespace {

TEST(HttpResponseParserTest, ParsesFragmentedContentLengthResponse) {
    net::Buffer input;
    HttpResponseParser parser;
    input.append("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nX-Test: yes\r\n\r\nhe");
    EXPECT_EQ(parser.parse(input), ResponseParseResult::NeedMore);
    input.append("lloNEXT");
    EXPECT_EQ(parser.parse(input), ResponseParseResult::Complete);
    const auto response = parser.takeResponse();
    EXPECT_EQ(response.status_code, 200);
    EXPECT_EQ(response.body, "hello");
    EXPECT_EQ(input.readableView(), "NEXT");
    EXPECT_TRUE(parser.reusable());
}

TEST(HttpResponseParserTest, DecodesChunkedResponseAndRejectsConflictingLength) {
    net::Buffer input;
    HttpResponseParser parser;
    input.append(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: "
        "chunked\r\n\r\n4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n");
    EXPECT_EQ(parser.parse(input), ResponseParseResult::Complete);
    EXPECT_EQ(parser.takeResponse().body, "Wikipedia");

    parser.reset();
    input.clear();
    input.append("HTTP/1.1 200 OK\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n");
    EXPECT_EQ(parser.parse(input), ResponseParseResult::BadResponse);
}

TEST(HttpResponseParserTest, HandlesHeadAndEofDelimitedBodies) {
    net::Buffer input;
    HttpResponseParser head_parser({}, true);
    input.append("HTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\n");
    EXPECT_EQ(head_parser.parse(input), ResponseParseResult::Complete);
    EXPECT_TRUE(head_parser.takeResponse().body.empty());

    HttpResponseParser eof_parser;
    input.clear();
    input.append("HTTP/1.0 200 OK\r\n\r\nabc");
    EXPECT_EQ(eof_parser.parse(input), ResponseParseResult::NeedMore);
    EXPECT_EQ(eof_parser.finishOnEof(), ResponseParseResult::Complete);
    auto response = eof_parser.takeResponse();
    EXPECT_EQ(response.body, "abc");
    EXPECT_TRUE(response.close_connection);
}

TEST(HttpResponseParserTest, SkipsInformationalResponseBeforeFinalResponse) {
    net::Buffer input;
    HttpResponseParser parser;
    input.append(
        "HTTP/1.1 100 Continue\r\nX-Trace: upstream\r\n\r\n"
        "HTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok");
    EXPECT_EQ(parser.parse(input), ResponseParseResult::Complete);
    const auto response = parser.takeResponse();
    EXPECT_EQ(response.status_code, 201);
    EXPECT_EQ(response.body, "ok");
}

TEST(HttpResponseParserTest, RejectsPrematureEofAndBodyLimit) {
    net::Buffer input;
    HttpResponseParser parser({.max_body_bytes = 3});
    input.append("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\n");
    EXPECT_EQ(parser.parse(input), ResponseParseResult::BodyTooLarge);
    parser.reset();
    input.clear();
    input.append("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\na");
    EXPECT_EQ(parser.parse(input), ResponseParseResult::NeedMore);
    EXPECT_EQ(parser.finishOnEof(), ResponseParseResult::PrematureEof);
}

}  // namespace
}  // namespace pulsegate::http
