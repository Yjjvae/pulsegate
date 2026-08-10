#pragma once

#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/thread_pool.hpp>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "pulsegate/net/asio_types.h"

namespace pulsegate::http {

enum class FileStatus { Ok, NotFound, Forbidden, TooLarge, Busy, InternalError };

struct FileResult {
    FileStatus status{FileStatus::InternalError};
    std::filesystem::path path;
    std::string body;
};

class FileService {
   public:
    virtual ~FileService() = default;

    virtual net::Awaitable<FileResult> read(net::asio::any_io_executor reply_executor,
                                            std::filesystem::path path,
                                            std::size_t maximum_bytes) = 0;
};

// A bounded pool for small regular files. It owns the canonical document root
// and verifies each resolved candidate remains below that root in the worker.
class BoundedFileService final : public FileService {
   public:
    BoundedFileService(std::filesystem::path document_root, std::size_t worker_count = 2,
                       std::size_t queue_capacity = 64);
    ~BoundedFileService() override;

    BoundedFileService(const BoundedFileService&) = delete;
    BoundedFileService& operator=(const BoundedFileService&) = delete;

    net::Awaitable<FileResult> read(net::asio::any_io_executor reply_executor,
                                    std::filesystem::path path, std::size_t maximum_bytes) override;

    [[nodiscard]] const std::filesystem::path& documentRoot() const noexcept;

   private:
    [[nodiscard]] FileResult readInWorker(const std::filesystem::path& path,
                                          std::size_t maximum_bytes) const;
    [[nodiscard]] bool tryAcquireSlot() noexcept;
    void releaseSlot() noexcept;

    std::filesystem::path root_;
    net::asio::thread_pool pool_;
    const std::size_t queue_capacity_;
    std::atomic_size_t pending_{0};
};

[[nodiscard]] std::optional<std::string> percentDecodePath(std::string_view value);
[[nodiscard]] std::string mimeTypeForPath(const std::filesystem::path& path);

}  // namespace pulsegate::http
