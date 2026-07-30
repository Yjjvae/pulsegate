# PulseGate：使用 Boost.Asio 与 C++20 协程实现高性能 HTTP 网关

> 一份以“能学习、能运行、能测试、能压测、能写进简历”为目标的完整实现教程。

---

## 导航

这份文档较长，建议按顺序学习，不要直接跳到性能优化：

- 基础准备：第 0～4 节，明确范围、目录、Git 与工程规范；
- 网络主线：第 5～10 节，从 CMake 骨架做到 Boost.Asio 异步并发服务；
- HTTP 与网关：第 11～19 节，从路由做到代理、可靠性和可观测性；
- 质量与性能：第 20～22 节，建立测试、Sanitizer 和可复现压测；
- 交付工程：第 23～25 节，完成 Docker、CI 与依赖管理；
- 学习执行：第 26～35 节，按周推进、做 PR、准备面试和发布。

可以选择三个完成层级：

| 层级 | 完成到 | 适用结果 |
|---|---|---|
| 最小可展示版 | `v0.5.0` | Linux C++ HTTP Server，可用于阶段性展示 |
| 简历推荐版 | `v0.7.0` | 有异步代理、健康检查和连接池，项目差异化明显 |
| 完整工程版 | `v1.0.0` | 再包含可靠性、可观测性、Docker、CI 和性能报告 |

---

## 0. 先读这里

这份教程不是让你一次性复制出一个“大项目”，而是指导你按可验证的小版本，逐步完成一个基于 Boost.Asio 的 C++ HTTP 网关：

```text
Client
  |
  v
+---------------------------+
| PulseGate                 |
|                           |
| HTTP parser / router      |
| reverse proxy             |
| rate limit / cache        |
| health check / metrics    |
+---------------------------+
  |           |           |
  v           v           v
upstream-a  upstream-b  upstream-c
```

完成后，项目应具备以下能力：

- 基于 Boost.Asio `io_context`、TCP Socket 和 C++20 Coroutine 的异步网络层；
- HTTP/1.1 增量解析、Keep-Alive、请求体和错误处理；
- 单线程异步模型到多线程 `io_context + strand` 的演进过程；
- 反向代理、上游连接池、负载均衡和健康检查；
- 超时、背压、限流、缓存、熔断和优雅停机；
- 日志、指标、单元测试、集成测试和故障测试；
- CMake、Git、GitHub Pull Request、GitHub Actions 和 Docker；
- 可复现的性能测试报告，而不是脱离环境的“十万 QPS”。

Boost.Asio 负责跨平台 I/O 抽象、Socket、Timer、Executor、Signal 和异步调度；项目自己实现 HTTP 协议层及全部网关能力。在 Linux 上，Boost.Asio 默认通过 `epoll` 等内核设施完成事件分发，但业务代码不直接依赖 `epoll` 接口。

### 0.1 项目边界

第一版主动限制范围：

- 开发和部署目标为 Linux，但主线网络代码不直接调用平台专用 API；
- 只实现 HTTP/1.1，不把 HTTP/2、HTTP/3 放进主线；
- v1.0 前不实现 TLS；以后使用 OpenSSL，不自行实现密码学；
- 不实现完整 Nginx 配置语法；
- 不实现分布式配置中心和服务注册中心；
- 不追求击败 Nginx，追求设计合理、数据可信、过程可解释；
- 不自行重复实现 `EventLoop`、`epoll` 包装和跨平台 Socket 层；
- 不把同步阻塞工作放进 `io_context` 线程；
- 不使用 Boost.Beast 作为主线 HTTP Parser，以保留协议解析的学习深度；
- 可在 `labs/raw_epoll_server/` 完成一个小型原生 `epoll` 实验，但它不是网关主线依赖。

### 0.2 建议周期

如果每周投入 10～15 小时，建议用 10～12 周完成。最迟到第 6 周时，项目已经可以作为初版简历项目；后续阶段负责增加技术深度和工程可信度。

### 0.3 技术选型

| 层次 | 主线选择 | 是否自研 |
|---|---|---|
| TCP、Timer、Signal、Executor | Boost.Asio | 否 |
| 错误码 | Boost.System | 否 |
| 异步组织 | C++20 Coroutine + Asio `awaitable` | 项目封装 |
| HTTP/1.1 Parser/Serializer | 项目实现 | 是 |
| HTTP 路由与 Session | 项目实现 | 是 |
| 反向代理、Pool、健康检查 | 项目实现 | 是 |
| 限流、缓存、熔断、背压 | 项目实现 | 是 |
| HTTP 差分验证/文件实验 | Boost.Beast，可选 | 否 |
| Linux 底层理解 | raw epoll lab，可选 | 是 |

选择原则：让 Boost.Asio 解决已经成熟且容易重复造错的 I/O 调度问题，把学习和简历价值集中在协程生命周期、HTTP 边界、代理、连接池、可靠性和工程交付上。

### 0.4 每一阶段都遵循同一个闭环

每个阶段都必须完成：

1. 写 Issue，说明目标、非目标和验收标准；
2. 从 `main` 创建功能分支；
3. 先写接口和测试用例，再完成最小实现；
4. 本地执行格式化、构建、测试和 Sanitizer；
5. 更新文档与变更日志；
6. 提交小而清晰的 Commit；
7. 推送 GitHub，创建 Pull Request；
8. CI 通过后合并；
9. 在对应里程碑打 Tag。

不要积攒几千行代码后一次提交。提交历史本身也是简历项目的一部分。

---

## 1. 学习路线与版本里程碑

| 阶段 | 版本 | 交付能力 | 核心知识 |
|---|---:|---|---|
| 0 | `v0.0.1` | 工程骨架可编译、可测试 | CMake、Boost、CTest、Git |
| 1 | `v0.1.0` | Asio 同步 TCP/HTTP 基线 | TCP、RAII、HTTP 基础 |
| 2 | `v0.2.0` | 单线程异步 HTTP Server | `io_context`、Coroutine、Buffer |
| 3 | `v0.3.0` | 超时、取消和连接生命周期 | `steady_timer`、取消、状态管理 |
| 4 | `v0.4.0` | 多线程异步服务器 | Executor、`strand`、线程封闭 |
| 5 | `v0.5.0` | 路由和静态资源服务 | Handler、Router、安全路径 |
| 6 | `v0.6.0` | 协程式异步反向代理 | Resolver、双向流、负载均衡 |
| 7 | `v0.7.0` | 健康检查和上游连接池 | 故障恢复、资源复用 |
| 8 | `v0.8.0` | 限流、缓存、熔断和背压 | 并发控制、过载保护 |
| 9 | `v0.9.0` | 配置、日志、指标、优雅停机 | 可观测性、运维能力 |
| 10 | `v1.0.0` | CI、Docker、压测报告和发布 | 工程化、性能分析 |

依赖关系如下：

```text
Boost.Asio
    ^
    |
runtime <--- net <--- http <--- gateway <--- app
   ^          ^         ^          ^
   +----------+---------+----------+--- observability/config
```

规则是：高层可以依赖低层，低层不能反向依赖高层。例如 `net` 不应该知道 HTTP，`http` 不应该知道某个具体负载均衡算法。Boost 类型尽量停留在 `runtime/net` 边界；纯业务算法如缓存、限流和负载均衡应能脱离真实网络进行单元测试。

---

## 2. 开发环境

### 2.1 推荐环境

- 推荐 Ubuntu 24.04 或其他提供 Boost 1.83+ 的 Linux 发行版；
- GCC 12+ 或 Clang 16+；
- CMake 3.24+；
- Boost 1.83+，至少包含 Asio 与 System；
- Ninja；
- Git；
- GDB；
- clang-format、clang-tidy；
- Docker Engine 和 Docker Compose；
- curl、wrk；
- 可选：perf、strace、Valgrind。

检查版本：

```bash
g++ --version
clang++ --version
cmake --version
ninja --version
git --version
docker --version
docker compose version
```

Ubuntu 示例安装命令：

```bash
sudo apt update
sudo apt install -y \
  build-essential clang clang-format clang-tidy cmake ninja-build \
  gdb git curl wrk linux-tools-common libboost-dev
```

Ubuntu 22.04 默认 Boost 版本低于本教程基线；若使用 22.04，应通过 Conan/vcpkg 或经过固定版本的自建依赖提供 Boost 1.83+，不能一部分模块包含系统 Boost、另一部分包含自带 Boost。如果 `wrk` 或 `perf` 包不存在，先通过发行版文档确认对应包名。

### 2.2 为什么选择 C++20

项目使用 C++20，但不为了“新”而滥用特性。优先使用：

- RAII；
- `std::unique_ptr` / `std::shared_ptr` / `std::weak_ptr`；
- `std::string_view`；
- `std::span`；
- `std::chrono`；
- `std::jthread` 和停止语义；
- `std::atomic`；
- 结构化绑定和强类型枚举。

网络层以 Boost.Asio 为主，重点使用：

- `boost::asio::io_context`；
- `boost::asio::ip::tcp::{acceptor,socket,resolver}`；
- `boost::asio::steady_timer`；
- `boost::asio::signal_set`；
- `boost::asio::strand`；
- `boost::asio::awaitable`、`co_spawn` 和 `use_awaitable`；
- `boost::system::error_code`。

预期的网络错误优先使用 `error_code` 分支处理；真正违反程序不变量的错误才抛异常。协程顶层必须有统一的异常收口，不能让异常穿过线程入口。

---

## 3. 最终目录结构

项目最终建议形成下面的结构。不要第一天创建全部空文件，而是在对应阶段逐步增加。

```text
http_server/
├── .github/
│   ├── ISSUE_TEMPLATE/
│   │   ├── bug.yml
│   │   └── feature.yml
│   ├── pull_request_template.md
│   └── workflows/
│       ├── ci.yml
│       └── docker.yml
├── app/
│   └── pulsegate_main.cpp
├── benchmarks/
│   ├── README.md
│   ├── wrk/
│   │   ├── get.lua
│   │   └── post.lua
│   └── results/
│       └── .gitkeep
├── cmake/
│   ├── Dependencies.cmake
│   ├── ProjectOptions.cmake
│   └── Sanitizers.cmake
├── config/
│   ├── pulsegate.example.yaml
│   └── prometheus.yml
├── docker/
│   └── entrypoint.sh
├── docs/
│   ├── architecture.md
│   ├── benchmark-report.md
│   ├── decisions/
│   │   ├── 0001-asio-coroutine-model.md
│   │   └── 0002-http-parser.md
│   └── development.md
├── include/
│   └── pulsegate/
│       ├── core/
│       │   ├── error.h
│       │   └── result.h
│       ├── runtime/
│       │   ├── asio_runtime.h
│       │   └── coroutine_guard.h
│       ├── net/
│       │   ├── buffer.h
│       │   ├── connection_limits.h
│       │   ├── deadline.h
│       │   ├── endpoint.h
│       │   ├── listener.h
│       │   ├── stream_ops.h
│       │   └── tcp_session.h
│       ├── http/
│       │   ├── headers.h
│       │   ├── http_parser.h
│       │   ├── http_request.h
│       │   ├── http_response.h
│       │   ├── http_server.h
│       │   └── router.h
│       ├── gateway/
│       │   ├── cache.h
│       │   ├── circuit_breaker.h
│       │   ├── health_checker.h
│       │   ├── load_balancer.h
│       │   ├── proxy_session.h
│       │   ├── rate_limiter.h
│       │   ├── upstream.h
│       │   ├── upstream_connection.h
│       │   └── upstream_pool.h
│       ├── config/
│       │   ├── config.h
│       │   └── config_loader.h
│       └── observability/
│           ├── logger.h
│           └── metrics.h
├── src/
│   ├── core/
│   ├── runtime/
│   ├── net/
│   ├── http/
│   ├── gateway/
│   ├── config/
│   └── observability/
├── tests/
│   ├── unit/
│   ├── integration/
│   ├── fuzz/
│   └── testdata/
├── labs/
│   └── raw_epoll_server/
│       ├── README.md
│       └── main.cpp
├── tools/
│   ├── benchmark.sh
│   ├── format.sh
│   └── mock_upstream.py
├── .clang-format
├── .clang-tidy
├── .dockerignore
├── .gitignore
├── CHANGELOG.md
├── CMakeLists.txt
├── CMakePresets.json
├── CODE_OF_CONDUCT.md
├── CONTRIBUTING.md
├── Dockerfile
├── LICENSE
├── README.md
└── compose.yaml
```

### 3.1 文件组织原则

- `include/pulsegate/`：其他模块需要包含的公开头文件；
- `src/`：实现文件和仅实现内部使用的私有头文件；
- `app/`：只负责解析启动参数、装配对象和启动服务；
- `runtime/`：管理 `io_context`、工作线程、停止过程和协程异常；
- `net/`：对 Asio TCP、Timer 与连接生命周期做项目级封装；
- `tests/unit/`：不依赖真实网络或时间的快速测试；
- `tests/integration/`：允许绑定 loopback 端口、启动线程和进程；
- `tests/fuzz/`：解析器等不可信输入边界的模糊测试；
- `benchmarks/`：压测脚本、环境说明和结果；
- `tools/`：开发辅助脚本，不放核心业务逻辑；
- `labs/`：与主项目隔离的底层实验，不允许被 `src/` 链接；
- `docs/decisions/`：Architecture Decision Record，记录重要取舍。

### 3.2 头文件规则

- 头文件必须自包含，单独包含也能编译；
- 优先前置声明，减少不必要包含；
- 不在头文件中写 `using namespace`；
- 一个主要类对应一对 `.h/.cpp`；
- 模板或真正短小的函数才放头文件；
- 所有权必须从类型上可见，避免裸 owning pointer；
- 回调参数和生命周期必须写进注释或接口约束。

---

## 4. Git 与 GitHub 工作流

### 4.1 初始化仓库

如果目录还不是正常 Git 仓库：

```bash
git init
git branch -M main
git config user.name "你的名字"
git config user.email "你的 GitHub 邮箱"
```

创建 GitHub 空仓库后，再关联远端：

```bash
git remote add origin git@github.com:<用户名>/pulsegate.git
git remote -v
git push -u origin main
```

不要把 GitHub Token、SSH 私钥、密码或包含密钥的配置文件提交到仓库。

### 4.2 分支策略

个人项目也使用轻量 GitHub Flow：

```text
main
  ├── feat/asio-runtime
  ├── feat/http-parser
  ├── feat/coroutine-http-session
  ├── fix/session-timeout-race
  └── docs/benchmark-v1
```

日常流程：

```bash
git switch main
git pull --ff-only
git switch -c feat/http-parser

# 修改、测试
git status
git diff
git add <明确的文件>
git commit -m "feat(http): add incremental request parser"
git push -u origin feat/http-parser
```

然后在 GitHub 创建 Pull Request。即使只有一个人，也要在 PR 中填写：

- 为什么做；
- 做了什么；
- 没做什么；
- 如何测试；
- 性能或兼容性影响；
- 风险和回滚方式。

GitHub 官方将 Pull Request 定义为从一个分支向另一个分支提出合并变更的协作机制，它还能承载检查结果和代码评审。个人项目使用 PR，可以保留完整的工程决策轨迹。

### 4.3 Commit 规范

采用简化 Conventional Commits：

```text
feat(runtime): add Boost.Asio io context runner
feat(net): accept sessions with C++20 coroutines
fix(http): preserve body bytes after header parse
test(cache): cover ttl expiration
perf(buffer): reduce front erase copies
refactor(net): serialize each session with a strand
docs(benchmark): record 1k connection result
build(cmake): add sanitizer presets
ci(github): run unit tests with gcc and clang
```

一次 Commit 只表达一个逻辑变化。不要使用：

```text
update
fix bug
final version
各种修改
```

### 4.4 Issue、Milestone 与 Tag

为上面每个版本创建 Milestone，再把功能拆成 Issue。例如 `v0.2.0`：

- `runtime: run and stop io_context`
- `net: implement coroutine listener`
- `net: implement Buffer prepare/commit interface`
- `http: implement incremental parser`
- `http: implement coroutine HTTP session`
- `net: add top-level coroutine exception boundary`
- `test: add fragmented request and peer-close integration tests`

版本完成后：

```bash
git switch main
git pull --ff-only
git tag -a v0.2.0 -m "Single-thread Boost.Asio coroutine server"
git push origin v0.2.0
```

从 `v0.1.0` 开始维护 `CHANGELOG.md`，按 Added、Changed、Fixed、Performance、Security 分类。

### 4.5 main 分支保护

仓库公开后，在 GitHub 中为 `main` 配置：

- 禁止 force push；
- 必须通过 Pull Request 合并；
- 必须通过 `build-and-test` 状态检查；
- 推荐 squash merge，保持每个 PR 在主线中是一个完整变化；
- 删除已合并分支。

---

## 5. 阶段 0：工程骨架

### 5.1 阶段目标

这一阶段不写网络代码，只建立一个可靠的工程入口：

- Debug 和 Release 都能构建；
- `ctest` 能运行；
- 编译警告开启；
- 支持 ASan/UBSan 和 TSan 独立构建；
- 格式化和静态检查有统一配置；
- 构建产物不进入 Git。

### 5.2 第一批文件

```text
CMakeLists.txt
CMakePresets.json
cmake/ProjectOptions.cmake
cmake/Sanitizers.cmake
cmake/Dependencies.cmake
app/pulsegate_main.cpp
tests/unit/smoke_test.cpp
.gitignore
.clang-format
.clang-tidy
README.md
LICENSE
```

### 5.3 顶层 CMake 设计

顶层 `CMakeLists.txt` 只负责项目级装配，不堆积所有源码：

```cmake
cmake_minimum_required(VERSION 3.24)
project(
  PulseGate
  VERSION 0.0.1
  DESCRIPTION "A Boost.Asio based HTTP gateway"
  LANGUAGES CXX
)

option(PULSEGATE_BUILD_TESTS "Build tests" ON)
option(PULSEGATE_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(PULSEGATE_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(PULSEGATE_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ProjectOptions)
include(Sanitizers)
include(Dependencies)

add_subdirectory(src)
add_subdirectory(app)

if(PULSEGATE_BUILD_TESTS)
  include(CTest)
  enable_testing()
  add_subdirectory(tests)
endif()
```

不要使用全局 `include_directories()`、`link_libraries()` 和到处修改 `CMAKE_CXX_FLAGS`。使用 target-based CMake：

```cmake
add_library(pulsegate_core)
target_sources(pulsegate_core PRIVATE core/error.cpp)
target_include_directories(
  pulsegate_core
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)
target_link_libraries(pulsegate_core PUBLIC pulsegate_project_options)
```

网络层单独建 target，不把 Boost 传播给纯算法模块：

```cmake
add_library(pulsegate_runtime)
target_sources(
  pulsegate_runtime
  PRIVATE
    runtime/asio_runtime.cpp
)
target_include_directories(
  pulsegate_runtime
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)
target_link_libraries(
  pulsegate_runtime
  PUBLIC
    Boost::headers
    Threads::Threads
    pulsegate_project_options
)
target_compile_definitions(
  pulsegate_runtime
  PUBLIC
    BOOST_ASIO_NO_DEPRECATED
)
```

`BOOST_ASIO_NO_DEPRECATED` 用于避免项目不知不觉依赖旧接口。如果某个发行版 Boost 无法通过该设置构建，先确认具体弃用接口，不要直接全局移除约束。

`pulsegate_project_options` 是 INTERFACE target，用来统一警告和通用选项：

```cmake
add_library(pulsegate_project_options INTERFACE)

target_compile_options(
  pulsegate_project_options
  INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang>:
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wshadow
      -Wformat=2
    >
)
```

不要立刻开启 `-Werror` 作为所有环境的默认值；可以在 CI 中开启，否则编译器升级可能让普通开发构建突然失效。

### 5.4 CMake Presets

将团队通用构建方式放进 `CMakePresets.json`，个人路径放进不提交的 `CMakeUserPresets.json`：

```json
{
  "version": 3,
  "cmakeMinimumRequired": {
    "major": 3,
    "minor": 24,
    "patch": 0
  },
  "configurePresets": [
    {
      "name": "debug",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/debug",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "PULSEGATE_BUILD_TESTS": "ON"
      }
    },
    {
      "name": "release",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/release",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Release",
        "PULSEGATE_BUILD_TESTS": "OFF"
      }
    },
    {
      "name": "asan",
      "inherits": "debug",
      "binaryDir": "${sourceDir}/build/asan",
      "cacheVariables": {
        "PULSEGATE_ENABLE_ASAN": "ON",
        "PULSEGATE_ENABLE_UBSAN": "ON"
      }
    },
    {
      "name": "tsan",
      "inherits": "debug",
      "binaryDir": "${sourceDir}/build/tsan",
      "cacheVariables": {
        "PULSEGATE_ENABLE_TSAN": "ON"
      }
    }
  ],
  "buildPresets": [
    {"name": "debug", "configurePreset": "debug"},
    {"name": "release", "configurePreset": "release"},
    {"name": "asan", "configurePreset": "asan"},
    {"name": "tsan", "configurePreset": "tsan"}
  ],
  "testPresets": [
    {
      "name": "debug",
      "configurePreset": "debug",
      "output": {"outputOnFailure": true}
    },
    {
      "name": "asan",
      "configurePreset": "asan",
      "output": {"outputOnFailure": true}
    },
    {
      "name": "tsan",
      "configurePreset": "tsan",
      "output": {"outputOnFailure": true}
    }
  ]
}
```

常用命令统一为：

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug

cmake --preset asan
cmake --build --preset asan
ctest --preset asan
```

ASan/UBSan 和 TSan 不放在同一个二进制中，分别测试。

### 5.5 Boost 与 GoogleTest 依赖

主线使用系统安装或依赖管理器提供的 Boost，并固定最低版本。Boost.System 从
1.69 起是 header-only，1.89 又移除了兼容用的空二进制库；因此不要强制要求一个
可能根本不存在的 `boost_system` CMake 组件。`cmake/Dependencies.cmake`：

```cmake
find_package(Threads REQUIRED)

if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()
find_package(Boost 1.83 REQUIRED)
```

选择 Boost 依赖方式时只保留一种来源：

1. **学习和本地开发**：发行版安装 `libboost-dev`；
2. **CI/Docker**：固定基础镜像和 Boost 包版本；
3. **需要严格跨环境复现时**：再迁移 Conan 或 vcpkg，并写 ADR；
4. 不要同时混用系统 Boost、`FetchContent` Boost 和包管理器 Boost。

网络 target 链接 `Boost::headers` 和 `Threads::Threads`。项目仍然使用
`boost::system::error_code`，只是现代 Boost.System 不再需要单独的链接库。

启动阶段增加一个最小 Asio smoke test：

```cpp
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <atomic>

#include <gtest/gtest.h>

TEST(AsioSmokeTest, ExecutesPostedHandler) {
    boost::asio::io_context context;
    std::atomic_bool called{false};

    boost::asio::post(context, [&called] {
        called.store(true, std::memory_order_relaxed);
    });

    context.run();
    EXPECT_TRUE(called.load(std::memory_order_relaxed));
}
```

在 `cmake/Dependencies.cmake` 中用 `FetchContent` 引入 GoogleTest，并固定到明确 release 或 commit SHA，避免每次构建下载不同代码：

```cmake
if(PULSEGATE_BUILD_TESTS)
  include(FetchContent)

  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG <固定的-release-tag-或-commit-sha>
    GIT_SHALLOW TRUE
  )

  FetchContent_MakeAvailable(googletest)
endif()
```

测试 target：

```cmake
add_executable(pulsegate_unit_tests)
target_sources(
  pulsegate_unit_tests
  PRIVATE
    unit/smoke_test.cpp
)
target_link_libraries(
  pulsegate_unit_tests
  PRIVATE
    pulsegate_core
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(pulsegate_unit_tests)
```

首次构建需要联网获取依赖。CI 和正式发布中应固定 commit，并考虑依赖缓存或包管理器。

### 5.6 `.gitignore`

至少忽略：

```gitignore
/build/
/out/
/cmake-build-*/
CMakeUserPresets.json
compile_commands.json
.cache/
.idea/
.vscode/
*.log
*.profraw
*.profdata
perf.data*
.env
config/pulsegate.local.yaml
```

示例配置可以提交，真实密钥和本地配置不能提交。

### 5.7 阶段验收

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
git status --short
```

必须满足：

- Debug 构建成功；
- Boost.Asio smoke test 通过；
- `git status` 中没有构建产物；
- README 写明构建命令；
- 编译器警告为 0。

建议 Commit：

```text
build(cmake): bootstrap C++20 Boost.Asio project
test: add initial GoogleTest target
docs: add local development instructions
```

---

## 6. 阶段 1：Boost.Asio 同步 TCP/HTTP 基线

### 6.1 为什么仍然先写同步版本

同步版不是最终架构，而是一份很小的正确性基线。它帮助你先理解：

- `io_context` 与 I/O 对象的关系；
- `tcp::endpoint`、`tcp::acceptor`、`tcp::socket`；
- TCP 是字节流，不保留 HTTP 消息边界；
- EOF、连接重置、短读和短写；
- Asio 对象通过 RAII 自动释放底层句柄；
- HTTP 请求和响应的最小闭环。

这一阶段不使用原生 `socket()`、`accept4()` 和 `close()`。Boost.Asio 仍会调用操作系统网络 API，但资源所有权和错误表示由 C++ 类型管理。

### 6.2 新增文件

```text
app/pulsegate_sync_main.cpp
include/pulsegate/net/endpoint.h
include/pulsegate/net/sync_http_server.h
src/net/endpoint.cpp
src/net/sync_http_server.cpp
tests/integration/sync_http_server_test.cpp
```

同步基线建议构建为独立可执行文件，后续异步主程序不依赖它：

```cmake
add_executable(pulsegate_sync_baseline)
target_sources(
  pulsegate_sync_baseline
  PRIVATE
    pulsegate_sync_main.cpp
)
target_link_libraries(
  pulsegate_sync_baseline
  PRIVATE
    pulsegate_net
    pulsegate_project_options
)
```

### 6.3 Asio 基本类型

统一别名写在实现文件或小型项目头中，不要在公共头文件全局污染命名空间：

```cpp
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using ErrorCode = boost::system::error_code;
```

同步服务器最小流程：

```cpp
asio::io_context context;
tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), 8080);
tcp::acceptor acceptor(context, endpoint);

for (;;) {
    ErrorCode accept_error;
    tcp::socket socket = acceptor.accept(accept_error);
    if (accept_error) {
        // 记录并按错误类型决定继续或退出
        continue;
    }

    handleConnection(socket);
}
```

即使同步操作也建议使用接收 `error_code&` 的重载处理预期网络错误，避免把客户端断开当作异常路径。初始化配置错误、端口占用等启动失败可以抛异常，并在 `main()` 统一捕获。

### 6.4 最小连接处理

```cpp
void handleConnection(tcp::socket& socket) {
    std::array<char, 4096> storage{};
    std::string input;
    ErrorCode error;

    while (input.find("\r\n\r\n") == std::string::npos) {
        const auto count = socket.read_some(asio::buffer(storage), error);

        if (error == asio::error::eof) {
            return;
        }
        if (error) {
            return;
        }

        input.append(storage.data(), count);
        if (input.size() > 16 * 1024) {
            // 返回 431 或关闭连接
            return;
        }
    }

    const std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 12\r\n"
        "Connection: close\r\n"
        "\r\n"
        "hello world\n";

    asio::write(socket, asio::buffer(response), error);
}
```

`socket.write_some()` 可能只写出部分数据；`asio::write()` 是组合操作，会继续调用底层写操作直到满足完成条件或发生错误。必须知道二者差别，不能因为使用库就忽略短写语义。

### 6.5 Endpoint 与启动配置

业务配置不要到处传字符串：

```cpp
struct ListenConfig {
    std::string host{"127.0.0.1"};
    std::uint16_t port{8080};
    int backlog{1024};
};

tcp::endpoint makeEndpoint(
    const ListenConfig& config,
    ErrorCode& error
);
```

启动时明确设置：

- `reuse_address`；
- backlog；
- IPv4/IPv6 策略；
- 端口为 `0` 时读取实际分配端口，方便集成测试；
- bind/listen 失败时输出包含地址和 `error.message()` 的诊断。

### 6.6 本阶段测试

手工验证：

```bash
./build/debug/app/pulsegate_sync_baseline \
  --listen 127.0.0.1:8080

curl -v http://127.0.0.1:8080/
printf 'GET / HTTP/1.1\r\nHost: localhost\r\n\r\n' \
  | nc 127.0.0.1 8080
```

自动测试：

- 启动服务器并绑定临时端口；
- 发送合法请求，断言状态行、Header 和 Body；
- 请求 Header 分段发送仍能接收完整；
- 客户端在 Header 中途关闭，服务器不崩溃；
- 客户端提前关闭响应读取，进程不终止；
- 超过 Header 上限时连接被拒绝；
- 服务器对象析构后端口可以重新绑定。

### 6.7 阶段验收

- 连续处理 100 个串行请求；
- `curl -v` 得到合法响应；
- ASan/UBSan 测试通过；
- README 明确它是同步基线；
- 不宣传并发性能；
- 异步阶段开始后，主程序不链接同步 Server。

建议 Commit：

```text
feat(net): add Boost.Asio synchronous TCP baseline
feat(http): serve minimal synchronous HTTP response
test(integration): verify sync server lifecycle
docs: explain Asio synchronous operation semantics
```

---

## 7. 阶段 2A：HTTP/1.1 增量解析

HTTP Parser 保持与 Boost.Asio 解耦。它只接收字节，不知道字节来自 Socket、测试文件还是 Fuzzer。这样既保留协议学习深度，也能快速单元测试。

### 7.1 新增文件

```text
include/pulsegate/net/buffer.h
include/pulsegate/http/headers.h
include/pulsegate/http/http_request.h
include/pulsegate/http/http_response.h
include/pulsegate/http/http_parser.h
src/net/buffer.cpp
src/http/headers.cpp
src/http/http_request.cpp
src/http/http_response.cpp
src/http/http_parser.cpp
tests/unit/buffer_test.cpp
tests/unit/http_parser_test.cpp
tests/testdata/http/
```

### 7.2 面向 Asio 的 `Buffer`

不要每解析一行就执行 `string.erase(0, n)`，否则会重复移动数据。Buffer 使用读写下标，并提供适合 `async_read_some()` 的可写区域：

```cpp
class Buffer {
public:
    explicit Buffer(std::size_t initial_size = 4096);

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
};
```

与 Asio 连接：

```cpp
auto writable = input_.prepare(4096);
const auto count = co_await socket_.async_read_some(
    asio::buffer(writable.data(), writable.size()),
    asio::use_awaitable
);
input_.commit(count);
```

约束：

- `prepare()` 返回的 `span` 在下一次可能扩容的操作前有效；
- `commit(count)` 的 count 不能超过上次可写区域；
- Parser 只 `consume()` 已确认属于当前请求的字节；
- 一个请求完成后，Buffer 中可能已有下一个请求的数据；
- 单连接 Buffer 有绝对上限，不能因恶意输入无限扩容。

### 7.3 请求对象

```cpp
enum class HttpMethod {
    Get,
    Head,
    Post,
    Put,
    Delete,
    Options,
    Unknown
};

struct HttpRequest {
    HttpMethod method{HttpMethod::Unknown};
    std::string target;
    int version_major{1};
    int version_minor{1};
    Headers headers;
    std::string body;

    [[nodiscard]] bool keepAlive() const;
};
```

Header 名称大小写不敏感。存储时可以统一转为小写，值保留原内容。必须限制：

- 请求行最大长度；
- Header 总字节数；
- Header 数量；
- 单个 Header 长度；
- Body 最大长度。

限制由配置提供，并设置不能被配置突破的安全上限。

### 7.4 解析状态机

```cpp
enum class ParseState {
    RequestLine,
    Headers,
    Body,
    Complete,
    Error
};

enum class ParseResult {
    NeedMore,
    Complete,
    BadRequest,
    HeaderTooLarge,
    BodyTooLarge,
    UnsupportedTransferEncoding
};

class HttpParser {
public:
    ParseResult parse(Buffer& input);
    [[nodiscard]] const HttpRequest& request() const noexcept;
    HttpRequest takeRequest();
    void reset();

private:
    ParseResult parseRequestLine(std::string_view line);
    ParseResult parseHeader(std::string_view line);

    ParseState state_{ParseState::RequestLine};
    HttpRequest request_;
    std::size_t expected_body_bytes_{0};
    std::size_t header_bytes_{0};
};
```

处理顺序：

```text
RequestLine
  | CRLF
  v
Headers -- empty line --> Body or Complete
  | malformed
  v
Error

Body -- Content-Length bytes --> Complete
```

v0.2 中：

- 支持 `Content-Length`；
- 拒绝冲突的多个 `Content-Length`；
- 暂不支持请求 `Transfer-Encoding: chunked`，返回固定错误；
- 一个请求完成后不丢弃 Buffer 中的后续字节；
- Keep-Alive 循环每次 `takeRequest()` 后重置 Parser；
- 明确 HTTP/1.0 与 HTTP/1.1 的连接关闭规则。

### 7.5 响应序列化

```cpp
struct HttpResponse {
    int status_code{200};
    std::string reason{"OK"};
    Headers headers;
    std::string body;
    bool close_connection{false};

    [[nodiscard]] std::string serialize(bool head_request) const;
};
```

初版返回拥有所有字节的 `std::string`，确保它在整个 `co_await async_write()` 期间存活。后期 profile 证明复制是热点后，再引入 buffer sequence 或 serializer。

序列化时自动生成或校验：

- 状态行；
- `Content-Length`；
- `Connection`；
- Header 后的空行；
- HEAD 请求不发送 Body，但长度表示对应 GET Body；
- Header 值禁止注入裸 CR/LF。

### 7.6 必须完成的测试矩阵

| 场景 | 期望 |
|---|---|
| 完整 GET 一次输入 | Complete |
| 每次只输入一个字节 | 最终 Complete |
| `\r\n\r\n` 跨 Buffer 边界 | Complete |
| Header 名称混合大小写 | 可查询 |
| 缺失 Host 的 HTTP/1.1 | 400 |
| 非法请求行 | 400 |
| 超长 Header | 431 |
| Body 小于 Content-Length | NeedMore |
| Body 等于 Content-Length | Complete |
| Body 超限 | 413 |
| 冲突 Content-Length | 400 |
| 不支持的 Transfer-Encoding | 固定为 501 或 400 |
| 两个请求连续输入 | 第一个完成且第二个字节保留 |
| Header 值含裸 CR/LF | 400 |

建议 Commit：

```text
feat(net): add Asio-friendly indexed byte buffer
feat(http): add incremental HTTP request parser
feat(http): serialize owned HTTP responses
test(http): cover fragmented and malformed requests
```

---

## 8. 阶段 2B：单线程 Boost.Asio 协程服务器

### 8.1 目标与执行模型

把同步基线升级为一个线程管理大量连接：

```text
io_context.run()
  |
  +--> listener coroutine
  |       \--> async_accept
  |
  +--> session coroutine A
  |       \--> async_read_some / async_write
  |
  +--> session coroutine B
          \--> async_read_some / async_write
```

异步操作的调用只负责发起；完成处理只有在某个线程执行 `io_context::run()` 时才会发生。单线程阶段让所有协程恢复都在同一线程，先验证生命周期和协议正确性，再引入并发。

### 8.2 新增文件

```text
include/pulsegate/runtime/coroutine_guard.h
include/pulsegate/net/listener.h
include/pulsegate/net/tcp_session.h
include/pulsegate/http/http_server.h
src/runtime/coroutine_guard.cpp
src/net/listener.cpp
src/net/tcp_session.cpp
src/http/http_server.cpp
app/pulsegate_main.cpp
tests/integration/async_http_server_test.cpp
```

### 8.3 协程约定

项目统一别名：

```cpp
namespace pulsegate::net {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using ErrorCode = boost::system::error_code;

template <typename T = void>
using Awaitable = asio::awaitable<T>;

inline constexpr auto use_awaitable = asio::use_awaitable;

}  // namespace pulsegate::net
```

规则：

- 协程函数返回 `asio::awaitable<T>`；
- 函数名使用动词，例如 `acceptLoop()`、`readRequest()`；
- 每个 `co_await` 前确认被引用对象会活到完成；
- 网络断开、取消、超时用 `error_code` 表示；
- 协程顶层统一捕获未知异常；
- 不使用无错误收口的裸 `detached`。

### 8.4 `CoroutineGuard`

`co_spawn()` 的完成处理器必须收口异常：

```cpp
using CoroutineErrorHandler =
    std::function<void(std::string_view, std::exception_ptr)>;

template <typename Executor, typename Factory>
void spawnGuarded(
    const Executor& executor,
    std::string operation,
    Factory&& factory,
    CoroutineErrorHandler on_error
) {
    boost::asio::co_spawn(
        executor,
        std::forward<Factory>(factory)(),
        [
            operation = std::move(operation),
            on_error = std::move(on_error)
        ](std::exception_ptr error) mutable {
            if (error) {
                on_error(operation, error);
            }
        }
    );
}
```

不要在完成处理器中简单 `std::rethrow_exception()` 后无人捕获。日志要包含操作名称，但不能把客户端正常 EOF 记为 Critical。

### 8.5 `Listener`

```cpp
class Listener
    : public std::enable_shared_from_this<Listener> {
public:
    using SessionFactory = std::function<void(tcp::socket)>;

    Listener(
        asio::io_context& context,
        tcp::endpoint endpoint,
        SessionFactory session_factory
    );

    void start();
    void stop();
    [[nodiscard]] tcp::endpoint localEndpoint() const;

private:
    Awaitable<void> acceptLoop();
    void stopInExecutor();

    asio::io_context& context_;
    asio::strand<asio::io_context::executor_type> strand_;
    tcp::acceptor acceptor_;
    asio::steady_timer retry_timer_;
    SessionFactory session_factory_;
    bool stopping_{false};
};
```

核心接受循环：

```cpp
Awaitable<void> Listener::acceptLoop() {
    while (!stopping_) {
        ErrorCode error;

        auto executor = asio::make_strand(context_);
        tcp::socket socket(executor);

        co_await acceptor_.async_accept(
            socket,
            asio::redirect_error(use_awaitable, error)
        );

        if (error == asio::error::operation_aborted && stopping_) {
            co_return;
        }
        if (error) {
            // 记录并退避，避免 EMFILE 等持续错误造成忙循环。
            retry_timer_.expires_after(std::chrono::milliseconds(50));
            co_await retry_timer_.async_wait(
                asio::redirect_error(use_awaitable, error)
            );
            if (stopping_) {
                co_return;
            }
            continue;
        }

        session_factory_(std::move(socket));
    }
}
```

`Listener` 构造时先创建 `strand_(asio::make_strand(context))`，再用该 strand 构造 `acceptor_`；`start()` 在 Listener strand 上 `co_spawn`。`stop()` 可以从任意线程调用，但只执行：

```cpp
void Listener::stop() {
    auto self = shared_from_this();
    asio::dispatch(strand_, [self] {
        self->stopInExecutor();
    });
}
```

`stopInExecutor()` 设置标志，取消 retry Timer，并取消/关闭 Acceptor。这样 stop 与 accept 完成不会并发修改 Listener。虽然当前只运行一个线程，仍让 Listener 和每个 Socket 使用明确的 `strand` Executor，为阶段 4 建立稳定约束。

### 8.6 `HttpSession`

```cpp
class HttpSession
    : public std::enable_shared_from_this<HttpSession> {
public:
    HttpSession(
        tcp::socket socket,
        const Router& router,
        SessionLimits limits
    );

    void start();
    void stop();

private:
    Awaitable<void> run();
    Awaitable<std::optional<HttpRequest>> readRequest();
    Awaitable<void> writeResponse(const HttpResponse& response);
    void close();

    tcp::socket socket_;
    const Router& router_;
    SessionLimits limits_;
    Buffer input_;
    HttpParser parser_;
    bool stopping_{false};
};
```

安全启动：

```cpp
void HttpSession::start() {
    auto self = shared_from_this();
    spawnGuarded(
        socket_.get_executor(),
        "http_session",
        [self]() -> Awaitable<void> {
            co_await self->run();
        },
        [self](std::string_view operation, std::exception_ptr error) {
            self->close();
            logCoroutineFailure(operation, error);
        }
    );
}
```

捕获 `self` 保证对象活到协程结束。Session 完成后不能残留 Timer 或回调继续强持有 `self`。

### 8.7 读取与 Keep-Alive 循环

```cpp
Awaitable<std::optional<HttpRequest>> HttpSession::readRequest() {
    for (;;) {
        const auto result = parser_.parse(input_);

        if (result == ParseResult::Complete) {
            co_return parser_.takeRequest();
        }
        if (result != ParseResult::NeedMore) {
            co_return std::nullopt;
        }

        auto writable = input_.prepare(4096);
        ErrorCode error;
        const auto count = co_await socket_.async_read_some(
            asio::buffer(writable.data(), writable.size()),
            asio::redirect_error(use_awaitable, error)
        );

        if (error == asio::error::eof) {
            co_return std::nullopt;
        }
        if (error) {
            co_return std::nullopt;
        }

        input_.commit(count);
    }
}
```

Session 主循环：

```cpp
Awaitable<void> HttpSession::run() {
    while (!stopping_) {
        auto request = co_await readRequest();
        if (!request) {
            break;
        }

        auto response = router_.handle(*request);
        const bool close_after_response =
            response.close_connection || !request->keepAlive();

        co_await writeResponse(response);

        if (close_after_response) {
            break;
        }

        parser_.reset();
    }

    close();
}
```

注意：

- Parser 完成时 Buffer 可能已经包含下一个请求；
- 第一版顺序处理 Pipelining，不并行响应；
- 写入字符串必须活过整个 `co_await asio::async_write()`；
- Session 同一时间只能有一个读和一个写的明确策略；
- 不允许两个协程同时写同一个 Socket；
- 关闭操作必须幂等。

### 8.8 写响应

```cpp
Awaitable<void> HttpSession::writeResponse(
    const HttpResponse& response
) {
    auto bytes = response.serialize(false);
    ErrorCode error;

    co_await asio::async_write(
        socket_,
        asio::buffer(bytes),
        asio::redirect_error(use_awaitable, error)
    );

    if (error) {
        stopping_ = true;
    }
}
```

`asio::buffer(bytes)` 不拥有 `bytes`，但局部变量位于协程帧中，在 `co_await` 返回前保持存活。以后改为多个 buffer sequence 时也必须保持所有底层内存有效。

### 8.9 测试

- 同时建立 100 个连接，单线程仍能推进；
- 一个客户端发送 1 字节后暂停，不阻塞其他客户端；
- 请求拆成几十段发送；
- 一次输入两个 Keep-Alive 请求；
- 客户端在响应中途关闭；
- Listener stop 能取消等待中的 accept；
- 协程异常被统一记录并清理 Session；
- 没有 `io_context::run()` 时操作不会神奇完成；
- 服务停止后不再创建新 Session。

### 8.10 阶段验收

```bash
wrk -t2 -c100 -d30s --latency \
  http://127.0.0.1:8080/healthz
```

此时只建立基线。保存：

- Git commit；
- Boost、编译器、内核版本；
- Release/Debug 类型；
- wrk 参数；
- RPS、延迟和错误；
- 已知限制：无超时、单 `io_context` 线程。

建议 Commit：

```text
feat(runtime): add guarded coroutine spawning
feat(net): accept TCP sessions with Boost.Asio
feat(http): add coroutine HTTP session
test(http): cover fragmented requests and keep-alive
docs(benchmark): record single-thread Asio baseline
```

---

## 9. 阶段 3：超时、取消与连接生命周期

### 9.1 需要解决的问题

没有超时的异步服务同样会耗尽资源。需要区分：

- Header 读取超时；
- Body 读取超时；
- Keep-Alive 空闲超时；
- 上游 DNS/连接超时；
- 上游响应超时；
- 服务停止导致的取消；
- 对端主动关闭；
- 资源上限导致的拒绝。

协议超时使用 `std::chrono::steady_clock` 和 `asio::steady_timer`，日志时间才使用 wall clock。

### 9.2 不要误解取消

Asio 的 `socket.cancel()` 会请求取消该 Socket 上未完成的异步操作。需要注意：

- 操作可能已经完成，取消与完成存在竞态；
- 回调/协程仍会恢复，并返回成功或 `operation_aborted`；
- 取消后不能直接销毁仍被协程引用的数据；
- Session 必须在自己的 Executor 上串行修改状态；
- 超时错误与服务停机取消在业务语义上不同；
- 一个旧 Timer 不能错误取消下一次新操作。

### 9.3 `Deadline`

第一版使用“Timer + generation”避免陈旧回调：

```cpp
class Deadline
    : public std::enable_shared_from_this<Deadline> {
public:
    explicit Deadline(asio::any_io_executor executor);

    template <typename Rep, typename Period, typename OnExpire>
    void arm(
        std::chrono::duration<Rep, Period> timeout,
        OnExpire&& on_expire
    );

    void disarm();

private:
    asio::steady_timer timer_;
    std::uint64_t generation_{0};
};
```

语义：

1. `arm()` 增加 generation；
2. 设置 `expires_after()`；
3. 启动 `async_wait()`；
4. 回调检查 `error != operation_aborted` 且 generation 匹配；
5. `disarm()` 再增加 generation 并取消 Timer；
6. 所有方法只在 Session strand 中调用，所以不需要 Mutex。

伪实现：

```cpp
template <typename Rep, typename Period, typename OnExpire>
void Deadline::arm(
    std::chrono::duration<Rep, Period> timeout,
    OnExpire&& on_expire
) {
    const auto ticket = ++generation_;
    timer_.expires_after(timeout);

    timer_.async_wait(
        [
            self = shared_from_this(),
            ticket,
            callback = std::forward<OnExpire>(on_expire)
        ](const ErrorCode& error) mutable {
            if (!error && ticket == self->generation_) {
                callback();
            }
        }
    );
}
```

Session 保存 `std::shared_ptr<Deadline>`。Timer Handler 捕获 Deadline 自身，保证取消完成 Handler 被调度前对象不会销毁；过期回调再通过 `weak_ptr<HttpSession>` 访问 Session。只把 Deadline 作为普通值成员并在 Handler 捕获裸 `this` 是不安全的，因为 `cancel()` 后 Handler 仍会以 `operation_aborted` 被投递。

### 9.4 给读操作加期限

```cpp
deadline_->arm(
    limits_.header_timeout,
    [weak = weak_from_this()] {
        if (auto self = weak.lock()) {
            self->last_stop_reason_ = StopReason::HeaderTimeout;
            ErrorCode ignored;
            self->socket_.cancel(ignored);
        }
    }
);

ErrorCode error;
const auto count = co_await socket_.async_read_some(
    asio::buffer(writable.data(), writable.size()),
    asio::redirect_error(use_awaitable, error)
);

deadline_->disarm();
```

真实实现应使用作用域清理器，保证协程异常或提前返回也会 disarm：

```cpp
auto cleanup = ScopeExit([this] {
    deadline_->disarm();
});
```

不要仅根据 `operation_aborted` 推断“发生了超时”，因为优雅停机也会取消操作。Session 保存明确的 `StopReason`。

### 9.5 Session 状态

```cpp
enum class SessionState {
    Created,
    Running,
    Draining,
    Closing,
    Closed
};

enum class StopReason {
    None,
    PeerClosed,
    ProtocolError,
    HeaderTimeout,
    BodyTimeout,
    IdleTimeout,
    ServerShutdown,
    ResourceLimit,
    InternalError
};
```

`close()`：

1. 如果已 Closed，直接返回；
2. 设置 Closing；
3. disarm 所有 Timer；
4. `socket.cancel()`；
5. 尝试 `shutdown(tcp::socket::shutdown_both)`；
6. `socket.close()`；
7. 设置 Closed；
8. 通知 Session Registry 移除；
9. Metrics 记录一次关闭原因。

网络关闭错误通常不覆盖原始 StopReason。

### 9.6 连接 Registry

为了优雅停机和连接上限，需要统一追踪 Session：

```cpp
class SessionRegistry {
public:
    bool tryAdd(const std::shared_ptr<HttpSession>& session);
    void remove(SessionId id);
    void beginDrain();
    void forceCloseAll();
    [[nodiscard]] std::size_t size() const;
};
```

Registry 不应永久强持有已经结束的连接。可以：

- 在统一 strand 中管理 `shared_ptr`；
- 或保存 `weak_ptr` 并定期清理；
- 任何方案都必须证明移除路径一定执行。

### 9.7 测试

- 慢速不完整 Header 超时；
- Body 超时与 Header 超时区分；
- 活跃请求完成后进入 idle timeout；
- 读操作刚完成时 Timer 触发不误取消下一读；
- stop 与 timeout 同时发生只关闭一次；
- 取消后协程正常恢复并退出；
- Registry 连接数最终归零；
- 10,000 次建立/取消无泄漏；
- ASan 和 TSan 通过。

时间策略单元测试使用 FakeClock；真正的 `steady_timer` 只做少量短时间集成测试，避免测试套件大量真实等待。

### 9.8 阶段验收

- 慢 Header、慢 Body 和空闲 Keep-Alive 都会被回收；
- 每次关闭有唯一 StopReason；
- Listener 与 Session stop 均幂等；
- 无未观察的协程异常；
- 服务停止后 `io_context` 可以自然退出。

建议 Commit：

```text
feat(net): add generation-safe Asio deadline
feat(http): enforce header body and idle timeouts
feat(server): track live sessions for coordinated shutdown
test(net): cover timeout cancellation races
```

---

## 10. 阶段 4：多线程 `io_context` 与 `strand`

### 10.1 并发模型

主线使用一个 `io_context`，由 N 个工作线程共同执行：

```text
                    +--> worker thread 0: io_context.run()
io_context queue ---+--> worker thread 1: io_context.run()
                    +--> worker thread 2: io_context.run()
                    +--> worker thread 3: io_context.run()

session A strand: handlers never execute concurrently
session B strand: handlers never execute concurrently
```

`io_context` 可以被多个线程并发运行，这意味着任意两个普通 Handler 可能同时执行。每条 Session 的 Socket、Parser、Buffer、Timer 和状态都绑定到同一个 `strand`，从结构上保证该 Session 的 Handler 串行执行。

`strand` 保证不并发执行，不保证同一 Session 永远停留在同一个操作系统线程。因此：

- 不把 thread-local 状态当 Session 状态；
- 不用线程 ID 判断 Session 所有权；
- 依赖关联 Executor，而不是手写“当前线程检查”；
- 跨 Session 的共享对象仍需独立同步策略。

### 10.2 `AsioRuntime`

```cpp
class AsioRuntime {
public:
    explicit AsioRuntime(std::size_t thread_count);
    ~AsioRuntime();

    AsioRuntime(const AsioRuntime&) = delete;
    AsioRuntime& operator=(const AsioRuntime&) = delete;

    asio::io_context& context() noexcept;
    void start();
    void requestStop();
    void join();

private:
    using WorkGuard = asio::executor_work_guard<
        asio::io_context::executor_type
    >;

    asio::io_context context_;
    std::optional<WorkGuard> work_guard_;
    std::vector<std::jthread> workers_;
    std::size_t thread_count_;
    std::atomic_bool started_{false};
};
```

启动：

```cpp
void AsioRuntime::start() {
    if (started_.exchange(true)) {
        throw std::logic_error("runtime already started");
    }

    work_guard_.emplace(asio::make_work_guard(context_));

    workers_.reserve(thread_count_);
    for (std::size_t index = 0; index < thread_count_; ++index) {
        workers_.emplace_back([this, index] {
            try {
                context_.run();
            } catch (...) {
                reportRuntimeFailure(index, std::current_exception());
                requestStop();
            }
        });
    }
}
```

停止要区分：

- **优雅 drain**：先停 Listener，等待 Session/代理事务完成；
- **运行时 stop**：所有业务结束后 reset work guard，让 `run()` 自然返回；
- **强制 stop**：超过 deadline 后 `context_.stop()`，仅作最终兜底。

不要一收到 SIGTERM 就立刻 `io_context.stop()`，否则未执行 Handler 可能永远不再运行，清理和 access log 都无法完成。

### 10.3 Session strand

Listener 接受连接时创建 Session strand：

```cpp
auto session_executor = asio::make_strand(context_);
tcp::socket socket(session_executor);

co_await acceptor_.async_accept(
    socket,
    asio::redirect_error(use_awaitable, error)
);
```

Session 的：

- `start()`；
- `stop()`；
- Timer；
- Socket 操作；
- Parser/Buffer；
- 当前 ProxySession 的弱引用和取消请求；

都必须在 `socket.get_executor()` 上执行。阶段 6 的单请求 ProxySession 也继承这个 Executor；阶段 7 的共享上游池和池连接使用各自 strand。外部线程请求停止 HttpSession 时：

```cpp
void HttpSession::stop() {
    auto self = shared_from_this();
    asio::dispatch(
        socket_.get_executor(),
        [self] {
            self->stopInExecutor(StopReason::ServerShutdown);
        }
    );
}
```

使用 `dispatch()` 允许已在关联 Executor 中时立即执行；需要强制延后时使用 `post()`。调用者不能依赖 stop 在函数返回前已经完成。

### 10.4 共享数据策略

| 数据 | 推荐同步方式 |
|---|---|
| 单 Session Buffer/Parser | Session strand |
| Listener 状态 | Listener executor/strand |
| Session Registry | 独立 strand 或 Mutex |
| 全局配置 | 不可变快照 + 原子替换 |
| Metrics | 每线程/每 shard 计数后聚合 |
| Cache | 分片 Mutex |
| Upstream Pool | Pool shard strand + connection strand |
| Health 状态 | 原子快照或专用 strand |

不要为了“使用 strand”把整个网关放到一个全局 strand，那会退化成单线程。

### 10.5 阻塞任务

`io_context` 工作线程中禁止：

- 同步 DNS；
- 同步文件大读取；
- 数据库阻塞调用；
- `sleep_for()`；
- 执行外部命令；
- 大量压缩/哈希等长 CPU 任务。

确实需要阻塞或 CPU 工作时，使用独立的 `asio::thread_pool`：

```cpp
template <
    typename Function,
    typename CompletionToken
>
auto asyncRunBlocking(
    asio::thread_pool& pool,
    Function function,
    CompletionToken&& token
);

auto result = co_await asyncRunBlocking(
    blocking_pool,
    [path] {
        return readBoundedFile(path);
    },
    asio::use_awaitable
);
```

`asyncRunBlocking()` 使用 `async_initiate`：工作函数在线程池执行，结果或异常通过 completion handler 的 associated executor 返回请求协程。队列必须有容量上限。不要在后台线程直接操作 Session Socket，也不要依赖一次裸 `post(thread_pool, use_awaitable)` 来隐式维持复杂的 Executor 切换。

### 10.6 测试

- 线程数为 1、2、4 均能运行；
- 同一 Session 的 Handler 最大并发数始终为 1；
- 不同 Session 可以并行推进；
- 外部线程调用 stop 被正确投递；
- Session Registry 在高频建立/关闭下正确；
- 停机时所有工作线程 join；
- 某协程意外抛出时 Runtime 有统一诊断；
- TSan 无数据竞争；
- blocking pool 满载时不会无限排队；
- 线程数为 0 的配置被校验拒绝。

可在测试中增加一个只用于断言的并发探针：

```cpp
struct ConcurrencyProbe {
    std::atomic_int active{0};
    std::atomic_int maximum{0};
};
```

进入 Session Handler 时递增，退出时递减，最终断言 maximum 为 1。

### 10.7 性能实验

固定其他条件，只改变 `io_context.run()` 工作线程数：

| I/O 线程 | 连接数 | RPS | P99 | CPU | RSS | 错误 |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 100 | | | | | |
| 2 | 100 | | | | | |
| 4 | 100 | | | | | |
| 8 | 100 | | | | | |

线程数不会永远线性提升性能。需要解释：

- CPU 物理核和 SMT；
- 同一个 `io_context` 调度开销；
- shared cache/metrics 锁竞争；
- 负载生成器是否饱和；
- Session 是否错误进入全局 strand；
- 日志是否成为瓶颈。

### 10.8 可选原生 epoll 实验

为了准备 Linux 网络面试，可以在 `labs/raw_epoll_server/` 单独实现一个 300～500 行实验：

- `socket/bind/listen/accept4`；
- nonblocking；
- `epoll_create1/epoll_ctl/epoll_wait`；
- LT 模式；
- 读到 `EAGAIN`；
- 处理部分写；
- 一个简单 Echo 或固定 HTTP 响应。

实验 README 回答：

| Asio 概念 | Linux 实现概念 |
|---|---|
| `io_context::run()` | 等待并分发内核事件 |
| `tcp::acceptor::async_accept()` | 非阻塞 accept + readiness |
| `async_read_some()` | readiness 后 read/recv |
| `steady_timer` | timer queue / timerfd 等 |
| `post()` | 跨执行器排队与唤醒 |
| `strand` | Handler 串行化约束 |

这个实验不能被主项目链接，CI 可以只做编译和少量功能测试。它用于理解抽象底层，不与 Boost.Asio 重复建设一套生产网络库。

### 10.9 阶段验收

- 多线程 Asio Server 功能与单线程一致；
- Session 无显式 Mutex，依靠 strand 串行化；
- 全局共享状态有明确同步方式；
- TSan 通过；
- 完成线程数对照实验；
- 文档能解释 strand 保证什么、不保证什么。

建议 Commit：

```text
feat(runtime): run one io_context on configurable workers
refactor(net): serialize each session with an Asio strand
test(net): verify per-session non-concurrency
perf(runtime): compare io_context worker counts
docs(lab): add isolated raw epoll experiment
```

### 10.10 `main()` 只负责装配

```cpp
int main(int argc, char** argv) {
    try {
        const auto options = parseCommandLine(argc, argv);
        const auto config = loadAndValidateConfig(options.config_path);

        AsioRuntime runtime(config.server.io_threads);

        auto logger = makeLogger(config.logging);
        auto metrics = std::make_shared<Metrics>();
        auto router = std::make_shared<Router>();
        auto registry = std::make_shared<SessionRegistry>();

        registerLocalRoutes(*router, metrics);

        auto server = std::make_shared<HttpServer>(
            runtime.context(),
            config.server,
            router,
            registry,
            logger,
            metrics
        );

        auto signals = installSignalHandling(
            runtime.context(),
            server,
            runtime
        );

        server->start();
        runtime.start();
        runtime.join();
        return 0;
    } catch (const std::exception& error) {
        writeStartupFailure(error);
        return 1;
    }
}
```

要求：

- `main()` 不实现网络协议和业务逻辑；
- 配置校验在启动线程完成，失败时不启动任何 Worker；
- `signals` 必须活到 Runtime 结束；
- Server、Router、Registry 和 Logger 的生命周期长于所有 Session；
- 正常停机路径先 drain Server，最后才结束 Runtime；
- 启动失败和运行期请求失败使用不同日志与退出策略。

---

## 11. 阶段 5：异步路由与静态资源

### 11.1 分离“路由匹配”与“处理执行”

路由匹配是纯逻辑，Handler 执行可以是异步的：

```cpp
struct RequestContext {
    boost::asio::any_io_executor executor;
    std::string request_id;
    boost::asio::ip::tcp::endpoint peer;
    std::weak_ptr<HttpSession> downstream;
};

using HttpHandler = std::function<
    boost::asio::awaitable<HttpResponse>(
        RequestContext&,
        HttpRequest
    )
>;

struct Route {
    HttpMethod method;
    std::string pattern;
    std::string name;
    HttpHandler handler;
};

class Router {
public:
    void add(Route route);

    [[nodiscard]] const Route* match(
        HttpMethod method,
        std::string_view target
    ) const;

    boost::asio::awaitable<HttpResponse> handle(
        RequestContext& context,
        HttpRequest request
    ) const;
};
```

为什么 Handler 返回 `awaitable<HttpResponse>`：

- 本地 `/healthz` 可以立即 `co_return`；
- 静态文件可以等待受限文件线程池；
- 代理可以异步解析 DNS、连接和读取上游；
- 限流和缓存可以在调用代理前短路；
- Session 不需要知道某个路由是否涉及 I/O。

路由表加载后使用不可变快照。请求热路径只读，不在每次匹配时持全局写锁。

### 11.2 第一批路由

```text
GET  /livez
GET  /readyz
GET  /metrics
GET  /api/version
POST /echo
GET  /static/*
```

本地 Handler 示例：

```cpp
boost::asio::awaitable<HttpResponse> healthHandler(
    RequestContext&,
    HttpRequest
) {
    HttpResponse response;
    response.status_code = 200;
    response.reason = "OK";
    response.body = "ok\n";
    co_return response;
}
```

先支持精确路径和前缀匹配，再实现 `/users/:id`。不要一开始引入正则路由；需要记录 route name，供日志和低基数 Metrics 使用。

### 11.3 Session 调用 Router

```cpp
RequestContext context{
    .executor = socket_.get_executor(),
    .request_id = requestIdGenerator_.next(),
    .peer = peer_endpoint_,
    .downstream = weak_from_this()
};

auto response = co_await router_.handle(
    context,
    std::move(request)
);

co_await writeResponse(response);
```

约束：

- Handler 在请求所属 Executor 中开始；
- Handler 不能保存 `RequestContext&` 到协程结束以后；
- `downstream` 使用 `weak_ptr`，避免 Handler 与 Session 形成环；
- 未知异常在 Router 边界映射为 500，并保留 request ID；
- 客户端断开后，耗时 Handler 应收到取消信号；
- 不能让一个请求的异常终止整个 `io_context` 工作线程。

### 11.4 静态文件服务

需要处理：

- URL 解码；
- `..` 路径穿越；
- 符号链接是否允许；
- 普通文件检查；
- 文件大小上限；
- MIME 类型；
- 404/403/500；
- HEAD；
- 大文件不能一次无限制读入内存。

禁止：

```cpp
root + request.target
```

推荐流程：

```text
URL path
  -> percent decode
  -> reject NUL/backslash according to policy
  -> lexical normalization
  -> resolve against canonical document root
  -> confirm final path remains below root
  -> inspect file type and size
  -> read with configured limit
```

### 11.5 文件 I/O 与 Asio 线程

普通文件读取可能阻塞。第一版采用：

- 小文件上限，例如 256 KiB；
- 专用有界文件线程池；
- 文件读取结果再投递回请求 Executor；
- 队列满时返回 503，不无限排队；
- 路径检查与打开尽量在同一个受控函数中，降低 TOCTOU 风险。

项目接口：

```cpp
class FileService {
public:
    virtual ~FileService() = default;

    virtual boost::asio::awaitable<FileResult> read(
        boost::asio::any_io_executor reply_executor,
        std::filesystem::path path,
        std::size_t maximum_bytes
    ) = 0;
};
```

不要只用一次 `co_await post(file_pool, use_awaitable)` 就假定协程会永久安全地切换执行器。更稳妥的实现是把文件任务包装成一个 Asio composed asynchronous operation：工作在线程池执行，完成 Handler 通过其 associated executor 回到请求 strand。

后期可比较：

- 有界工作池分块读取；
- Boost.Beast `file_body`；
- Linux 专用 `sendfile` adapter。

平台专用优化必须放在可替换实现中，并用 profile 证明收益。

### 11.6 HTTP 错误映射

| 条件 | 状态码 |
|---|---:|
| 请求行/Header 非法 | 400 |
| 未授权（以后可选） | 401 |
| 路径被禁止 | 403 |
| 路由不存在 | 404 |
| 方法不支持 | 405 |
| Body 过大 | 413 |
| Header 过大 | 431 |
| 限流 | 429 |
| 上游失败 | 502 |
| 上游超时 | 504 |
| 服务停止接流量 | 503 |

Router 的错误映射不能覆盖已经开始发送的响应。v0.5 仍采用完整响应后再写出，因此规则较简单；流式代理阶段需要显式追踪 `response_started`。

### 11.7 测试与验收

- 路由匹配单元测试不启动真实网络；
- 异步 Handler 能在等待后返回；
- `/livez` 和 `/readyz` 语义区分；
- `/echo` 能处理拆分请求体；
- 静态文件无法越过 document root；
- HEAD 不发送 Body；
- 文件工作池满时快速失败；
- Handler 异常得到 500，Session 仍按连接策略清理；
- Keep-Alive 连续多个请求正确；
- 错误响应包含合法长度和 request ID。

建议 Commit：

```text
feat(http): add coroutine-based route handlers
feat(http): add live ready and echo endpoints
feat(http): serve bounded static files asynchronously
test(http): reject path traversal attempts
```

---

## 12. 阶段 6：Boost.Asio 协程式反向代理

这是项目从“HTTP Server 练习”变成“实用网关”的关键阶段。

### 12.1 请求路径

```text
downstream HttpSession
  -> match proxy route
  -> choose healthy upstream
  -> async_resolve
  -> async_connect
  -> rewrite and async_write request
  -> async_read upstream response
  -> return/stream response downstream
  -> close or release upstream connection
```

整个过程必须异步。禁止在 `io_context` 工作线程中使用同步 DNS、同步 connect 或阻塞等待上游。

### 12.2 拆成两个小版本

#### v0.6.0-a：完整缓冲的最小代理

- 只代理 GET/HEAD；
- 每次请求新建上游连接；
- Round Robin 选择健康上游；
- `async_resolve` + `async_connect`；
- 支持 `Content-Length` 响应；
- 限制上游 Header 和 Body；
- 设置 DNS、连接、响应超时；
- 响应完整解析后再写下游；
- 完成后关闭上游连接。

这版内存边界容易证明，先解决正确性和错误映射。

#### v0.6.0-b：流式基础代理

- GET/HEAD/POST/PUT/DELETE；
- 有上限的请求 Body 转发；
- 上游 Keep-Alive；
- 支持 chunked 响应；
- 上游 Header 完成后尽快写下游；
- 高低水位背压；
- 观察到下游关闭或服务停机时取消上游事务；
- 为后续连接池提供可复用连接状态。

### 12.3 上游响应 Parser

自研 HTTP Parser 需要补齐响应方向：

```text
include/pulsegate/http/http_response_parser.h
src/http/http_response_parser.cpp
tests/unit/http_response_parser_test.cpp
```

```cpp
class HttpResponseParser {
public:
    ResponseParseResult parse(Buffer& input);
    [[nodiscard]] bool headerComplete() const noexcept;
    [[nodiscard]] bool messageComplete() const noexcept;
    HttpResponse takeResponse();
    void reset();
};
```

必须处理或明确拒绝：

- `Content-Length`；
- `Transfer-Encoding: chunked`；
- HEAD 响应；
- 1xx 中间响应；
- 204/304 无 Body；
- 连接关闭界定 Body；
- Header/Body 上限；
- 冲突长度；
- 上游提前 EOF。

连接能否复用完全依赖响应边界是否可靠确定。

### 12.4 `ProxySession`

最小版本每次请求一个对象：

```cpp
class ProxySession
    : public std::enable_shared_from_this<ProxySession> {
public:
    ProxySession(
        boost::asio::any_io_executor executor,
        UpstreamEndpoint upstream,
        ProxyLimits limits
    );

    boost::asio::awaitable<ProxyResult> execute(
        RequestContext& context,
        HttpRequest request
    );

    void cancel(ProxyStopReason reason);

private:
    boost::asio::awaitable<bool> resolve();
    boost::asio::awaitable<bool> connect();
    boost::asio::awaitable<bool> sendRequest(const HttpRequest& request);
    boost::asio::awaitable<ProxyResult> readResponse();

    boost::asio::any_io_executor executor_;
    tcp::resolver resolver_;
    tcp::socket socket_;
    std::shared_ptr<Deadline> deadline_;
    Buffer input_;
    HttpResponseParser parser_;
    ProxyState state_{ProxyState::Created};
    ProxyStopReason stop_reason_{ProxyStopReason::None};
};
```

上面接口对应完整缓冲的 v0.6.0-a。流式响应版本把 `execute()` 演进为接收受控 `DownstreamWriter` 的 `forward()`，并显式记录 `response_started`、已写字节和背压状态；不能一边直接写 HttpSession Socket，一边又返回一份完整 `HttpResponse`。

HttpSession 构造 ProxySession 时传入 `socket_.get_executor()`；这个 Executor 已经是该下游 Session 的 strand。ProxySession 的 Resolver、Socket、Deadline 和状态都使用同一个 Executor，因此单次代理事务与下游连接状态按顺序推进，不需要再嵌套一层 strand。下游只持有 `weak_ptr<ProxySession>` 作为当前事务取消句柄；ProxySession 也只弱引用下游，避免环。

如果 ProxySession 被用于不带串行保证的普通 `io_context` Executor，必须在构造入口拒绝这种用法或内部创建 strand，并通过 composed operation 把完成结果送回调用者；不能仅仅添加一个 `strand_` 成员，却让 `execute()` 协程继续运行在调用者的其他 Executor 上。

状态机：

```cpp
enum class ProxyState {
    Created,
    Resolving,
    Connecting,
    SendingRequest,
    ReadingResponseHeaders,
    ReadingResponseBody,
    Complete,
    Failed,
    Cancelled
};
```

即使使用协程，也保留显式状态。它用于：

- 判断取消应该作用于 resolver 还是 socket；
- 生成准确 Metrics；
- 约束重试；
- 防止完成后再次回调；
- 判断是否已经向下游发送响应。

### 12.5 异步解析 DNS

```cpp
boost::system::error_code error;
auto results = co_await resolver_.async_resolve(
    upstream_.host,
    upstream_.service,
    boost::asio::redirect_error(
        boost::asio::use_awaitable,
        error
    )
);
```

需要：

- DNS timeout；
- `resolver_.cancel()`；
- 空结果处理；
- 域名与静态 IP 使用统一 Endpoint 抽象；
- 不把解析失败算作 TCP connect failure；
- 后期可增加带 TTL 的解析缓存，但不要无限缓存 DNS。

### 12.6 异步连接

```cpp
boost::system::error_code error;
const auto endpoint = co_await boost::asio::async_connect(
    socket_,
    resolved_endpoints_,
    boost::asio::redirect_error(
        boost::asio::use_awaitable,
        error
    )
);
```

Boost.Asio 已封装底层非阻塞 connect 和多个解析结果的尝试。项目仍需负责：

- connect deadline；
- 取消；
- 总请求 deadline；
- 记录实际选中的 Endpoint；
- 区分 refused、timeout、network unreachable；
- 失败是否可以选择另一个上游重试。

不再在主线手写 `EINPROGRESS`、`EPOLLOUT` 和 `SO_ERROR`；这些知识放进 raw epoll 实验与面试章节。

### 12.7 请求写入

```cpp
auto bytes = serializeUpstreamRequest(request, upstream_);

boost::system::error_code error;
co_await boost::asio::async_write(
    socket_,
    boost::asio::buffer(bytes),
    boost::asio::redirect_error(
        boost::asio::use_awaitable,
        error
    )
);
```

`bytes` 必须位于协程帧中直到写完成。记录：

- `request_bytes_sent`；
- 是否已发送完整 Header；
- 是否已发送任何 Body；
- 当前请求能否安全重试。

v0.6 的 `HttpRequest` 仍拥有完整且有上限的请求 Body，因此写上游时可以分成 buffer sequence，但这不等于真正的请求流式代理。若后期要边读下游 Body 边写上游，Parser 必须提供 Header 完成和 Body chunk 事件，并使用有界管道形成背压；不要在简历中把完整缓冲实现描述成“双向流式”。

### 12.8 Header 处理

代理时移除或重建 hop-by-hop Header：

- `Connection`；
- `Keep-Alive`；
- `Proxy-Authenticate`；
- `Proxy-Authorization`；
- `TE`；
- `Trailer`；
- `Transfer-Encoding`；
- `Upgrade`；
- `Connection` 点名的其他字段。

增加：

- `Host`；
- `X-Forwarded-For`；
- `X-Forwarded-Proto`；
- `X-Request-Id`。

只有网关位于可信代理链后时才接受外部 `X-Forwarded-For`；否则从实际 peer endpoint 重新生成。

初版不支持 WebSocket Upgrade 时，明确返回 501 或按配置拒绝。

### 12.9 下游取消传播

HTTP 请求已经完整读取后，如果 Session 只是在等待上游，客户端发送 FIN 不一定会立刻被观察到，因为此时没有下游读写操作正在完成。第一版必须诚实定义：

- 服务停机能立即传播取消；
- 下游写失败后能立即取消仍在运行的上游操作；
- 已经观察到的 reset/EOF 能传播；
- 等待上游期间的静默关闭，可能到下一次下游 I/O 才被发现。

不要为了“立即检测”在同一个 Socket 上无约束地启动第二个 `async_read_some()`，它可能消费 Pipelining 数据，并与正常读取产生并发操作。若后期实现 disconnect watcher，需要单独设计 `async_wait`、peek/平台能力和取消测试。

当 HttpSession 发现客户端关闭：

```cpp
if (auto proxy = current_proxy_.lock()) {
    proxy->cancel(ProxyStopReason::DownstreamClosed);
}
```

`cancel()` 内部使用 `dispatch(executor_, ...)`：

- 取消 resolver；
- disarm Timer；
- cancel/close 上游 Socket；
- 设置唯一 StopReason；
- 让等待中的协程以 `operation_aborted` 恢复；
- 协程根据 StopReason 决定是否还生成下游错误响应。

不能从其他线程直接并发关闭由 ProxySession Executor 管理的 Socket。

### 12.10 重试规则

- 默认只重试幂等方法；
- 已向下游发送响应字节后不能换上游；
- POST 默认不自动重试；
- 请求 Body 已部分发送时，即使连接失败也可能已被上游处理；
- 设置最大尝试次数；
- 总 deadline 覆盖解析、连接和所有尝试；
- 每次尝试排除已经失败的 Endpoint；
- 熔断、健康检查与单请求重试分别统计；
- 重试原因写进 debug log，不泄露敏感请求内容。

### 12.11 错误映射

| Asio/代理结果 | 下游状态 |
|---|---:|
| DNS 失败 | 502 |
| connect refused | 502 |
| connect timeout | 504 |
| response timeout | 504 |
| 上游协议错误 | 502 |
| 上游 Body 超限 | 502 |
| 无健康上游 | 503 |
| 下游已关闭 | 不再发送 |
| 服务 draining | 503 |

必须在代码中使用结构化内部错误枚举，再统一映射 HTTP；不要到处直接比较错误字符串。

### 12.12 集成测试拓扑

```text
test client --> PulseGate --> mock upstream A
                         \--> mock upstream B
```

`tools/mock_upstream.py` 支持：

- 固定状态码；
- 固定/随机延迟；
- 指定大小 Body；
- 接收后立即断开；
- Header 发送一半后断开；
- Body 发送一半后停住；
- chunked 响应；
- 记录 request ID 和连接 ID。

测试：

- 请求在两个上游间轮询；
- DNS 失败、连接拒绝、超时分别映射；
- 上游响应拆分到任意边界；
- 已观察到的下游断开和服务停机能取消 resolver/socket/timer；
- hop-by-hop Header 被移除；
- request ID 创建或透传；
- POST Body 不因分片丢失；
- 非幂等请求不会被重复；
- 上游超大 Body 不造成内存无界增长；
- 协程完成后 ProxySession 被释放。

建议 Commit：

```text
feat(http): parse upstream HTTP responses
feat(gateway): proxy with Asio resolver and TCP coroutines
feat(gateway): propagate downstream cancellation
fix(proxy): remove hop-by-hop headers
test(proxy): cover DNS connect timeout and partial response
```

---

## 13. 阶段 7：Asio 健康检查与上游连接池

### 13.1 主动健康检查协程

配置：

```yaml
upstreams:
  - name: demo
    health_check:
      path: /healthz
      interval_ms: 2000
      timeout_ms: 500
      healthy_threshold: 2
      unhealthy_threshold: 3
```

每个上游组由一个 HealthChecker 协程管理：

```cpp
class HealthChecker
    : public std::enable_shared_from_this<HealthChecker> {
public:
    HealthChecker(
        boost::asio::any_io_executor executor,
        HealthCheckConfig config,
        std::vector<std::shared_ptr<UpstreamEndpoint>> endpoints
    );

    void start();
    void stop();

private:
    boost::asio::awaitable<void> run();
    boost::asio::awaitable<ProbeResult> probe(
        const UpstreamEndpoint& endpoint
    );

    boost::asio::strand<boost::asio::any_io_executor> strand_;
    boost::asio::steady_timer interval_timer_;
    bool stopping_{false};
};
```

运行循环：

```text
wait interval
  -> probe endpoints asynchronously
  -> apply success/failure thresholds
  -> publish immutable health snapshot
  -> repeat
```

状态不能由一次抖动决定：

```text
Healthy
  -- N consecutive failures --> Unhealthy

Unhealthy
  -- M consecutive successes --> Healthy
```

要求：

- `steady_timer` 等待可被 stop 取消；
- Probe 有独立 connect/response timeout；
- 单个 Endpoint 的慢 Probe 不应永久阻止其他 Endpoint；
- 控制同时 Probe 数量；
- 避免把全部 Endpoint 检查串成过长一轮；
- 停机后不再安排下一轮；
- Probe 连接与业务连接池隔离，避免池满后永远无法恢复健康。

可以对各 Endpoint `co_spawn` 子任务，但子任务结果必须回到 HealthChecker strand 汇总。不要从多个协程无同步写同一计数器。

### 13.2 健康状态快照

业务热路径不应为每次选择上游都等待 HealthChecker strand：

```cpp
struct EndpointHealth {
    bool healthy;
    std::uint32_t consecutive_successes;
    std::uint32_t consecutive_failures;
    std::chrono::steady_clock::time_point changed_at;
};

struct HealthSnapshot {
    std::unordered_map<EndpointId, EndpointHealth> endpoints;
};
```

HealthChecker 更新完成后发布：

```cpp
std::atomic<std::shared_ptr<const HealthSnapshot>> snapshot_;
```

读请求加载不可变快照，无需修改它。若编译器/标准库对 `atomic<shared_ptr>` 支持存在兼容问题，使用 `atomic_load/atomic_store` 自由函数或一个很短的读写锁，并记录 ADR。

### 13.3 被动健康信号

业务请求提供：

- DNS/connect failure；
- connect timeout；
- reset；
- response timeout；
- 协议错误；
- 5xx 是否计入失败由配置决定。

不应计入上游失败：

- 下游客户端取消；
- 本地限流；
- 服务进入 draining；
- 请求本身不合法；
- 本地缓存错误。

ProxySession 通过 `post(health_checker_strand, event)` 上报，不能在请求 strand 直接修改 HealthChecker 状态。

### 13.4 为什么连接池需要自己的 Executor

多个下游 Session 会并发借还上游连接。池和连接各有明确并发边界：

```text
UpstreamPool shard strand
  - idle/busy/waiter collections
  - capacity accounting
  - acquire/release/discard

UpstreamConnection strand
  - socket
  - parser/buffer
  - timer
  - one active transaction
```

第一版每个上游组使用一个 Pool strand；如果 profile 证明它成为热点，再按 Endpoint 或哈希分成多个 shard。不要一开始用无锁结构。

### 13.5 异步连接池接口

连接池操作跨 Executor，设计为 Asio 异步操作，而不是同步返回裸 `shared_ptr`：

```cpp
class UpstreamLease;

class UpstreamPool {
public:
    template <typename CompletionToken>
    auto asyncAcquire(
        EndpointId endpoint,
        std::chrono::steady_clock::time_point deadline,
        CompletionToken&& token
    );

    void releaseReusable(UpstreamLease lease);
    void discard(UpstreamLease lease, DiscardReason reason);

private:
    boost::asio::strand<boost::asio::any_io_executor> strand_;
    std::unordered_map<EndpointId, EndpointPool> pools_;
};
```

协程调用：

```cpp
boost::system::error_code error;
auto lease = co_await pool_.asyncAcquire(
    endpoint.id,
    acquire_deadline,
    boost::asio::redirect_error(
        boost::asio::use_awaitable,
        error
    )
);
```

`asyncAcquire()` 内部可以使用 `async_initiate`：

1. 保存调用者 completion handler；
2. `post()` 到 Pool strand；
3. 有空闲连接则立即完成；
4. 容量未满则异步新建连接；
5. 否则放进有界等待队列；
6. acquire deadline 到期则从等待队列移除；
7. 完成 Handler 按 associated executor 返回调用者。

这是非常值得单独写测试和 ADR 的 composed asynchronous operation。

### 13.6 `UpstreamLease`

```cpp
class UpstreamLease {
public:
    UpstreamLease() = default;
    UpstreamLease(const UpstreamLease&) = delete;
    UpstreamLease& operator=(const UpstreamLease&) = delete;
    UpstreamLease(UpstreamLease&&) noexcept = default;
    UpstreamLease& operator=(UpstreamLease&&) noexcept = default;
    ~UpstreamLease();

    template <typename CompletionToken>
    auto asyncRoundTrip(
        SerializedRequest request,
        CompletionToken&& token
    );

    [[nodiscard]] bool valid() const noexcept;

private:
    friend class UpstreamPool;
    std::shared_ptr<UpstreamConnection> connection_;
    std::weak_ptr<UpstreamPool> owner_;
    bool explicitly_returned_{false};
};
```

Lease 保证同一上游连接同时只属于一个事务。它不暴露 Socket 或可任意调用的裸 Connection 引用；`asyncRoundTrip()` 内部把工作投递到 UpstreamConnection strand，并将完成结果送回调用者的 associated executor。由于析构函数不能 `co_await`：

- 正常成功路径显式调用 `releaseReusable()`；
- 协议错误和取消路径显式 `discard()`；
- 如果调用者忘记，Lease 析构只向 Pool strand 投递 discard，绝不默认复用状态未知的连接。

### 13.7 连接回池条件

只有全部满足才允许复用：

- 响应消息边界完整解析；
- 没有协议错误；
- 双方没有 `Connection: close`；
- Parser/Buffer 没有污染下一请求的未知字节；
- Socket 仍打开；
- 未超最大复用次数；
- 未超最大连接寿命；
- 当前 Endpoint 仍健康；
- 没有超时或取消；
- 当前事务没有未完成异步操作。

默认策略应偏向丢弃可疑连接，而不是冒险污染下一个请求。

### 13.8 池配置

- 每个 Endpoint 最大连接数；
- 最大空闲连接数；
- 空闲超时；
- 最大连接寿命；
- 最大复用次数；
- 等待队列长度；
- acquire timeout；
- connect timeout；
- 每轮最多创建的新连接数；
- draining 时是否允许归还。

空闲连接由 `steady_timer` 清理。Timer 回调只向 Pool strand 投递清理，不与借还操作并发修改容器。

### 13.9 测试

- 第二次请求复用相同连接 ID；
- 同一连接不会同时借给两个事务；
- `Connection: close` 不回池；
- 半响应、超时和取消连接不回池；
- 空闲和最大寿命到期；
- 最大连接数生效；
- 等待队列满时快速失败；
- acquire timeout 会移除 waiter；
- waiter 取消与连接归还竞态只完成一次；
- Lease 遗漏显式归还时安全 discard；
- 上游恢复后重新加入健康集合；
- 服务停机时 idle、busy 和 waiter 正确清理；
- 多线程 TSan 通过。

建议 Commit：

```text
feat(gateway): run threshold health checks with Asio
feat(gateway): publish immutable health snapshots
feat(gateway): add strand-owned upstream connection pool
feat(gateway): add exclusive upstream lease
test(gateway): cover acquire cancel release races
```

---

## 14. 阶段 8A：限流

### 14.1 Token Bucket

每个 Bucket 保存：

```cpp
class TokenBucket {
public:
    TokenBucket(double rate_per_second, double burst);
    bool allow(
        double tokens,
        std::chrono::steady_clock::time_point now
    );

private:
    double rate_;
    double capacity_;
    double tokens_;
    std::chrono::steady_clock::time_point last_refill_;
    std::mutex mutex_;
};
```

补充令牌：

```text
new_tokens = elapsed_seconds * rate
tokens = min(capacity, tokens + new_tokens)
```

分两层实现：

1. 全局限流；
2. 按路由或客户端 IP 限流。

客户端维度会产生大量 Bucket，需要：

- 分片 HashMap；
- TTL 清理；
- 最大 key 数；
- 明确可信代理链，否则不能盲信 `X-Forwarded-For`；
- 429 响应和可选 `Retry-After`；
- Metrics 记录拒绝原因，但 IP 不能直接成为 Prometheus label。

### 14.2 测试

- 初始 burst；
- 持续速率；
- 时间推进后恢复；
- 并发调用不超发；
- FakeClock 保证测试确定性；
- 客户端 Bucket 过期；
- key 数到上限时策略明确；
- 限流响应不会建立上游连接。

---

## 15. 阶段 8B：分片 LRU + TTL 缓存

### 15.1 第一版缓存范围

只缓存满足全部条件的响应：

- 方法为 GET/HEAD；
- 状态码为 200；
- Body 小于配置上限；
- 路由明确允许缓存；
- 不包含项目决定禁止缓存的敏感 Header；
- TTL 大于 0。

Cache key 至少包含：

```text
scheme + host + normalized_path + query + configured_vary_headers
```

不要默认缓存带 `Authorization` 或 `Cookie` 的请求。

### 15.2 数据结构

```cpp
struct CacheEntry {
    int status_code;
    http::Headers headers;
    std::string body;
    std::chrono::steady_clock::time_point expires_at;
    std::size_t charge;
};

class CacheShard {
public:
    std::optional<CacheEntry> get(
        std::string_view key,
        std::chrono::steady_clock::time_point now
    );
    void put(std::string key, CacheEntry entry);
    void erase(std::string_view key);

private:
    using List = std::list<std::pair<std::string, CacheEntry>>;
    std::mutex mutex_;
    List lru_;
    std::unordered_map<std::string, List::iterator> index_;
    std::size_t current_bytes_{0};
    std::size_t max_bytes_;
};

class ShardedCache {
public:
    explicit ShardedCache(std::size_t shard_count);
private:
    std::vector<std::unique_ptr<CacheShard>> shards_;
};
```

限制以总字节数为主，不只按对象数量。否则少量大响应就能耗尽内存。

### 15.3 演进优化

按独立 Commit 实验：

1. 单锁 LRU；
2. 分片 LRU；
3. 过期惰性删除；
4. 后台分批清理；
5. 可选 SingleFlight，避免热点 miss 同时打爆上游；
6. 对比 hit/miss 下的吞吐和锁竞争。

### 15.4 测试

- 命中更新 LRU 顺序；
- 容量淘汰；
- TTL 过期；
- 相同 key 更新；
- 按字节容量淘汰；
- 多线程 get/put；
- 敏感请求不缓存；
- 过大响应不缓存；
- Cache hit 不访问上游；
- HEAD 与 GET 的 Body 行为正确。

---

## 16. 阶段 8C：熔断、背压与过载保护

### 16.1 熔断器

```text
Closed
  -- failure threshold --> Open

Open
  -- cooldown elapsed --> HalfOpen

HalfOpen
  -- probe success --> Closed
  -- probe failure --> Open
```

实现：

```cpp
enum class CircuitState {
    Closed,
    Open,
    HalfOpen
};

class CircuitBreaker {
public:
    bool allowRequest(std::chrono::steady_clock::time_point now);
    void recordSuccess();
    void recordFailure(std::chrono::steady_clock::time_point now);
    [[nodiscard]] CircuitState state() const;
};
```

需要定义哪些失败计入熔断；客户端取消和本地限流通常不应计入上游故障。

### 16.2 背压

至少配置：

- 全局最大连接数；
- 单连接最大输入 Buffer；
- 单连接最大输出 Buffer；
- 上游等待队列上限；
- 最大 Header/Body；
- 每轮单连接最多处理字节或请求数；
- 服务总 in-flight 请求数；
- 高水位和低水位。

当下游读取过慢、输出超过高水位时：

1. 暂停对应上游连接的读取；
2. 下游输出降到低水位后恢复；
3. 超过绝对上限则中止事务；
4. 记录 `backpressure_pauses_total` 和拒绝原因。

这比无限增长 Buffer 更可靠，也比立即关闭所有慢客户端更温和。

### 16.3 公平性

即使使用协程，也不能让单个活跃连接在不挂起的循环里无限处理数据。可以设置每轮预算：

```text
max_read_bytes_per_tick
max_write_bytes_per_tick
max_requests_per_tick
```

超出预算后通过 `post()` 主动让出执行机会，再继续处理，避免其他连接饥饿。不要在每个小分片后无条件 `post()`，否则会制造大量调度开销；用基准确定合理预算。

### 16.4 故障测试

- 上游持续 500 触发熔断；
- Open 期间请求快速失败且不访问上游；
- HalfOpen 只允许少量探测；
- 慢客户端不会造成进程内存无界增长；
- 上游快速、下游慢时读暂停生效；
- 输出恢复后能继续传输；
- 达到全局连接上限时行为明确；
- 高负载下 `io_context` 仍能及时恢复 Timer 协程。

建议 Commit：

```text
feat(gateway): add token bucket rate limiting
feat(cache): add byte-bounded sharded LRU cache
feat(gateway): add upstream circuit breaker
feat(net): apply output high-water backpressure
test(resilience): cover overload and slow downstream
```

---

## 17. 阶段 9A：配置管理

### 17.1 配置内容

最终配置示例：

```yaml
server:
  listen_host: 0.0.0.0
  listen_port: 8080
  io_threads: 4
  idle_timeout_ms: 30000
  header_timeout_ms: 5000
  graceful_shutdown_ms: 15000
  max_connections: 20000
  max_header_bytes: 16384
  max_body_bytes: 1048576
  output_high_water_bytes: 4194304

routes:
  - path_prefix: /api/
    upstream: demo
    rate_limit:
      requests_per_second: 1000
      burst: 200
    cache:
      enabled: true
      ttl_ms: 1000
      max_object_bytes: 262144

upstreams:
  - name: demo
    endpoints:
      - 127.0.0.1:9001
      - 127.0.0.1:9002
    connect_timeout_ms: 300
    response_timeout_ms: 2000
    max_connections_per_endpoint: 64
    pool_shards: 4

logging:
  level: info
  format: json
```

可以在这一阶段引入 `yaml-cpp`。依赖必须在 `Dependencies.cmake` 中集中管理并固定版本。

### 17.2 校验与默认值

配置解析和配置校验是两个步骤：

```cpp
class ConfigLoader {
public:
    Config loadFromFile(const std::filesystem::path& path) const;
    std::vector<ConfigError> validate(const Config& config) const;
};
```

校验：

- 端口范围；
- 线程数范围；
- 超时非负且合理；
- route 引用的 upstream 存在；
- Endpoint 不重复；
- cache shard 数为正；
- 低水位小于高水位；
- 最大 Body 不超过绝对安全上限；
- 同一字段的单位明确写在名称中。

错误应一次报告尽可能多的问题，并包含字段路径：

```text
routes[1].upstream: unknown upstream "orders"
server.output_low_water_bytes: must be lower than high water
```

### 17.3 热更新

安全流程：

1. `asio::signal_set` 收到 SIGHUP；
2. 向配置管理 strand 投递 reload 请求并做去重；
3. 在专用工作线程读取文件、解析为候选对象；
4. 完整校验；
5. 把结果投递回配置 strand；
6. 构造不可变 `shared_ptr<const ConfigSnapshot>`；
7. 原子替换快照；
8. 新请求使用新快照；
9. 已建立连接按明确策略继续或逐步迁移；
10. 失败时保留旧配置并记录错误。

`signal_set.async_wait()` 的完成 Handler 是普通 Asio Handler，不是传统 POSIX signal handler，但仍只负责调度 reload；解析和校验在受控路径完成。

不可热更新的项目，例如监听地址或 I/O 线程数，应提示需要重启。

---

## 18. 阶段 9B：日志与可观测性

### 18.1 日志

可以引入 `spdlog`，但要包装成项目接口，避免每个模块直接依赖第三方 API：

```cpp
enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Critical
};

struct RequestLog {
    std::string request_id;
    std::string method;
    std::string target;
    int status;
    std::chrono::microseconds duration;
    std::size_t bytes_in;
    std::size_t bytes_out;
    std::string upstream;
    std::string cache_status;
};
```

每个请求结束写一条 access log：

```json
{
  "timestamp": "2026-01-01T00:00:00.000Z",
  "level": "info",
  "request_id": "01...",
  "method": "GET",
  "target": "/api/users",
  "status": 200,
  "duration_us": 812,
  "bytes_in": 128,
  "bytes_out": 2048,
  "upstream": "demo/127.0.0.1:9001",
  "cache": "miss"
}
```

要求：

- 默认不记录 Authorization、Cookie 和完整敏感 Body；
- 日志时间使用 wall clock，超时和耗时使用 steady clock；
- 支持日志级别；
- 高负载时不能让同步磁盘日志阻塞 `io_context` 工作线程；
- 异步日志队列必须有容量上限和丢弃策略；
- 被丢弃日志数量也要成为 Metric。

### 18.2 Metrics

提供 `/metrics`，先实现 Prometheus text exposition：

```text
# HELP pulsegate_http_requests_total Total HTTP requests.
# TYPE pulsegate_http_requests_total counter
pulsegate_http_requests_total{method="GET",status="200"} 1024

# HELP pulsegate_active_connections Current downstream connections.
# TYPE pulsegate_active_connections gauge
pulsegate_active_connections 37
```

至少包含：

- `http_requests_total{method,status_class,route}`；
- `http_request_duration_seconds` histogram；
- `active_connections`；
- `accepted_connections_total`；
- `rejected_connections_total{reason}`；
- `upstream_requests_total{upstream,result}`；
- `upstream_connect_duration_seconds`；
- `cache_requests_total{result}`；
- `rate_limit_rejections_total{route}`；
- `circuit_state{upstream}`；
- `runtime_active_coroutines`；
- `upstream_pool_waiters`；
- `output_buffer_bytes`；
- `logs_dropped_total`。

避免高基数 Label：

- 不用 request ID；
- 不用用户 ID；
- 不用完整 URL query；
- 不用客户端 IP；
- 路由使用模板或配置名称，不使用原始路径。

计数器建议每线程分片，抓取时聚合，减少所有请求争用一个原子变量。先实现正确版本，再根据 profile 优化。

### 18.3 健康接口

区分：

- `/livez`：进程是否活着；
- `/readyz`：是否已加载配置且能接收流量；
- `/metrics`：指标；
- 可选 `/debug/config`：仅绑定管理地址，输出脱敏配置摘要。

---

## 19. 阶段 9C：优雅停机

### 19.1 状态机

```text
Running
  -- SIGTERM -->
Draining
  - stop accept
  - readyz returns 503
  - reject new requests on existing keep-alive connections
  - wait in-flight transactions
  -- all complete --> Stopped
  -- deadline --> force close --> Stopped
```

### 19.2 Signal 处理

使用 Boost.Asio `signal_set`：

```cpp
void waitForSignal(
    const std::shared_ptr<boost::asio::signal_set>& signals,
    const std::shared_ptr<HttpServer>& server
) {
    signals->async_wait(
        [signals, server](
            const boost::system::error_code& error,
            int signal
        ) {
            if (error) {
                return;
            }

            if (signal == SIGHUP) {
                server->requestConfigReload();
                waitForSignal(signals, server);
                return;
            }

            server->beginDrain();
        }
    );
}

auto signals = std::make_shared<boost::asio::signal_set>(
    runtime.context(),
    SIGINT,
    SIGTERM,
    SIGHUP
);
waitForSignal(signals, server);
```

注意：

- 保持 `signal_set` 活到服务停止；
- SIGHUP 完成后重新发起 `async_wait()`；
- SIGTERM 只触发一次 `beginDrain()`，它必须幂等；
- Draining 先关闭 Listener，再等待 Registry 和 ProxySession；
- grace deadline 使用 `steady_timer`；
- deadline 到期后取消 Session、Resolver、Pool waiter 和上游 Socket；
- 业务清理完成后 reset work guard，让 `io_context.run()` 自然返回；
- `io_context.stop()` 只作为强制退出的最后手段。

### 19.3 测试

集成测试：

1. 启动慢上游；
2. 发起代理请求；
3. 向 PulseGate 发送 SIGTERM；
4. 验证监听端口停止接受新连接；
5. 已在处理的请求在 deadline 内完成；
6. 进程退出码为 0；
7. 将上游延迟设为大于 deadline；
8. 验证事务被强制取消且进程按时退出。

建议 Commit：

```text
feat(config): load and validate YAML configuration
feat(config): atomically reload immutable snapshots
feat(observability): add structured access logs
feat(observability): expose Prometheus metrics
feat(server): drain in-flight requests on SIGTERM
```

---

## 20. 测试体系

### 20.1 测试金字塔

```text
           少量端到端测试
        集成测试与故障注入
     大量快速、确定的单元测试
```

### 20.2 单元测试

重点覆盖纯逻辑：

- Buffer；
- HTTP Parser；
- Header；
- Router；
- Token Bucket；
- LRU；
- Cache key；
- Load Balancer；
- Circuit Breaker；
- Config Validation；
- Deadline policy（FakeClock）；
- Health threshold state machine；
- Pool capacity and waiter policy；
- Header 重写。

每个 bug 修复先增加能复现问题的测试，再修复代码。

### 20.3 集成测试

使用 loopback 和临时端口，不固定占用 8080/9001：

- Server 启停；
- Keep-Alive；
- 慢发送；
- 慢读取；
- 连接重置；
- 代理；
- 上游池；
- Timer；
- 配置热更新；
- 优雅停机。

测试夹具负责：

- 创建独立 `io_context` 和 work guard；
- 在后台线程执行 `io_context.run()`；
- 等待 Server ready；
- 获取实际监听端口；
- 测试结束依次 drain、reset work guard、join；
- 即使断言失败也通过 RAII 清理。

每个测试使用独立 Runtime，不能依赖测试执行顺序。需要控制异步步骤时，优先注入 FakeClock、FakeResolver 或测试 Executor；不要用大段 `sleep_for()` 猜测操作已经完成。

### 20.4 端到端测试

`docker compose up --build` 后运行：

```bash
curl --fail http://127.0.0.1:8080/livez
curl --fail http://127.0.0.1:8080/api/whoami
curl --fail http://127.0.0.1:8080/metrics
```

然后检查 A/B 上游轮询、缓存命中和限流。

### 20.5 模糊测试

HTTP Parser 非常适合 libFuzzer：

```cpp
extern "C" int LLVMFuzzerTestOneInput(
    const std::uint8_t* data,
    std::size_t size
) {
    pulsegate::net::Buffer buffer;
    buffer.append(std::string_view(
        reinterpret_cast<const char*>(data),
        size
    ));

    pulsegate::http::HttpParser parser;
    static_cast<void>(parser.parse(buffer));
    return 0;
}
```

不要求解析任意字节都成功，只要求：

- 不崩溃；
- 不越界；
- 不无限循环；
- 不申请不受限制的内存；
- Complete 时对象满足不变量。

把发现崩溃的输入最小化并加入 `tests/testdata/http/` 回归测试。

### 20.6 测试命名

推荐：

```cpp
TEST(HttpParserTest, ReturnsNeedMoreForFragmentedHeader)
TEST(HttpParserTest, RejectsConflictingContentLength)
TEST(CacheTest, EvictsLeastRecentlyUsedEntryByBytes)
TEST(ProxyTest, DoesNotRetryPostAfterBytesWereForwarded)
```

名称描述行为，不描述实现细节。

---

## 21. Sanitizer、静态分析与调试

### 21.1 建议顺序

每个 PR 本地至少执行：

```bash
cmake --build --preset debug
ctest --preset debug

cmake --build --preset asan
ctest --preset asan
```

涉及线程时额外执行：

```bash
cmake --build --preset tsan
ctest --preset tsan
```

Sanitizer 构建不是性能结果来源，也不能直接用于生产镜像。

### 21.2 ASan/UBSan

典型配置：

```cmake
target_compile_options(
  pulsegate_sanitizers
  INTERFACE
    -fsanitize=address,undefined
    -fno-omit-frame-pointer
)
target_link_options(
  pulsegate_sanitizers
  INTERFACE
    -fsanitize=address,undefined
)
```

可发现：

- use-after-free；
- 越界；
- double free；
- 部分泄漏；
- 部分未定义行为。

### 21.3 TSan

```cmake
-fsanitize=thread -g -O1
```

用来发现数据竞争。TSan 开销很高，测试规模可以缩小，但不能因为“运行慢”而跳过并发核心模块。不要同时启用 ASan 和 TSan。

### 21.4 clang-tidy

`.clang-tidy` 初始建议：

```yaml
Checks: >
  -*,
  bugprone-*,
  clang-analyzer-*,
  concurrency-*,
  performance-*,
  portability-*,
  modernize-*,
  readability-*
WarningsAsErrors: ''
HeaderFilterRegex: 'include/pulsegate/|src/'
```

不要第一天把所有检查设为 error。先记录基线，逐步修复，再对稳定检查设门禁。

生成 compilation database 后：

```bash
run-clang-tidy -p build/debug
```

### 21.5 调试工具

```bash
gdb --args ./build/debug/app/pulsegate --config config/pulsegate.example.yaml
strace -f -e trace=network,epoll_wait,eventfd2,read,write \
  ./build/debug/app/pulsegate
lsof -p <pid>
```

排查异步执行顺序时，可以只在专用 Debug 构建中定义：

```cmake
BOOST_ASIO_ENABLE_HANDLER_TRACKING
```

它会输出 Handler 创建、完成与取消轨迹，日志量很大，禁止进入 Release 性能测试。

常见定位方向：

| 现象 | 优先检查 |
|---|---|
| CPU 100% 且无流量 | 协程循环没有 `co_await`、accept 错误无退避、零期限 Timer |
| 请求偶尔卡住 | `io_context` 无运行线程、取消未处理、Pool waiter 未完成 |
| 内存上涨 | 输出无背压、`shared_ptr` 环、遗失协程、缓存/队列无上限 |
| 同一 Session 数据竞争 | Socket 未绑定 strand、外部线程直接改状态 |
| Body 错位 | 解析完成后错误丢弃 Buffer 剩余字节 |
| 压测出现 reset | backlog、fd 限制、客户端端口、过载策略 |
| 停机无法退出 | work guard 未 reset、Timer/accept 未取消、Lease 未归还 |

---

## 22. 性能测试与逐步优化

### 22.1 性能测试纪律

一次实验只改变一个主要变量。每份结果必须记录：

```text
date:
git_commit:
build_type:
compiler:
compiler_flags:
os_kernel:
cpu:
logical_cores:
memory:
server_command:
server_config:
load_generator_machine:
wrk_command:
warmup:
duration:
connections:
threads:
response_size:
rps:
p50:
p90:
p99:
max_latency:
socket_errors:
server_cpu:
server_rss:
notes:
```

压测机和服务机最好分开；如果在同一台机器，必须明确说明，因为它们会争用 CPU 和网络栈。

### 22.2 基础命令

```bash
wrk \
  -t4 \
  -c400 \
  -d60s \
  --latency \
  http://127.0.0.1:8080/healthz
```

先预热，再正式运行。每组至少重复 3 次，报告中位数和波动，不只挑最好的一次。

连接矩阵：

```text
connections = 1, 10, 100, 500, 1000, 5000
payload     = 0 B, 1 KiB, 64 KiB, 1 MiB
scenario    = local response, proxy, cache hit, cache miss, slow upstream
```

### 22.3 先确认负载生成器不是瓶颈

检查：

- wrk CPU 是否已打满；
- 临时端口是否耗尽；
- `ulimit -n`；
- listen backlog；
- 是否出现 connect/read/write/timeout 错误；
- loopback 是否隐藏真实网络成本；
- 是否意外启用 Debug 或 Sanitizer；
- 日志量是否主导性能。

不要为了漂亮数字直接修改系统参数后不记录。

### 22.4 Profile

Release 构建：

```bash
perf record -F 99 -g -- \
  ./build/release/app/pulsegate \
  --config config/pulsegate.example.yaml

perf report
```

观察：

- `io_context::run()` 与底层 `epoll_wait`；
- Asio Handler/Coroutine frame 调度；
- 系统调用；
- Buffer 移动和分配；
- HTTP 解析；
- Header lowercase/hash；
- 日志格式化；
- Metric 原子争用；
- Cache Mutex；
- 上游选择；
- `shared_ptr` 引用计数。

### 22.5 优化顺序

严格按证据推进：

1. **Release 基线**：确认 `-O2/-O3` 和 `NDEBUG`；
2. **算法问题**：删除 O(n²) Buffer erase、重复扫描；
3. **不必要复制**：合理使用 `string_view`、move、reserve；
4. **Asio Buffer sequence**：减少小块写和重复序列化；
5. **静态文件路径**：比较有界工作池、Beast `file_body` 或平台 adapter；
6. **日志**：异步、有界队列、采样；
7. **Metric**：每线程分片，抓取时聚合；
8. **锁竞争**：Cache/Pool 分片，缩小 strand 串行范围；
9. **连接复用**：测量 connect、DNS 和 Pool 命中；
10. **协程/Handler 分配**：确认热点后才尝试 allocator、对象池或 PMR；
11. **线程与内核参数**：工作线程数、CPU affinity、backlog 作为独立实验。

每个优化 PR 都回答：

- Profile 证据是什么？
- 预期改善哪个指标？
- 正确性风险是什么？
- 基准是否可复现？
- 改善是否超过噪声？
- 如果无改善，是否回退？

### 22.6 Boost.Asio 专项对照实验

主项目不直接控制 epoll LT/ET；Boost.Asio 负责平台分发机制。按以下顺序实验：

1. `io_context` 工作线程数 1/2/4/8；
2. 所有 Session 共用 strand（错误基线）与每 Session strand；
3. 读取 Buffer 4/16/64 KiB；
4. 完整缓冲响应与分块流式响应；
5. 无上游池与不同 Pool 容量；
6. 同步日志、异步日志和关闭 access log 的差异；
7. Cache 单锁与分片；
8. 默认分配与有证据的 Handler allocator；
9. Debug handler tracking 开/关，证明它不能用于性能构建；
10. 可选比较 Boost.Asio 默认 Linux backend 与经过验证的 `io_uring` 配置。

最后一项依赖 Boost 构建特性、内核和 `liburing`，只能作为独立分支；不能为了“用了 io_uring”强行进入主线。LT/ET、`EAGAIN` 和 `EPOLLONESHOT` 放在 `labs/raw_epoll_server` 中学习。

### 22.7 Benchmark 报告结构

`docs/benchmark-report.md`：

```markdown
# Benchmark Report

## Goal
## Hardware and OS
## Commit and build flags
## Topology
## Server configuration
## Workload
## Raw commands
## Results
## Flame graph / perf findings
## Optimization performed
## Before-and-after comparison
## Limitations
## How to reproduce
```

README 只放摘要，原始结果和完整方法放报告。不要只写最高 RPS，要同时报告尾延迟和错误。

建议 Commit：

```text
perf(buffer): reduce copies with Asio buffer sequences
perf(metrics): aggregate per-shard counters on scrape
perf(runtime): evaluate io_context worker counts
docs(benchmark): publish reproducible v1 benchmark
```

---

## 23. Docker 容器化

### 23.1 目标

Docker 不是“能打包就结束”，而是实现：

- 编译环境可复现；
- 运行镜像不包含编译器和源码；
- 非 root 用户运行；
- 配置通过只读挂载；
- 日志输出 stdout/stderr；
- SIGTERM 能到达主进程并触发优雅停机；
- Compose 一条命令启动网关和多个上游。

Docker 官方推荐多阶段构建，把编译环境与运行环境分开，最终镜像只复制运行所需产物。

### 23.2 多阶段 Dockerfile

下面是目标结构，实际包名按最终依赖调整：

```dockerfile
# syntax=docker/dockerfile:1

FROM ubuntu:24.04 AS build

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       build-essential \
       cmake \
       ninja-build \
       ca-certificates \
       git \
       libboost-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

COPY CMakeLists.txt CMakePresets.json ./
COPY cmake ./cmake
COPY include ./include
COPY src ./src
COPY app ./app

RUN cmake --preset release \
    && cmake --build --preset release

FROM ubuntu:24.04 AS runtime

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --system --gid 10001 pulsegate \
    && useradd --system --uid 10001 --gid pulsegate \
       --home-dir /nonexistent --shell /usr/sbin/nologin pulsegate

COPY --from=build \
  /src/build/release/app/pulsegate \
  /usr/local/bin/pulsegate
COPY config/pulsegate.example.yaml /etc/pulsegate/config.yaml

USER 10001:10001
EXPOSE 8080
STOPSIGNAL SIGTERM

ENTRYPOINT ["/usr/local/bin/pulsegate"]
CMD ["--config", "/etc/pulsegate/config.yaml"]
```

注意：

- Boost.Asio 与 Boost.System 在这里都是 header-only，不需要向运行镜像复制 Boost
  动态库；切换或增加依赖后仍要用 `ldd` 检查实际运行时依赖；
- 示例未包含 `yaml-cpp`/`spdlog` 的安装方式，最终应根据静态/动态链接策略补齐；
- 构建镜像与本地开发必须满足项目声明的 Boost 最低版本；
- 不使用 `latest` 作为你自己发布镜像的唯一 Tag；
- 基础镜像最好进一步固定 digest，并由自动化工具定期更新；
- `ENTRYPOINT` 使用 exec form，保证信号直接到应用；
- 运行用户不需要 shell 和 home；
- 不把 `.git`、构建目录和 benchmark 结果复制进 build context。

### 23.3 `.dockerignore`

```dockerignore
.git
.github
build
cmake-build-*
.cache
benchmarks/results
docs
tests
*.log
.env
config/pulsegate.local.yaml
```

如果 Docker build 需要测试源码，则在 test stage 中调整，而不是永久忽略。

### 23.4 Compose

最终 `compose.yaml`：

```yaml
name: pulsegate-demo

services:
  gateway:
    build:
      context: .
      target: runtime
    ports:
      - "8080:8080"
    volumes:
      - ./config/pulsegate.docker.yaml:/etc/pulsegate/config.yaml:ro
    depends_on:
      upstream-a:
        condition: service_healthy
      upstream-b:
        condition: service_healthy
    init: true
    stop_grace_period: 20s
    read_only: true
    tmpfs:
      - /tmp
    security_opt:
      - no-new-privileges:true

  upstream-a:
    image: <固定版本的测试上游镜像>
    command: ["--port", "9000", "--name", "upstream-a"]
    healthcheck:
      test: ["CMD", "<health-check-command>"]
      interval: 2s
      timeout: 1s
      retries: 10

  upstream-b:
    image: <固定版本的测试上游镜像>
    command: ["--port", "9000", "--name", "upstream-b"]
    healthcheck:
      test: ["CMD", "<health-check-command>"]
      interval: 2s
      timeout: 1s
      retries: 10
```

尖括号内容必须替换为你实际提供的 mock upstream 镜像/命令，不能把占位文件当作完成版本提交。

常用命令：

```bash
docker compose config
docker compose build
docker compose up -d
docker compose ps
docker compose logs -f gateway
curl http://127.0.0.1:8080/livez
docker compose down
```

### 23.5 容器验收

- 镜像内进程 UID 非 0；
- `docker stop` 能优雅退出；
- 根文件系统只读时仍可运行；
- 配置文件只读挂载；
- 两个上游都能被轮询；
- 停掉一个上游后网关继续工作；
- 恢复上游后健康检查能重新加入；
- 最终镜像不包含编译器；
- 记录镜像大小和 SBOM/扫描结果作为加分项。

---

## 24. GitHub Actions CI

### 24.1 CI 最小门禁

每次 Push 和 Pull Request：

- GCC Debug 构建；
- Clang Debug 构建；
- 单元和集成测试；
- ASan/UBSan；
- TSan（可以拆分并降低频率）；
- clang-format 检查；
- clang-tidy；
- Docker build；
- 可选端到端 Compose 测试。

### 24.2 示例 `ci.yml`

下面是教学模板，应在实际仓库中固定依赖版本并根据测试命令调整：

```yaml
name: ci

on:
  push:
    branches: [main]
  pull_request:
    branches: [main]

permissions:
  contents: read

concurrency:
  group: ci-${{ github.workflow }}-${{ github.ref }}
  cancel-in-progress: true

jobs:
  build-and-test:
    strategy:
      fail-fast: false
      matrix:
        include:
          - compiler: gcc
            cc: gcc
            cxx: g++
          - compiler: clang
            cc: clang
            cxx: clang++

    runs-on: ubuntu-latest

    steps:
      - name: Checkout
        uses: actions/checkout@v6

      - name: Install build tools
        run: |
          sudo apt-get update
          sudo apt-get install -y \
            cmake ninja-build clang libboost-dev

      - name: Configure
        env:
          CC: ${{ matrix.cc }}
          CXX: ${{ matrix.cxx }}
        run: >
          cmake -S . -B build/ci -G Ninja
          -DCMAKE_BUILD_TYPE=Debug
          -DPULSEGATE_BUILD_TESTS=ON

      - name: Build
        run: cmake --build build/ci --parallel 2

      - name: Test
        run: ctest --test-dir build/ci --output-on-failure

  sanitizer:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v6

      - name: Install build tools
        run: |
          sudo apt-get update
          sudo apt-get install -y \
            cmake ninja-build clang libboost-dev

      - name: Configure ASan and UBSan
        env:
          CC: clang
          CXX: clang++
        run: >
          cmake -S . -B build/asan -G Ninja
          -DCMAKE_BUILD_TYPE=Debug
          -DPULSEGATE_BUILD_TESTS=ON
          -DPULSEGATE_ENABLE_ASAN=ON
          -DPULSEGATE_ENABLE_UBSAN=ON

      - name: Build
        run: cmake --build build/asan --parallel 2

      - name: Test
        env:
          ASAN_OPTIONS: detect_leaks=1:halt_on_error=1
          UBSAN_OPTIONS: print_stacktrace=1:halt_on_error=1
        run: ctest --test-dir build/asan --output-on-failure
```

GitHub 官方 Actions Quickstart 当前示例使用 `actions/checkout@v6`。未来大版本变化时，以官方文档为准，并通过 Dependabot 或定期维护 PR 更新。

### 24.3 CI 不做什么

- 普通 PR 不跑长达数分钟的完整性能基准；
- 不把不稳定的 RPS 阈值作为单元测试；
- 不在日志打印 Secret；
- 不给默认 workflow 不必要的写权限；
- 不直接在 fork PR 中运行不可信代码并暴露 Secret；
- 不用 CI 掩盖本地不可复现的构建。

### 24.4 性能回归

后期可以增加手动触发：

```yaml
on:
  workflow_dispatch:
```

它运行固定硬件自托管 Runner 上的短基准，输出 artifact。GitHub 托管 Runner 的性能会波动，不适合用非常窄的 RPS 阈值作判断。

---

## 25. 依赖管理

### 25.1 允许的依赖

建议：

| 功能 | 选择 |
|---|---|
| 异步网络与 Timer | Boost.Asio |
| 错误码 | Boost.System |
| 测试 | GoogleTest |
| 日志 | spdlog |
| YAML | yaml-cpp |
| TLS（v1.0 后） | OpenSSL |
| Runtime/Session 封装 | 基于 Boost.Asio 自己设计 |
| HTTP Parser | 自己实现学习版 |
| 指标格式 | 初版自己实现最小 Prometheus 文本格式 |

### 25.2 原则

- 所有依赖集中在 `cmake/Dependencies.cmake`；
- 固定 tag 或最好固定 commit SHA；
- 记录许可证；
- 不追踪第三方生成文件；
- 依赖升级单独 PR；
- GitHub CI 与 Docker 使用相同版本；
- README 写明网络构建需求；
- 如果改用 Conan/vcpkg，应写 ADR 说明原因，不要两套方案混用。

### 25.3 为什么不全部自己实现

异步组合、Session 生命周期和 HTTP Parser 是学习重点；不重复实现 `epoll` 分发器、跨平台 Socket、日志格式库、YAML 解析和 TLS。工程能力也包括知道哪些组件应该复用成熟方案，以及如何在库的抽象之上设计清晰边界。

Boost.Beast 不进入主线，但可以作为两个用途：

- 在测试中与自研 Parser 做差分验证；
- v1.0 后实验 `file_body`、TLS stream 或 WebSocket。

如果直接将 HTTP 全部替换为 Beast，需要在 ADR 中说明学习目标变化，并把精力转移到异步组合、代理流式背压和连接池，而不是声称 HTTP Parser 是自研模块。

---

## 26. 每周执行计划

### 第 1 周：工程骨架与 Asio 同步基线

- 完成阶段 0；
- 配置 Boost.Asio、Boost.System、Threads 和 GoogleTest；
- 完成 `io_context` smoke test；
- 使用 `tcp::acceptor/socket` 返回最小 HTTP 响应；
- 建立 GitHub 仓库、Issue、PR、Milestone；
- 写明同步基线的限制。

交付：`v0.1.0`。

### 第 2 周：Buffer 与 HTTP Parser

- 实现带读写下标的 Buffer；
- 提供 `prepare/commit/consume` 接口；
- Request/Response/Header；
- Parser 状态机；
- 分片、冲突长度和超限测试；
- Keep-Alive 规则。

交付：Parser ADR 和完整测试矩阵。

### 第 3 周：单线程 Asio Coroutine Server

- `CoroutineGuard`；
- `Listener::acceptLoop()`；
- `HttpSession::run()`；
- `async_read_some/async_write`；
- Session 生命周期；
- Keep-Alive；
- 初次 wrk 基线。

交付：`v0.2.0` 和单线程 baseline。

### 第 4 周：Deadline、取消与多线程

- `steady_timer` Deadline；
- Header/Body/Idle timeout；
- StopReason；
- Session Registry；
- `AsioRuntime`；
- 多线程 `io_context.run()`；
- 每 Session strand；
- TSan 和线程数对照。

交付：`v0.4.0`。如希望版本历史更细，可在超时完成后先打 `v0.3.0`。

### 第 5 周：异步 HTTP 应用

- Coroutine Router；
- `/livez`、`/readyz`、`/echo`；
- 有界文件工作池；
- 静态文件路径安全；
- HTTP 错误映射；
- Handler 异常边界。

交付：`v0.5.0`，此时是可展示的 Asio HTTP Server。

### 第 6 周：协程式反向代理

- 上游 Response Parser；
- `async_resolve`；
- `async_connect`；
- `ProxySession`；
- Round Robin；
- 超时与取消传播；
- hop-by-hop Header；
- 故障注入测试。

交付：`v0.6.0`，此时升级为网关。

### 第 7 周：健康检查与连接池

- HealthChecker 协程；
- 不可变健康快照；
- Pool strand；
- composed `asyncAcquire()`；
- UpstreamLease；
- 空闲超时和复用规则；
- acquire/release/cancel 竞态测试。

交付：`v0.7.0`。

### 第 8 周：限流与缓存

- Token Bucket；
- 客户端 key 生命周期；
- 分片 LRU + TTL；
- Cache policy；
- hit/miss benchmark；
- 确认缓存和限流不会阻塞 Session strand。

交付：`v0.8.0`。

### 第 9 周：可靠性

- Circuit Breaker；
- 流式代理；
- 高低水位背压；
- 全局资源上限；
- 慢客户端/慢上游；
- 协程公平预算；
- 过载下 Timer 延迟测试。

交付：可靠性报告。

### 第 10 周：配置与可观测性

- YAML；
- SIGHUP + `signal_set`；
- 不可变配置热更新；
- JSON 日志；
- Metrics；
- graceful drain；
- Runtime 自然退出。

交付：`v0.9.0`。

### 第 11 周：Docker 与 CI

- 构建/运行镜像中的 Boost 依赖；
- 多阶段镜像；
- Compose demo；
- GCC/Clang GitHub Actions；
- ASan/UBSan/TSan；
- 分支保护；
- 一键端到端验证。

交付：Release Candidate。

### 第 12 周：Profile、优化和简历

- 完整 Benchmark matrix；
- perf 与 Asio Handler tracking；
- 完成 2～3 个有证据的优化；
- 工作线程、Buffer 和 Pool 对照；
- 可选 raw epoll 实验；
- 发布性能报告；
- README 架构图和 Demo；
- 整理面试问答；
- 打 `v1.0.0`。

---

## 27. 每阶段 PR 模板

```markdown
## Why

解决什么问题，为什么现在需要。

## What

- 新增：
- 修改：
- 删除：

## Non-goals

- 本 PR 明确不处理：

## Design

接口、线程模型、生命周期和错误处理。

## Tests

- [ ] Unit tests
- [ ] Integration tests
- [ ] ASan/UBSan
- [ ] TSan（涉及并发时）
- [ ] Manual curl/nc test

## Performance

是否在热路径；如果是，附基准命令和 before/after。

## Risk

潜在失败模式与回滚方法。

## Documentation

- [ ] README
- [ ] CHANGELOG
- [ ] ADR（重要设计变化时）
```

---

## 28. Architecture Decision Record

`docs/decisions/0001-asio-coroutine-model.md`：

```markdown
# ADR 0001: Use Boost.Asio C++20 coroutines with per-session strands

## Status
Accepted

## Context
需要支持大量连接，避免自研跨平台事件分发器，并保持连接状态可推理。

## Decision
使用一个 io_context，由 N 个工作线程执行 run()。
每条 Session 使用独立 strand；异步流程使用 awaitable/co_spawn。
HTTP Parser 和网关能力保持为项目自研模块。

## Alternatives
- 原生 epoll Reactor
- callback 风格 Boost.Asio
- 每线程一个 io_context
- Boost.Beast 完整 HTTP 栈
- 每连接一个线程
- io_uring

## Consequences
减少底层样板代码并获得跨平台 I/O 抽象；
需要深入理解 executor、strand、协程生命周期、取消和 composed operation；
底层 epoll 学习放入独立 lab。
```

建议至少记录：

- Asio Coroutine 与 Executor 模型；
- strand 粒度；
- error_code 与异常边界；
- timeout/cancellation 语义；
- HTTP Parser 范围；
- 为什么主线不用 Beast Parser；
- 依赖管理；
- 上游池 strand 与 Lease；
- 配置热更新语义；
- Cache policy；
- TLS 延后原因。

---

## 29. Definition of Done

一个 Issue 只有满足以下条件才算完成：

- 接口和实现完成；
- 正常路径、错误路径和边界测试完成；
- 没有新增编译警告；
- Debug/Release 构建成功；
- ASan/UBSan 通过；
- 涉及并发时 TSan 通过；
- 配置、README 和 CHANGELOG 已更新；
- 日志不泄露敏感信息；
- 新资源有明确上限；
- 新回调的生命周期可以解释；
- 性能宣称有可复现数据；
- CI 通过；
- PR 已自审并合并。

---

## 30. 最终 README 应该包含什么

推荐顺序：

1. 一句话介绍；
2. 架构图；
3. 核心特性；
4. 30 秒 Docker Demo；
5. 本地构建；
6. 配置示例；
7. API/路由；
8. 测试方式；
9. Benchmark 摘要与完整报告链接；
10. 设计文档和 ADR；
11. Roadmap；
12. 已知限制；
13. License。

不要把 README 写成堆砌术语的简历。每个特性尽量链接到：

- 代码；
- 测试；
- 设计文档；
- Benchmark；
- Issue/PR。

---

## 31. 面试时必须能解释的问题

### Boost.Asio 与网络基础

- TCP 为什么有半包和粘包？
- 阻塞、非阻塞、同步、异步分别是什么？
- `io_context::run()` 执行什么？
- 发起异步操作和完成 Handler 有什么区别？
- `awaitable` 的协程帧如何保存局部变量？
- `co_spawn` 的完成处理器为什么必须观察异常？
- `strand` 保证什么、不保证什么？
- `post()`、`dispatch()` 和直接调用的区别？
- 为什么一个 `io_context` 可以由多个线程运行？
- `redirect_error(use_awaitable, ec)` 解决什么问题？
- `socket.cancel()` 后协程是否会立刻消失？
- `steady_timer` 与 wall clock Timer 有什么区别？
- Asio 在 Linux 上与 epoll 的关系是什么？
- select/poll/epoll 的差别？
- LT 与 ET 的差别？
- ET 为什么要读写到 `EAGAIN`？
- 为什么 `EPOLLOUT` 不能一直开启？
- 非阻塞 connect 如何判断成功？
- 为什么会出现短写？

### C++ 与生命周期

- Asio Socket 如何通过 RAII 管理资源？
- 协程挂起期间引用和 `string_view` 什么时候会悬空？
- `asio::buffer` 是否拥有底层内存？
- Session 为什么在协程入口捕获 `shared_ptr`？
- Session、Proxy 和 Timer 如何避免 `shared_ptr` 环？
- 为什么 Lease 析构时不能执行需要 `co_await` 的归还？
- 哪些状态只能通过关联 strand 修改？
- Move、RAII、异常安全如何应用？

### HTTP 与代理

- 如何解析分片 Header/Body？
- Keep-Alive 的关闭规则？
- `Content-Length` 冲突为什么危险？
- hop-by-hop Header 是什么？
- 为什么 POST 默认不重试？
- 上游连接什么时候可以回池？
- 客户端断开后如何取消上游请求？

### 并发与可靠性

- 为什么选择一个 `io_context` 多线程和每 Session strand？
- 为什么不能使用一个全局 strand？
- 哪些数据用 strand，哪些用 Mutex/原子快照？
- Pool 的 composed async acquire 如何回到调用者 Executor？
- Cache 为什么分片？
- Token Bucket 如何处理突发？
- 熔断与健康检查的区别？
- 背压如何避免内存无界增长？
- 优雅停机期间新请求如何处理？

### 性能与工程

- Benchmark 是否可复现？
- 负载生成器是否可能成为瓶颈？
- P99 为什么比平均值重要？
- 你的 profile 热点是什么？
- 哪个优化没有效果，为什么？
- Sanitizer 分别发现什么？
- Docker 为什么使用多阶段构建？
- CI 为什么不直接跑严格 RPS 门禁？

如果这些问题回答不出来，就说明项目还停留在“代码存在”，没有变成你的能力。

---

## 32. 简历写法

不要在项目没完成时提前填写数字。最终可使用以下模板：

> **PulseGate — C++20 高性能 HTTP 网关**
>
> - 基于 Boost.Asio、C++20 Coroutine 和多线程 `io_context + strand` 实现异步 HTTP/1.1 服务，完成增量解析、Keep-Alive、超时取消和优雅停机。
> - 使用 `async_resolve/async_connect` 实现协程式反向代理，并设计 strand-owned 上游连接池、独占 Lease、负载均衡及主动/被动健康检查。
> - 设计 Token Bucket 限流、分片 LRU/TTL 缓存、熔断与高低水位背压机制，通过资源上限避免慢连接造成内存无界增长。
> - 使用 GoogleTest、ASan/UBSan、TSan、clang-tidy 和 GitHub Actions 建立测试与质量门禁，并通过 Docker Compose 一键启动网关和多上游演示环境。
> - 在【硬件/内核/编译参数】下使用 wrk 完成可复现压测，达到【RPS】，P99【延迟】，相较【基线版本】将【某指标】优化【比例】。

简历只写你确实完成、测试并能解释的内容。

---

## 33. 最终发布检查表

### 功能

- [ ] HTTP/1.1 请求与响应边界正确；
- [ ] Keep-Alive；
- [ ] 路由和静态文件；
- [ ] 反向代理；
- [ ] 负载均衡；
- [ ] 健康检查；
- [ ] 上游连接池；
- [ ] 限流；
- [ ] 缓存；
- [ ] 熔断和背压；
- [ ] 配置热更新；
- [ ] 日志和 Metrics；
- [ ] 优雅停机。

### 正确性

- [ ] 单元测试；
- [ ] 集成测试；
- [ ] 故障注入；
- [ ] Parser fuzz；
- [ ] ASan/UBSan；
- [ ] TSan；
- [ ] 资源泄漏检查；
- [ ] 慢连接测试。

### 工程

- [ ] 固定 Boost 最低版本；
- [ ] Asio Coroutine 异常边界；
- [ ] 多线程 strand/TSan 验证；
- [ ] CMake Presets；
- [ ] GCC/Clang CI；
- [ ] clang-format；
- [ ] clang-tidy；
- [ ] Docker 多阶段构建；
- [ ] Compose demo；
- [ ] 非 root 容器；
- [ ] GitHub Issues/PR/Milestones；
- [ ] CHANGELOG；
- [ ] `v1.0.0` Release。

### 展示

- [ ] 清晰 README；
- [ ] 架构图；
- [ ] 配置示例；
- [ ] 一键运行命令；
- [ ] Benchmark 原始命令与环境；
- [ ] Before/After；
- [ ] 已知限制；
- [ ] 简历描述；
- [ ] 3～5 分钟 Demo 脚本。

---

## 34. 推荐的下一步

从阶段 0 开始，不要先创建最终目录中的全部文件。第一个 PR 只完成：

```text
feat/bootstrap-project
  ├── CMake 工程
  ├── CMake Presets
  ├── Boost.Asio/Boost.System 依赖
  ├── io_context + GoogleTest smoke test
  ├── 编译警告
  ├── Sanitizer 开关
  ├── 格式化配置
  ├── .gitignore
  └── 最小 README
```

它的验收命令只有：

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

这三个命令稳定通过后，再进入 Asio 同步 TCP 基线。不要跨阶段同时开发 Parser、Coroutine Session、连接池和代理，否则任何故障都会难以定位。

---

## 35. 官方参考资料

- [CMake 官方教程](https://cmake.org/cmake/help/latest/guide/tutorial/index.html)
- [CMake Presets 官方手册](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html)
- [GoogleTest CMake Quickstart](https://google.github.io/googletest/quickstart-cmake.html)
- [Boost.Asio Overview](https://www.boost.org/doc/libs/latest/doc/html/boost_asio/overview.html)
- [Boost.Asio Basic Anatomy](https://www.boost.org/latest/doc/html/boost_asio/overview/basics.html)
- [Boost.Asio C++20 Examples](https://www.boost.org/doc/libs/latest/doc/html/boost_asio/examples/cpp20_examples.html)
- [Boost.Asio Platform-Specific Implementation](https://www.boost.org/doc/libs/latest/doc/html/boost_asio/overview/implementation.html)
- [Boost.Beast HTTP](https://www.boost.org/doc/libs/latest/libs/beast/doc/html/beast/using_http.html)
- [Linux epoll(7) 手册](https://man7.org/linux/man-pages/man7/epoll.7.html)
- [GitHub Flow](https://docs.github.com/en/get-started/using-github/github-flow)
- [GitHub Pull Request 文档](https://docs.github.com/en/pull-requests/get-started/about-pull-requests)
- [GitHub Actions Quickstart](https://docs.github.com/en/actions/get-started/quickstart)
- [Docker 多阶段构建](https://docs.docker.com/build/building/multi-stage/)
- [Docker Compose 文档](https://docs.docker.com/compose/)
- [Clang AddressSanitizer](https://clang.llvm.org/docs/AddressSanitizer.html)
- [Clang ThreadSanitizer](https://clang.llvm.org/docs/ThreadSanitizer.html)
- [clang-tidy](https://clang.llvm.org/extra/clang-tidy/)
- [wrk](https://github.com/wg/wrk)
- [Prometheus Exposition Formats](https://prometheus.io/docs/instrumenting/exposition_formats/)

阅读顺序建议：先看 CMake/GoogleTest 和 Asio Basic Anatomy，再学习官方 Coroutine 示例；完成单线程 Session 后再深入 Executor、strand、取消和 composed operation。`epoll` 手册用于可选底层实验，不是主线实现前置条件。
