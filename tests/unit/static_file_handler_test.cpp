#include "pulsegate/http/static_file_handler.h"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace {

using pulsegate::http::FileResult;
using pulsegate::http::FileService;
using pulsegate::http::FileStatus;
using pulsegate::http::HttpMethod;
using pulsegate::http::HttpRequest;
using pulsegate::http::HttpResponse;
using pulsegate::http::RequestContext;
using pulsegate::http::StaticFileHandler;

class TemporaryDirectory {
   public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() / "pulsegate-static-handler-test";
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() {
        std::filesystem::remove_all(path_);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

   private:
    std::filesystem::path path_;
};

HttpResponse invoke(StaticFileHandler& handler, std::string target) {
    boost::asio::io_context context;
    RequestContext request_context{.executor = context.get_executor(),
                                   .request_id = "static-request",
                                   .peer = {},
                                   .downstream = {},
                                   .set_current_proxy = {},
                                   .write_downstream = {}};
    HttpRequest request{.method = HttpMethod::Get,
                        .target = std::move(target),
                        .version_major = 1,
                        .version_minor = 1,
                        .headers = {},
                        .body = {}};
    auto future = boost::asio::co_spawn(context, handler(request_context, std::move(request)),
                                        boost::asio::use_future);
    context.run();
    return future.get();
}

class BusyFileService final : public FileService {
   public:
    pulsegate::net::Awaitable<FileResult> read(pulsegate::net::asio::any_io_executor,
                                               std::filesystem::path, std::size_t) override {
        co_return FileResult{.status = FileStatus::Busy, .path = {}, .body = {}};
    }
};

TEST(StaticFileHandlerTest, ServesBoundedFileAndRejectsTraversal) {
    TemporaryDirectory directory;
    {
        std::ofstream output(directory.path() / "index.txt", std::ios::binary);
        output << "hello static\n";
    }
    {
        std::ofstream output(directory.path() / "large.txt", std::ios::binary);
        output << std::string(33, 'x');
    }
    const auto outside = directory.path().parent_path() / "pulsegate-static-outside.txt";
    {
        std::ofstream output(outside, std::ios::binary);
        output << "outside\n";
    }
    std::filesystem::create_symlink(outside, directory.path() / "escape.txt");
    auto files = std::make_shared<pulsegate::http::BoundedFileService>(directory.path(), 1, 2);
    StaticFileHandler handler(directory.path(), files, 1024);

    const auto response = invoke(handler, "/static/index.txt");
    EXPECT_EQ(response.status_code, 200);
    EXPECT_EQ(response.body, "hello static\n");
    EXPECT_EQ(response.headers.get("content-type"), "text/plain; charset=utf-8");

    EXPECT_EQ(invoke(handler, "/static/%2e%2e/secret.txt").status_code, 403);
    EXPECT_EQ(invoke(handler, "/static/%00bad").status_code, 403);
    EXPECT_EQ(invoke(handler, "/static/missing.txt").status_code, 404);
    EXPECT_EQ(invoke(handler, "/static/escape.txt").status_code, 403);

    StaticFileHandler small_handler(directory.path(), files, 32);
    EXPECT_EQ(invoke(small_handler, "/static/large.txt").status_code, 413);
    std::filesystem::remove(outside);
}

TEST(StaticFileHandlerTest, MapsBusyFilePoolToServiceUnavailable) {
    TemporaryDirectory directory;
    StaticFileHandler handler(directory.path(), std::make_shared<BusyFileService>(), 1024);

    EXPECT_EQ(invoke(handler, "/static/index.txt").status_code, 503);
}

}  // namespace
