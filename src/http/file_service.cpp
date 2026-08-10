#include "pulsegate/http/file_service.h"

#include <algorithm>
#include <boost/asio/async_result.hpp>
#include <boost/asio/post.hpp>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace pulsegate::http {
namespace {

bool isBelow(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    std::error_code error;
    const auto relative = std::filesystem::relative(candidate, root, error);
    if (error || relative.empty() || relative.is_absolute()) {
        return false;
    }
    return std::none_of(relative.begin(), relative.end(),
                        [](const auto& component) { return component == ".."; });
}

FileResult failure(FileStatus status) {
    return {.status = status, .path = {}, .body = {}};
}

int hexValue(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

}  // namespace

BoundedFileService::BoundedFileService(std::filesystem::path document_root,
                                       std::size_t worker_count, std::size_t queue_capacity)
    : pool_(worker_count), queue_capacity_(queue_capacity) {
    if (worker_count == 0 || queue_capacity_ == 0) {
        throw std::invalid_argument("file service requires positive worker and queue limits");
    }
    std::error_code error;
    root_ = std::filesystem::canonical(std::move(document_root), error);
    if (error || !std::filesystem::is_directory(root_)) {
        throw std::invalid_argument("static document root must be an existing directory");
    }
}

BoundedFileService::~BoundedFileService() {
    pool_.join();
}

net::Awaitable<FileResult> BoundedFileService::read(net::asio::any_io_executor reply_executor,
                                                    std::filesystem::path path,
                                                    std::size_t maximum_bytes) {
    auto result =
        co_await net::asio::async_initiate<decltype(net::use_awaitable), void(FileResult)>(
            [this, reply_executor, path = std::move(path),
             maximum_bytes](auto completion_handler) mutable {
                using Handler = std::decay_t<decltype(completion_handler)>;
                auto handler = std::make_shared<Handler>(std::move(completion_handler));
                if (!tryAcquireSlot()) {
                    net::asio::post(reply_executor,
                                    [handler] { (*handler)(failure(FileStatus::Busy)); });
                    return;
                }
                net::asio::post(
                    pool_, [this, reply_executor, path = std::move(path), maximum_bytes, handler] {
                        auto file_result = readInWorker(path, maximum_bytes);
                        releaseSlot();
                        net::asio::post(reply_executor,
                                        [handler, file_result = std::move(file_result)]() mutable {
                                            (*handler)(std::move(file_result));
                                        });
                    });
            },
            net::use_awaitable);
    co_return result;
}

const std::filesystem::path& BoundedFileService::documentRoot() const noexcept {
    return root_;
}

FileResult BoundedFileService::readInWorker(const std::filesystem::path& path,
                                            std::size_t maximum_bytes) const {
    std::error_code error;
    const auto candidate = std::filesystem::weakly_canonical(path, error);
    if (error) {
        return failure(FileStatus::NotFound);
    }
    if (!isBelow(root_, candidate)) {
        return failure(FileStatus::Forbidden);
    }
    if (!std::filesystem::exists(candidate, error)) {
        return failure(FileStatus::NotFound);
    }
    if (error || !std::filesystem::is_regular_file(candidate, error)) {
        return failure(FileStatus::Forbidden);
    }
    const auto size = std::filesystem::file_size(candidate, error);
    if (error) {
        return failure(FileStatus::InternalError);
    }
    if (size > maximum_bytes) {
        return failure(FileStatus::TooLarge);
    }

    std::ifstream file(candidate, std::ios::binary);
    if (!file) {
        return failure(FileStatus::InternalError);
    }
    std::string body(static_cast<std::size_t>(size), '\0');
    file.read(body.data(), static_cast<std::streamsize>(body.size()));
    if (!file && !file.eof()) {
        return failure(FileStatus::InternalError);
    }
    return {.status = FileStatus::Ok, .path = candidate, .body = std::move(body)};
}

bool BoundedFileService::tryAcquireSlot() noexcept {
    auto pending = pending_.load(std::memory_order_relaxed);
    while (pending < queue_capacity_) {
        if (pending_.compare_exchange_weak(pending, pending + 1, std::memory_order_acq_rel,
                                           std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

void BoundedFileService::releaseSlot() noexcept {
    pending_.fetch_sub(1, std::memory_order_release);
}

std::optional<std::string> percentDecodePath(std::string_view value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        char character = value[index];
        if (character == '%') {
            if (index + 2 >= value.size()) {
                return std::nullopt;
            }
            const auto high = hexValue(value[++index]);
            const auto low = hexValue(value[++index]);
            if (high < 0 || low < 0) {
                return std::nullopt;
            }
            character = static_cast<char>((high << 4) | low);
        }
        if (character == '\0' || character == '\\') {
            return std::nullopt;
        }
        decoded.push_back(character);
    }
    return decoded;
}

std::string mimeTypeForPath(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::transform(
        extension.begin(), extension.end(), extension.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    if (extension == ".html" || extension == ".htm") {
        return "text/html; charset=utf-8";
    }
    if (extension == ".css") {
        return "text/css; charset=utf-8";
    }
    if (extension == ".js") {
        return "application/javascript";
    }
    if (extension == ".json") {
        return "application/json";
    }
    if (extension == ".txt") {
        return "text/plain; charset=utf-8";
    }
    if (extension == ".png") {
        return "image/png";
    }
    if (extension == ".jpg" || extension == ".jpeg") {
        return "image/jpeg";
    }
    return "application/octet-stream";
}

}  // namespace pulsegate::http
