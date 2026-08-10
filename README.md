# PulseGate

PulseGate 是一个用 C++20 和 Boost.Asio 逐步实现的 HTTP 网关学习项目。

当前开发进度是教程第 8 章“单线程 Boost.Asio 协程服务器”；最近一个发布标签
是同步基线的 `v0.1.0`。当前主程序能在一个 `io_context` 线程上管理多个连接，
但这仍是异步正确性基线，不是多线程性能版本。

完整教学见 [PROJECT_TUTORIAL.md](PROJECT_TUTORIAL.md)。

按日期记录的实际推进过程见 [docs/work-log.md](docs/work-log.md)。

## 当前能力

- target-based CMake 工程；
- GCC/Clang 高等级编译警告；
- Boost.Asio 与 header-only Boost.System 依赖边界；
- 基于 Boost.Asio 的同步 TCP accept/read/write 闭环；
- 带读写下标和绝对上限的 Asio-friendly 字节 Buffer；
- 支持分段输入、`Content-Length`、Keep-Alive 与顺序 Pipelining 的 HTTP/1.1 解析器；
- 安全 Header 处理、响应序列化，以及 400、405、413、431、501 错误映射；
- 单线程 C++20 Coroutine Listener/Session，慢客户端不会阻塞其他连接；
- GoogleTest + CTest；
- Debug、Release、ASan/UBSan、TSan 独立预设；
- clang-format、clang-tidy 与 GitHub 协作模板。

## 环境要求

- Linux；
- CMake 3.24+；
- 支持 C++20 的 GCC 12+ 或 Clang 16+；
- Boost 1.83+ 头文件（Asio 与 System）；
- Make 或 Ninja；
- Git。

如果系统没有 GoogleTest，首次 Debug/ASan/TSan 配置会从官方仓库下载固定的
GoogleTest 1.17.0 源码并校验 SHA-256。之后复用对应构建目录中的依赖。

Ubuntu 24.04 的基础依赖示例：

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build \
  libboost-dev git
```

## 构建与测试

预设没有硬编码生成器：安装 Ninja 的环境可以设置
`CMAKE_GENERATOR=Ninja`，否则 CMake 会使用本机默认生成器。

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug

./build/debug/app/pulsegate --version
./build/debug/app/pulsegate --help
```

启动当前异步服务器：

```bash
./build/debug/app/pulsegate --listen 127.0.0.1:8080
```

另开一个终端验证健康检查：

```bash
curl --noproxy '*' --include http://127.0.0.1:8080/healthz
```

预期 Body 为 `ok`。当前支持 HTTP/1.0、HTTP/1.1、`Content-Length`、Keep-Alive 和
顺序处理的 Pipelining；暂不支持 chunked 请求、超时或多线程 I/O。

第 6 章同步基线仍保留为独立学习程序：

```bash
./build/debug/app/pulsegate_sync_baseline --listen 127.0.0.1:8080
```

另开一个终端验证响应。如果本机设置了 HTTP 代理，`--noproxy '*'` 可以确保请求
直接访问回环地址：

```bash
curl --noproxy '*' --include http://127.0.0.1:8080/health
```

预期 Body 为 `hello world`。它有意一次只处理一条连接，方便与当前异步实现对照。

Release 构建：

```bash
cmake --preset release
cmake --build --preset release
```

Sanitizer 构建相互隔离：

```bash
cmake --preset asan
cmake --build --preset asan
ctest --preset asan

cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan
```

不要把 ASan/UBSan 与 TSan 放进同一个二进制；项目配置会主动拒绝这种组合。

## 开发检查

格式化本阶段源码：

```bash
clang-format -i \
  app/*.cpp include/pulsegate/core/*.h include/pulsegate/http/*.h \
  include/pulsegate/net/*.h include/pulsegate/runtime/*.h \
  src/core/*.cpp src/http/*.cpp src/net/*.cpp tests/unit/*.cpp \
  tests/integration/*.cpp
```

通过构建生成的编译数据库运行静态检查：

```bash
clang-tidy -p build/debug \
  app/pulsegate_main.cpp app/pulsegate_sync_main.cpp \
  src/core/version.cpp src/http/*.cpp src/net/*.cpp \
  tests/unit/smoke_test.cpp tests/integration/*.cpp
```

临时将警告视为错误：

```bash
cmake -S . -B build/werror \
  -DPULSEGATE_BUILD_TESTS=ON \
  -DPULSEGATE_WARNINGS_AS_ERRORS=ON
cmake --build build/werror
```

## Git 工作流

开发约定集中在 [CONTRIBUTING.md](CONTRIBUTING.md)。建议一个 Issue 对应一个
短生命周期分支，通过 Pull Request 合入 `main`。提交信息使用：

```text
<type>(<scope>): <imperative summary>
```

例如：

```text
build(cmake): bootstrap C++20 Boost.Asio project
test: add initial GoogleTest target
```

## 当前目录

```text
.
├── .github/                 # Issue 与 PR 模板
├── app/                     # 可执行程序装配
├── cmake/                   # 项目选项、Sanitizer、依赖
├── docs/                    # 开发文档
├── include/pulsegate/       # 公共头文件
├── src/                     # 库实现
├── tests/                   # 单元测试与真实回环网络集成测试
├── CMakeLists.txt
├── CMakePresets.json
└── PROJECT_TUTORIAL.md
```

只创建当前阶段需要的目录；后续模块在对应章节加入，避免堆积空文件。
