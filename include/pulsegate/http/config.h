#pragma once

#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/thread_pool.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "pulsegate/http/rate_limiter.h"
#include "pulsegate/http/response_cache.h"
#include "pulsegate/http/reverse_proxy.h"
#include "pulsegate/http/upstream_endpoint.h"
#include "pulsegate/net/asio_types.h"

namespace pulsegate::http {

struct ConfigError {
    std::string path;
    std::string message;
    [[nodiscard]] std::string format() const;
};

struct ServerConfig {
    std::string listen_host{"127.0.0.1"};
    std::uint32_t listen_port{8080};
    std::size_t io_threads{1};
    std::chrono::milliseconds idle_timeout{std::chrono::seconds(15)};
    std::chrono::milliseconds header_timeout{std::chrono::seconds(10)};
    std::chrono::milliseconds body_timeout{std::chrono::seconds(30)};
    std::chrono::milliseconds graceful_shutdown{std::chrono::seconds(15)};
    std::size_t max_connections{1024};
    std::size_t max_header_bytes{16 * 1024};
    std::size_t max_body_bytes{1024 * 1024};
    std::size_t output_high_water_bytes{4 * 1024 * 1024};
    std::size_t output_low_water_bytes{2 * 1024 * 1024};
};

struct UpstreamConfig {
    std::string name;
    std::vector<UpstreamEndpoint> endpoints;
    ProxyLimits proxy_limits{};
    std::size_t pool_shards{1};
};

struct RouteConfig {
    std::string path_prefix;
    std::string upstream;
    std::optional<RateLimitConfig> rate_limit;
    std::optional<ResponseCacheConfig> cache;
};

struct LoggingConfig {
    std::string level{"info"};
    std::string format{"text"};
};

struct Config {
    ServerConfig server;
    std::vector<RouteConfig> routes;
    std::vector<UpstreamConfig> upstreams;
    LoggingConfig logging;
};

struct ConfigSnapshot {
    Config value;
    std::filesystem::path source_path;
    std::chrono::system_clock::time_point loaded_at;
};

class ConfigValidationError : public std::runtime_error {
   public:
    explicit ConfigValidationError(std::vector<ConfigError> errors);
    [[nodiscard]] const std::vector<ConfigError>& errors() const noexcept;

   private:
    std::vector<ConfigError> errors_;
};

class ConfigLoader {
   public:
    [[nodiscard]] Config loadFromFile(const std::filesystem::path& path) const;
    [[nodiscard]] std::vector<ConfigError> validate(const Config& config) const;
};

struct ConfigReloadResult {
    bool published{false};
    std::vector<ConfigError> errors;
    std::vector<std::string> restart_required;
};

// Owns an immutable snapshot. Reload parsing happens on a dedicated worker;
// publication and de-duplication happen on this manager's strand.
class ConfigManager : public std::enable_shared_from_this<ConfigManager> {
   public:
    using ReloadHandler = std::function<void(ConfigReloadResult)>;

    ConfigManager(net::asio::any_io_executor executor, std::filesystem::path path,
                  std::shared_ptr<const ConfigSnapshot> initial);
    ~ConfigManager();

    void requestReload(ReloadHandler handler = {});
    [[nodiscard]] std::shared_ptr<const ConfigSnapshot> snapshot() const noexcept;

   private:
    [[nodiscard]] static std::vector<std::string> staticChanges(const Config& before,
                                                                const Config& after);

    net::asio::strand<net::asio::any_io_executor> strand_;
    std::filesystem::path path_;
    ConfigLoader loader_;
    net::asio::thread_pool worker_{1};
    std::atomic<std::shared_ptr<const ConfigSnapshot>> snapshot_;
    bool reload_in_progress_{false};
};

}  // namespace pulsegate::http
