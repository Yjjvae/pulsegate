# PulseGate

PulseGate 是一个用 C++20 和 Boost.Asio 逐步实现的 HTTP 网关学习项目。

当前开发进度是教程第 13 章“主动健康检查与上游连接池”。当前开发版本为 `0.7.0`；最近发布
标签为 `v0.6.0`。主程序使用一个 `io_context` 和可配置数量的工作线程；每条
Session 仍有自己的 strand，因此并发不会让单条连接的状态并行修改。

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
- generation-safe `steady_timer` Deadline，以及 Header、Body 和 Keep-Alive 空闲超时；
- 带唯一 `StopReason` 的 Session 状态机、连接注册表、连接上限和优雅 drain；
- `AsioRuntime` 管理一个 `io_context`、work guard 与可配置的 `std::jthread` worker；
- Session strand 串行化单连接状态；不同连接可由不同 worker 并行推进；
- coroutine Router：精确/前缀匹配、405 Allow、请求 ID、异常到 500 的统一边界；
- `/livez`、`/readyz`、`/metrics`、`/api/version`、`POST /echo`，以及可选静态文件；
- 有界专用文件线程池，拒绝路径穿越、符号链接逃逸和超限静态文件；
- `async_resolve` / `async_connect` 协程代理、Round Robin 上游选择和结构化 502/503/504 映射；
- 上游响应 Parser（Content-Length、chunked、1xx、EOF 边界与上限），以及受控 chunked 下游流式写入；
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
./build/debug/app/pulsegate --listen 127.0.0.1:8080 --threads 4
```

另开一个终端验证健康检查：

```bash
curl --noproxy '*' --include http://127.0.0.1:8080/healthz
```

预期 Body 为 `ok`。当前支持 HTTP/1.0、HTTP/1.1、`Content-Length`、Keep-Alive 和
顺序处理的 Pipelining；慢 Header、慢 Body 和空闲 Keep-Alive 会按期限关闭。
暂不支持 chunked 请求；默认 worker 数为硬件并发数，使用 `--threads N` 覆盖。不要在
`io_context` worker 中执行同步 DNS、磁盘读取或长 CPU 任务。

启用受限静态文件服务（`/static/*`）时显式给出 document root：

```bash
./build/debug/app/pulsegate --listen 127.0.0.1:8080 --threads 4 \
  --document-root ./public
curl --noproxy '*' --include http://127.0.0.1:8080/livez
curl --noproxy '*' --include http://127.0.0.1:8080/static/index.html
```

静态文件默认最多 256 KiB；`..`、编码后的 `..`、NUL、反斜杠及逃出 document root 的
符号链接会被拒绝。文件 I/O 不在 I/O worker 上执行，队列满时返回 503。

启用反向代理时可重复传入上游地址，所有请求到 `/proxy/*` 将按 Round Robin 选择一个上游：

```bash
python3 tools/mock_upstream.py --port 9001 --name upstream-a
python3 tools/mock_upstream.py --port 9002 --name upstream-b --chunked

./build/debug/app/pulsegate --listen 127.0.0.1:8080 --threads 4 \
  --proxy-upstream 127.0.0.1:9001 --proxy-upstream 127.0.0.1:9002
curl --noproxy '*' --include http://127.0.0.1:8080/proxy/demo
```

代理会移除 hop-by-hop Header，并重建 `Host`、`X-Forwarded-For`、`X-Forwarded-Proto` 与
`X-Request-Id`。下游响应在上游 Header 到达后使用 chunked 编码逐块写出；每次写完成前不会
继续读取下一块上游数据。请求 Body 仍由当前下游 Parser 完整、有上限地读入后才转发。

每个上游端点都有一个 strand-owned 连接池：请求先异步取得独占 Lease；只有完整解析响应、上游
没有声明 `Connection: close`、没有残留字节且没有超时/取消时，连接才会归还。默认主动探测
`/healthz`，2 秒一轮；连续 3 次失败会摘除端点，连续 2 次成功会恢复。Health Probe 使用独立连接，
不会占用业务池。WebSocket Upgrade、请求 chunked 和自动重试尚未实现。

要手工确认连接复用，可让 mock 保持上游连接：

```bash
python3 tools/mock_upstream.py --port 9001 --name upstream-a --keep-alive
./build/debug/app/pulsegate --listen 127.0.0.1:8080 --threads 1 \
  --proxy-upstream 127.0.0.1:9001
curl --noproxy '*' http://127.0.0.1:8080/proxy/one
curl --noproxy '*' http://127.0.0.1:8080/proxy/two
```

mock 输出中两条业务请求应具有同一个 `connection=N`；健康检查会额外建立独立连接，这是预期行为。

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
