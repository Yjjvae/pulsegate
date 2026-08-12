#include "pulsegate/http/config.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>

namespace pulsegate::http {
namespace {

class ConfigFile {
   public:
    explicit ConfigFile(std::string contents)
        : path_(std::filesystem::temp_directory_path() /
                ("pulsegate-config-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 ".yaml")) {
        write(std::move(contents));
    }
    ~ConfigFile() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }
    void write(std::string contents) const {
        std::ofstream output(path_);
        if (!output.is_open()) throw std::runtime_error("cannot write test configuration");
        output << contents;
        if (!output.good()) throw std::runtime_error("cannot flush test configuration");
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

   private:
    std::filesystem::path path_;
};

constexpr std::string_view kValidConfig = R"yaml(
server:
  listen_host: 127.0.0.1
  listen_port: 8080
  io_threads: 2
  max_connections: 32
  max_header_bytes: 16384
  max_body_bytes: 1048576
  output_high_water_bytes: 4096
  output_low_water_bytes: 2048
upstreams:
  - name: catalog
    endpoints: [127.0.0.1:9001, 127.0.0.1:9002]
    connect_timeout_ms: 300
    response_timeout_ms: 2000
    max_connections_per_endpoint: 4
    pool_shards: 1
routes:
  - path_prefix: /api/
    upstream: catalog
    rate_limit:
      requests_per_second: 100
      burst: 20
    cache:
      enabled: true
      ttl_ms: 1000
      max_object_bytes: 128
      max_bytes: 1024
      shards: 2
logging:
  level: info
  format: json
)yaml";

TEST(ConfigLoaderTest, LoadsTypedYamlAndAppliesDefaults) {
    ConfigFile file{std::string(kValidConfig)};
    const auto config = ConfigLoader{}.loadFromFile(file.path());

    EXPECT_EQ(config.server.listen_host, "127.0.0.1");
    EXPECT_EQ(config.server.listen_port, 8080U);
    EXPECT_EQ(config.server.io_threads, 2U);
    ASSERT_EQ(config.upstreams.size(), 1U);
    EXPECT_EQ(config.upstreams.front().endpoints.size(), 2U);
    ASSERT_EQ(config.routes.size(), 1U);
    ASSERT_TRUE(config.routes.front().rate_limit);
    EXPECT_EQ(config.routes.front().rate_limit->burst, 20.0);
    ASSERT_TRUE(config.routes.front().cache);
    EXPECT_EQ(config.routes.front().cache->ttl, std::chrono::seconds(1));
    EXPECT_EQ(config.logging.format, "json");
}

TEST(ConfigLoaderTest, ReturnsAllMeaningfulValidationErrors) {
    Config config;
    config.server.listen_port = 70000;
    config.server.io_threads = 0;
    config.server.output_low_water_bytes = 8;
    config.server.output_high_water_bytes = 4;
    config.upstreams.push_back({.name = "orders", .endpoints = {{.host = "", .service = ""}}});
    config.routes.push_back(
        {.path_prefix = "relative", .upstream = "missing", .rate_limit = {}, .cache = {}});

    const auto errors = ConfigLoader{}.validate(config);
    std::vector<std::string> paths;
    for (const auto& error : errors) paths.push_back(error.path);
    EXPECT_NE(std::find(paths.begin(), paths.end(), "server.listen_port"), paths.end());
    EXPECT_NE(std::find(paths.begin(), paths.end(), "server.io_threads"), paths.end());
    EXPECT_NE(std::find(paths.begin(), paths.end(), "server.output_low_water_bytes"), paths.end());
    EXPECT_NE(std::find(paths.begin(), paths.end(), "routes[0].upstream"), paths.end());
    EXPECT_NE(std::find(paths.begin(), paths.end(), "routes[0].path_prefix"), paths.end());
}

TEST(ConfigManagerTest, PublishesDynamicChangeButKeepsSnapshotOnStaticChange) {
    ConfigFile file{std::string(kValidConfig)};
    ConfigLoader loader;
    auto initial = std::make_shared<const ConfigSnapshot>(
        ConfigSnapshot{.value = loader.loadFromFile(file.path()),
                       .source_path = file.path(),
                       .loaded_at = std::chrono::system_clock::now()});
    boost::asio::io_context context;
    auto guard = boost::asio::make_work_guard(context);
    auto manager = std::make_shared<ConfigManager>(context.get_executor(), file.path(), initial);
    std::jthread runner([&context] { context.run(); });

    file.write(std::string(kValidConfig)
                   .replace(std::string(kValidConfig).find("level: info"), 11, "level: debug"));
    std::promise<ConfigReloadResult> first;
    manager->requestReload(
        [&first](ConfigReloadResult result) { first.set_value(std::move(result)); });
    const auto first_result = first.get_future().get();
    EXPECT_TRUE(first_result.published);
    EXPECT_EQ(manager->snapshot()->value.logging.level, "debug");

    file.write(
        std::string(kValidConfig)
            .replace(std::string(kValidConfig).find("listen_port: 8080"), 17, "listen_port: 8081"));
    std::promise<ConfigReloadResult> second;
    manager->requestReload(
        [&second](ConfigReloadResult result) { second.set_value(std::move(result)); });
    const auto second_result = second.get_future().get();
    EXPECT_FALSE(second_result.published);
    EXPECT_EQ(second_result.restart_required, std::vector<std::string>{"server.listen_port"});
    EXPECT_EQ(manager->snapshot()->value.server.listen_port, 8080U);
    manager.reset();
    guard.reset();
    runner.join();
}

}  // namespace
}  // namespace pulsegate::http
