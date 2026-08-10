#include "pulsegate/net/buffer.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace {

using pulsegate::net::Buffer;

TEST(BufferTest, AppendsConsumesAndKeepsUnreadBytes) {
    Buffer buffer(8, 32);
    buffer.append("first-second");

    EXPECT_EQ(buffer.takeString(6), "first-");
    EXPECT_EQ(buffer.readableView(), "second");

    buffer.append("-third");
    EXPECT_EQ(buffer.readableView(), "second-third");
}

TEST(BufferTest, PreparesWritableMemoryForAsioStyleReads) {
    Buffer buffer(8, 32);
    auto writable = buffer.prepare(4);
    ASSERT_GE(writable.size(), 4U);
    std::memcpy(writable.data(), "ping", 4);
    buffer.commit(4);

    EXPECT_EQ(buffer.readableView(), "ping");
}

TEST(BufferTest, FindsCrlfWithoutConsumingInput) {
    Buffer buffer;
    buffer.append("one\r\ntwo");

    const auto* crlf = buffer.findCrlf();
    ASSERT_NE(crlf, nullptr);
    EXPECT_EQ(std::string_view(buffer.data(), static_cast<std::size_t>(crlf - buffer.data())),
              "one");
    EXPECT_EQ(buffer.readableView(), "one\r\ntwo");
}

TEST(BufferTest, EnforcesConfiguredMaximumCapacity) {
    Buffer buffer(4, 8);
    buffer.append("12345678");

    EXPECT_THROW(buffer.append("9"), std::length_error);
}

TEST(BufferTest, RejectsInvalidCommitAndConsumeCounts) {
    Buffer buffer;
    const auto writable = buffer.prepare(4);

    EXPECT_THROW(buffer.commit(writable.size() + 1), std::out_of_range);
    buffer.commit(0);
    EXPECT_THROW(buffer.consume(1), std::out_of_range);
}

}  // namespace
