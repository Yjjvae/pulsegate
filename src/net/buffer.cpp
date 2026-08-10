#include "pulsegate/net/buffer.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace pulsegate::net {

Buffer::Buffer(std::size_t initial_size, std::size_t maximum_size) : maximum_size_(maximum_size) {
    if (initial_size == 0 || maximum_size == 0 || initial_size > maximum_size) {
        throw std::invalid_argument("buffer sizes must be non-zero and ordered");
    }
    storage_.resize(initial_size);
}

std::size_t Buffer::readableBytes() const noexcept {
    return write_index_ - read_index_;
}

const char* Buffer::data() const noexcept {
    return storage_.data() + read_index_;
}

std::string_view Buffer::readableView() const noexcept {
    return {data(), readableBytes()};
}

std::span<char> Buffer::prepare(std::size_t minimum) {
    if (prepared_bytes_ != 0) {
        throw std::logic_error("commit the previous prepared buffer before preparing again");
    }
    if (minimum == 0) {
        return {};
    }

    compactOrGrow(minimum);
    prepared_bytes_ = storage_.size() - write_index_;
    return {storage_.data() + write_index_, prepared_bytes_};
}

void Buffer::commit(std::size_t count) {
    if (count > prepared_bytes_) {
        throw std::out_of_range("buffer commit exceeds prepared writable region");
    }
    write_index_ += count;
    prepared_bytes_ = 0;
}

void Buffer::consume(std::size_t count) {
    if (prepared_bytes_ != 0) {
        throw std::logic_error("cannot consume while a writable region is prepared");
    }
    if (count > readableBytes()) {
        throw std::out_of_range("buffer consume exceeds readable bytes");
    }
    read_index_ += count;
    if (read_index_ == write_index_) {
        clear();
    }
}

void Buffer::clear() noexcept {
    read_index_ = 0;
    write_index_ = 0;
    prepared_bytes_ = 0;
}

void Buffer::append(std::string_view bytes) {
    while (!bytes.empty()) {
        auto writable = prepare(1);
        const auto count = std::min(writable.size(), bytes.size());
        std::memcpy(writable.data(), bytes.data(), count);
        commit(count);
        bytes.remove_prefix(count);
    }
}

const char* Buffer::findCrlf() const noexcept {
    constexpr char kCrlf[] = {'\r', '\n'};
    const auto begin = storage_.begin() + static_cast<std::ptrdiff_t>(read_index_);
    const auto end = storage_.begin() + static_cast<std::ptrdiff_t>(write_index_);
    const auto match = std::search(begin, end, std::begin(kCrlf), std::end(kCrlf));
    if (match == end) {
        return nullptr;
    }
    return storage_.data() + std::distance(storage_.begin(), match);
}

std::string Buffer::takeString(std::size_t count) {
    if (count > readableBytes()) {
        throw std::out_of_range("buffer take exceeds readable bytes");
    }
    std::string result(data(), count);
    consume(count);
    return result;
}

void Buffer::compactOrGrow(std::size_t minimum) {
    if (minimum > maximum_size_) {
        throw std::length_error("requested writable buffer exceeds maximum capacity");
    }
    if (storage_.size() - write_index_ >= minimum) {
        return;
    }

    const auto readable = readableBytes();
    if (storage_.size() - readable >= minimum) {
        std::memmove(storage_.data(), data(), readable);
        read_index_ = 0;
        write_index_ = readable;
        return;
    }

    if (readable > maximum_size_ - minimum) {
        throw std::length_error("buffer maximum capacity exceeded");
    }
    const auto required = readable + minimum;
    const auto doubled = storage_.size() > maximum_size_ / 2 ? maximum_size_ : storage_.size() * 2;
    const auto new_size = std::max(required, doubled);
    storage_.resize(new_size);
    if (read_index_ != 0) {
        std::memmove(storage_.data(), storage_.data() + read_index_, readable);
        read_index_ = 0;
        write_index_ = readable;
    }
}

}  // namespace pulsegate::net
