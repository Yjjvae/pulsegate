#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulsegate::net {

// Indexed byte storage for stream protocols. Bytes are moved only when the
// writable tail needs to be reclaimed or the allocation must grow.
class Buffer {
   public:
    explicit Buffer(std::size_t initial_size = 4096, std::size_t maximum_size = 64 * 1024);

    [[nodiscard]] std::size_t readableBytes() const noexcept;
    [[nodiscard]] const char* data() const noexcept;
    [[nodiscard]] std::string_view readableView() const noexcept;

    [[nodiscard]] std::span<char> prepare(std::size_t minimum);
    void commit(std::size_t count);

    void consume(std::size_t count);
    void clear() noexcept;
    void append(std::string_view bytes);

    [[nodiscard]] const char* findCrlf() const noexcept;
    [[nodiscard]] std::string takeString(std::size_t count);

   private:
    void compactOrGrow(std::size_t minimum);

    std::vector<char> storage_;
    std::size_t read_index_{0};
    std::size_t write_index_{0};
    std::size_t prepared_bytes_{0};
    std::size_t maximum_size_{0};
};

}  // namespace pulsegate::net
