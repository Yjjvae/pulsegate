#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string_view>

#include "pulsegate/http/http_parser.h"
#include "pulsegate/net/buffer.h"

namespace {

constexpr std::size_t kMaximumInputBytes = 64U * 1024U;

void assertCompleteState(pulsegate::http::HttpParser& parser, pulsegate::http::ParseResult result) {
    if (result != pulsegate::http::ParseResult::Complete) return;
    if (parser.state() != pulsegate::http::ParseState::Complete) {
        __builtin_trap();
    }
    static_cast<void>(parser.takeRequest());
}

void parseWholeBuffer(std::string_view input) {
    pulsegate::net::Buffer buffer(4096, kMaximumInputBytes);
    buffer.append(input);
    pulsegate::http::HttpParser parser;
    assertCompleteState(parser, parser.parse(buffer));
}

void parseInDeterministicChunks(std::string_view input) {
    pulsegate::net::Buffer buffer(4096, kMaximumInputBytes);
    pulsegate::http::HttpParser parser;
    for (std::size_t offset = 0; offset < input.size();) {
        const auto chunk_size = std::min<std::size_t>(1U + (offset % 31U), input.size() - offset);
        buffer.append(input.substr(offset, chunk_size));
        const auto result = parser.parse(buffer);
        if (result == pulsegate::http::ParseResult::Complete) {
            assertCompleteState(parser, result);
            return;
        }
        offset += chunk_size;
    }
    assertCompleteState(parser, parser.parse(buffer));
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // Buffer and HttpParser both impose independent hard limits. Inputs larger
    // than either limit are intentionally uninteresting for this parser fuzz
    // target: they exercise allocation failure, not HTTP state transitions.
    if (size > kMaximumInputBytes) return 0;

    try {
        const std::string_view input(reinterpret_cast<const char*>(data), size);
        parseWholeBuffer(input);
        parseInDeterministicChunks(input);
    } catch (const std::exception&) {
        // Malformed data may legitimately violate bounded Buffer/Parser input
        // contracts. The fuzzer is checking memory safety and termination.
    }
    return 0;
}
