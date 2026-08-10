#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "pulsegate/http/file_service.h"
#include "pulsegate/http/router.h"

namespace pulsegate::http {

class StaticFileHandler {
   public:
    StaticFileHandler(std::filesystem::path document_root, std::shared_ptr<FileService> files,
                      std::size_t maximum_bytes = 256 * 1024);

    net::Awaitable<HttpResponse> operator()(RequestContext& context, HttpRequest request) const;

   private:
    std::filesystem::path document_root_;
    std::shared_ptr<FileService> files_;
    std::size_t maximum_bytes_;
};

}  // namespace pulsegate::http
