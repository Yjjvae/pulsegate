# PulseGate 依赖管理

本项目只保留一种依赖来源策略：**Ubuntu 24.04 系统包提供平台库；项目源码用 CMake FetchContent
拉取固定 revision。** 不同时引入 Conan、vcpkg 或同一库的第二个来源。

## 当前清单

| 组件 | 用途 | 版本/固定值 | 来源 | 许可证 |
| --- | --- | --- | --- | --- |
| Boost.Asio / Boost.System | 异步 I/O、Timer、错误码 | 系统包，最低 1.83 | `libboost-dev` | BSL-1.0 |
| Threads | 线程支持 | 操作系统提供 | CMake `Threads::Threads` | 平台组件，不随项目分发 |
| yaml-cpp | YAML 配置解析 | 0.8.0，`f7320141120f720aecc4c32be25586e7da9eb978` | FetchContent Git revision | MIT |
| GoogleTest | 单元/集成测试 | 1.17.0，`52eb8108c5bdec04579160ae17225d66034bd723`，SHA-256 已校验 | FetchContent archive | BSD-3-Clause |

当前没有使用 spdlog、OpenSSL、Boost.Beast、Conan 或 vcpkg。引入任何一个前，先说明它解决的实际需求，
再通过独立 Pull Request 更新本清单。

## 可复现边界

- Docker runtime/build 使用 `ubuntu:24.04`；GitHub Actions 固定为 `ubuntu-24.04`，两者安装同一发行版的
  `libboost-dev`；
- 本地开发机可使用其他 Linux 发行版，但必须满足 Boost 1.83+；CMake 配置日志会打印实际探测到的版本；
- yaml-cpp 固定 Git commit；GoogleTest 固定 archive commit 并校验 SHA-256；
- `build/`、FetchContent 生成目录和下载缓存均不会提交；第一次配置需要访问 Ubuntu 软件源和 GitHub，之后可复用
  对应构建目录的缓存；
- 离线构建时不要伪造或替换依赖：保留已配置的构建目录，或在联网机器预先准备依赖缓存。

## 本地验证

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug

# 查看本次实际解析到的系统 Boost 和固定源码版本
cmake --preset debug | grep 'PulseGate dependency'
```

第一次从空 `build/` 配置时应看到 Boost、yaml-cpp 和 GoogleTest 的版本信息；测试配置不会因为某台机器恰好
安装了系统 GoogleTest 而切换版本。

## 升级流程

一次 Pull Request 只升级一个依赖：

1. 从上游 release/commit 和许可证确认目标版本；
2. 更新 `cmake/Dependencies.cmake` 中的版本、commit 和 archive SHA-256；
3. 更新本清单，并说明兼容性风险；
4. 从干净 `build/` 目录运行 Debug、ASan/UBSan、clang-tidy 与 Docker/CI 检查；
5. 在 PR 中记录旧值、新值、验证结果和回滚方式。

不要直接修改 `build/_deps/`，也不要把第三方源码、二进制、`compile_commands.json` 或下载缓存提交进 Git。

## 未来包管理器迁移

当需要锁定更多二进制依赖、支持多平台或建立公司级缓存时，可以评估 Conan 或 vcpkg。迁移前必须新增 ADR，
说明选择原因、lockfile/registry 策略、许可证/SBOM 流程和从现有 FetchContent 的迁移计划；完成迁移后删除旧来源，
不要让两套依赖解析并存。
