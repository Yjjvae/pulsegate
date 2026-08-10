#include "pulsegate/http/static_file_handler.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace pulsegate::http {
namespace {

HttpResponse makeStaticError(int status, std::string reason, std::string body) {
    HttpResponse response;
    response.status_code = status;
    response.reason = std::move(reason);
    response.body = std::move(body);
    response.headers.add("Content-Type", "text/plain");
    return response;
}

}  // namespace

StaticFileHandler::StaticFileHandler(std::filesystem::path document_root,
                                     std::shared_ptr<FileService> files, std::size_t maximum_bytes)
    : document_root_(std::move(document_root)),
      files_(std::move(files)),
      maximum_bytes_(maximum_bytes) {
    if (!files_ || maximum_bytes_ == 0) {
        throw std::invalid_argument("static file handler requires a file service and size limit");
    }
}

net::Awaitable<HttpResponse> StaticFileHandler::operator()(RequestContext& context,
                                                           HttpRequest request) const {
    constexpr std::string_view kPrefix = "/static/";
    const auto query = request.target.find('?');
    const auto path = std::string_view(request.target).substr(0, query);
    if (!path.starts_with(kPrefix)) {
        co_return makeStaticError(404, "Not Found", "not found\n");
    }
    const auto decoded = percentDecodePath(path.substr(kPrefix.size()));
    if (!decoded || decoded->empty()) {
        co_return makeStaticError(403, "Forbidden", "forbidden\n");
    }
    const std::filesystem::path relative(*decoded);
    if (relative.is_absolute() || relative.has_root_directory() ||
        std::any_of(relative.begin(), relative.end(),
                    [](const auto& component) { return component == ".."; })) {
        co_return makeStaticError(403, "Forbidden", "forbidden\n");
    }

    const auto result = co_await files_->read(
        context.executor, document_root_ / relative.lexically_normal(), maximum_bytes_);
    switch (result.status) {
        case FileStatus::Ok: {
            HttpResponse response;
            response.body = result.body;
            response.headers.add("Content-Type", mimeTypeForPath(result.path));
            co_return response;
        }
        case FileStatus::NotFound:
            co_return makeStaticError(404, "Not Found", "not found\n");
        case FileStatus::Forbidden:
            co_return makeStaticError(403, "Forbidden", "forbidden\n");
        case FileStatus::TooLarge:
            co_return makeStaticError(413, "Content Too Large", "static file too large\n");
        case FileStatus::Busy:
            co_return makeStaticError(503, "Service Unavailable", "file service busy\n");
        case FileStatus::InternalError:
            co_return makeStaticError(500, "Internal Server Error", "static file error\n");
    }
    co_return makeStaticError(500, "Internal Server Error", "static file error\n");
}

}  // namespace pulsegate::http
