# Changelog

本项目遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 的结构，并使用
[Semantic Versioning](https://semver.org/lang/zh-CN/) 管理版本。只有通过发布清单的 tag 才是正式发布。

## [1.0.0] - 2026-08-19

这是首个稳定 `v1.0.0` 发布。发布前验收、性能基线和回滚策略见
[v1.0 发布准备](docs/release-v1.0.md)。

### Added

- 基于 Boost.Asio coroutine 的 HTTP/1.1 网关：路由、静态文件、反向代理、轮询负载均衡、健康检查和上游连接池；
- 全局/路由限流、响应缓存、熔断器、代理并发与连接池排队上限；
- YAML 配置、受控的日志热更新、异步日志、Prometheus 指标，以及优雅停机；
- CMake Presets、GoogleTest/CTest、Parser fuzz、GCC/Clang/ASan/UBSan/TSan、clang-format、clang-tidy；
- 多阶段 Docker 镜像和 Docker Compose 多上游演示；
- 可复现的 `/healthz` Release 性能基线脚本和环境记录。

### Changed

- CMake 项目、二进制和 `/api/version` 统一为 `1.0.0`；
- yaml-cpp 公开头文件以系统包含目录处理，保持项目自身的严格 `-Werror` 质量门禁。

### Fixed

- 顶层受保护协程不再通过带捕获的临时 coroutine lambda 启动，避免异步恢复时访问已销毁捕获导致的
  `stack-use-after-return`。
- 文件线程池读取会持有回调执行器的 work guard，避免 `io_context` 退出后仍向其投递回调的 TSan 数据竞争。

### Known limitations

- 不支持 TLS、WebSocket Upgrade、chunked 请求体与自动上游重试；
- 配置热更新仅支持 logging 元数据，监听地址、线程数、路由和上游变更需要重启；
- 性能报告是同机 loopback 基线，不是生产 SLO 或跨机器承诺。

## [0.8.2] - 2026-08-11

- `v1.0.0` 之前最近的正式发布标签。
