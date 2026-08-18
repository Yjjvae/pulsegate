# PulseGate

PulseGate 是一个用 C++20 和 Boost.Asio 逐步实现的 HTTP 网关学习项目。

当前开发进度是教程第 24 章“GitHub Actions CI”。当前开发版本为 `0.9.3`；最近发布
标签为 `v0.8.2`。主程序使用一个 `io_context` 和可配置数量的工作线程；每条
Session 仍有自己的 strand，因此并发不会让单条连接的状态并行修改。

完整教学见 [PROJECT_TUTORIAL.md](PROJECT_TUTORIAL.md)。

按日期记录的实际推进过程见 [docs/work-log.md](docs/work-log.md)。

从零启动 Docker Compose 演示环境见 [docs/docker-quickstart.md](docs/docker-quickstart.md)。

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
- 带唯一 `StopReason` 的 Session 状态机、连接注册表、连接上限，以及带 deadline 的优雅 drain；
- `AsioRuntime` 管理一个 `io_context`、work guard 与可配置的 `std::jthread` worker；
- Session strand 串行化单连接状态；不同连接可由不同 worker 并行推进；
- coroutine Router：精确/前缀匹配、405 Allow、请求 ID、异常到 500 的统一边界；
- `/livez`、`/readyz`、`/metrics`、`/api/version`、`POST /echo`，以及可选静态文件；
- 有界专用文件线程池，拒绝路径穿越、符号链接逃逸和超限静态文件；
- `async_resolve` / `async_connect` 协程代理、Round Robin 上游选择和结构化 502/503/504 映射；
- 上游响应 Parser（Content-Length、chunked、1xx、EOF 边界与上限），以及受控 chunked 下游流式写入；
- Token Bucket 全局/路由限流：支持按客户端 IP 分桶、分片 TTL 清理、最大 key 数、429/Retry-After 与聚合拒绝指标；
- 路由显式启用的分片 LRU + TTL 响应缓存：按总字节数有界淘汰，保护 Authorization/Cookie/Set-Cookie 等敏感响应；
- 每端点 Closed/Open/Half-Open 熔断器：连续上游失败后快速拒绝，冷却后限制探测请求；
- 代理全局 in-flight 上限、连接池 waiter 上限与显式 503/Retry-After 过载反馈；
- 集中管理、固定版本的 yaml-cpp 配置依赖；YAML 配置解析、聚合校验与不可变 reload 快照；
- 有界异步 JSON/text access log、日志丢弃计数，以及低基数 Prometheus `/metrics`；
- GoogleTest + CTest；
- HTTP Parser 的 Clang libFuzzer 目标、语料目录和最小化崩溃回归流程；
- Debug、Release、ASan/UBSan、TSan 独立预设；
- clang-format、clang-tidy、Boost.Asio handler tracking 与 GitHub 协作模板；
- GitHub Actions：GCC/Clang、ASan/UBSan、定期 TSan、格式化、静态分析与 Docker Compose 冒烟测试。

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

启用全局限流时，`--rate-limit` 和 `--rate-burst` 必须成对给出；添加
`--rate-per-client` 后按 TCP 对端 IP 分桶。网关不会直接信任客户端可伪造的
`X-Forwarded-For`。`/proxy/*` 也可单独使用 `--proxy-rate-limit`、`--proxy-rate-burst`
与 `--proxy-rate-per-client`，在建立上游连接前拒绝超额请求：

```bash
./build/debug/app/pulsegate --listen 127.0.0.1:8080 --threads 4 \
  --rate-limit 100 --rate-burst 200 --rate-per-client \
  --proxy-upstream 127.0.0.1:9001 \
  --proxy-rate-limit 20 --proxy-rate-burst 40 --proxy-rate-per-client
```

超额请求返回 `429 Too Many Requests` 和向上取整的 `Retry-After` 秒数。`/metrics`
额外输出 `pulsegate_rate_limit_requests_total`，仅以 `scope`（global/route）和
`outcome`（allowed/rejected/key_capacity）聚合，绝不把 IP 用作标签。

为 `/proxy/*` 启用缓存时，TTL 和总容量必须成对指定。缓存只接受 GET 的 200 响应；HEAD
只读取已有的 GET 缓存，绝不会用无 Body 的 HEAD 响应覆盖它。带 `Authorization`、`Cookie` 的
请求，以及带 `Set-Cookie`、`Cache-Control: private/no-store` 或 `Vary: *` 的响应都不会缓存：

```bash
./build/debug/app/pulsegate --listen 127.0.0.1:8080 --threads 4 \
  --proxy-upstream 127.0.0.1:9001 \
  --proxy-cache-ttl-ms 30000 --proxy-cache-max-bytes 4194304 \
  --proxy-cache-entry-max-bytes 262144 --proxy-cache-shards 16
```

缓存 key 包含固定 `http` scheme、规范化 Host、路径、Query 和可配置的 Vary 请求头；当前 CLI
未开放自定义 Vary，因此默认不按额外 Header 分片。命中响应附带 `X-Cache: HIT`，成功填充为
`MISS`，不满足策略或容量时为 `BYPASS`。启用缓存的代理 miss 会完整缓冲上游响应，确保只有可接受
的完整 200 响应入缓存；未启用缓存时仍使用原有流式代理路径。`/metrics` 同时输出
`pulsegate_response_cache_operations_total` 的 hit/miss/store/eviction/expired 聚合计数。

代理还在上游连接池之前执行全局 in-flight 准入；达到默认 1024 个活动代理事务时，立即返回
`503 Service Unavailable` 与 `Retry-After: 1`，不会继续积压协程或创建连接。每个上游端点另有独立
熔断器：默认连续 3 个**上游**失败（连接、超时、协议错误，或启用了默认策略的 5xx）会进入 Open，
持续 5 秒；Open 期间不会访问该端点，冷却后只允许 1 个 Half-Open 探测。成功探测恢复 Closed，失败
探测重新开始冷却。客户端断开、服务器停机、路由限流与池准入失败不计为上游失败。连接池已限制每端点
连接数和 waiter 数；流式代理每次仅在下游上一块写完后读取下一块上游数据，因此慢下游不会形成无界待写
队列。当前 CLI 仍使用这些安全默认值，配置文件章节会统一暴露和校验它们。

## YAML 配置

第 17 章新增 `--config FILE`。配置文件是 CLI 参数的替代入口，不能与其他运行参数混用；完整示例见
[config/pulsegate.example.yaml](config/pulsegate.example.yaml)。

```bash
cmake --preset debug
cmake --build --preset debug
./build/debug/app/pulsegate --config ./config/pulsegate.example.yaml
```

加载分为解析和校验两个阶段。错误一次性列出并带字段路径，例如未知 upstream、重复 endpoint、无效端口、
不合法的缓存 shard 预算，或 `output_low_water_bytes >= output_high_water_bytes`。yaml-cpp 固定为官方
`0.8.0` 提交并只由 `cmake/Dependencies.cmake` 管理。

进程收到 `SIGHUP` 时，`ConfigManager` 在专用 worker 解析候选配置、回到配置 strand 校验并原子发布
不可变快照；解析/校验失败时保留旧快照。监听地址、I/O 线程、Session 限制、上游和路由会明确提示
“需要重启”，不会产生只更新部分运行对象的危险热更新。当前唯一可安全发布的热更新字段是 logging 元数据；
第 18 章已将它接入异步 access logger。

## 优雅停机

收到 `SIGINT` 或 `SIGTERM` 后，PulseGate 进入 draining：先停止监听新连接，`/readyz` 改为
`503 Service Unavailable`，既有 Keep-Alive 连接不会接收下一条请求；正在执行的 HTTP/代理事务则可在
grace period 内完成。配置模式使用 `server.graceful_shutdown_ms`，CLI 模式使用 15 秒。

超过 grace period 仍未完成时，Server 会取消剩余 Session；这会级联取消其代理 Resolver、池 waiter 和
上游 socket。注册表清空后才释放 `AsioRuntime` 的 work guard，让 `io_context` 自然退出，而不是提前调用
`io_context.stop()`。

可以用下面方式做一次本机验收（端口可按需替换）：

```bash
./build/debug/app/pulsegate --listen 127.0.0.1:18080 --threads 2 &
server_pid=$!
curl --noproxy '*' http://127.0.0.1:18080/readyz
kill -TERM "$server_pid"
wait "$server_pid"
echo "$?" # 预期 0
```

若要观察 deadline 的强制取消，可在配置中将 `graceful_shutdown_ms` 调小，并让上游响应延迟更久。

## 日志与可观测性

每个已完成请求都会产生 access log。日志只保留请求 ID、方法、**不含 Query 的路径**、状态码、耗时、
输入/输出字节数、上游和缓存结果；不会记录 Authorization、Cookie 或请求 Body。请求耗时使用
`steady_clock`。`AsyncLogger` 在专用线程写出 JSON（或 YAML 配置的 `text` 格式），队列默认最多 4096 条；
队满时直接丢弃新日志，不阻塞 `io_context`，并增加 `pulsegate_logs_dropped_total`。SIGHUP 成功发布新的
logging 快照后，日志级别和格式会随即更新。

`/metrics` 采用 Prometheus text 格式，包含 HTTP 请求计数与耗时直方图、连接接受/拒绝与活动数、上游
请求/连接耗时、缓存、限流、熔断状态、活跃协程、连接池 waiter、输出 Buffer 与日志丢弃计数。指标标签
只使用 HTTP 方法、状态类别、路由名和配置的上游名；不会使用 request ID、客户端 IP、用户 ID 或完整
URL，避免不可控的时序数据基数。

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

## Fuzz 与静态分析

HTTP Parser 使用单独的 Clang/libFuzzer 构建，fuzzer 自带 ASan/UBSan。Ubuntu 上首次使用时需要安装
`clang-21` 和 `libclang-rt-21-dev`；后者提供 libFuzzer 运行时。

```bash
cmake --preset fuzz
cmake --build --preset fuzz
corpus_dir=$(mktemp -d)
cp -a tests/testdata/http/. "$corpus_dir"
./build/fuzz/tests/fuzz/pulsegate_http_parser_fuzz \
  "$corpus_dir" -dict=tests/fuzz/http_parser.dict \
  -artifact_prefix="$corpus_dir/" -runs=10000 -max_len=65536
rm -rf "$corpus_dir"
```

发现 crash 后，先让 libFuzzer 最小化输入，再将结果加入 `tests/testdata/http/` 和对应的确定性单元测试。
完整流程见 [tests/fuzz/README.md](tests/fuzz/README.md)。

clang-tidy 使用与普通 Debug 隔离的 Clang compilation database，先作为非阻断基线报告运行：

```bash
cmake --preset clang-tidy
cmake --build --preset clang-tidy
clang-tidy -p build/clang-tidy src/http/http_parser.cpp --quiet
```

排查异步 handler 调度时使用 `asio-tracking` 预设；它会产生大量诊断，只限 Debug：

```bash
cmake --preset asio-tracking
cmake --build --preset asio-tracking
./build/asio-tracking/app/pulsegate --listen 127.0.0.1:8080
```

## 性能基准

第 22 章提供可复现的 Release `/healthz` worker 对照脚本。它会关闭 access log、预热后运行多轮
`wrk`，并保存原始输出及服务端 CPU/RSS 采样；结果目录默认不纳入版本控制。

```bash
cmake --preset release
cmake --build --preset release
tools/benchmark.sh --workers 1,2,4,8 --trials 3 \
  --connections 100 --load-threads 2 --warmup 5s --duration 15s
```

完整方法、实测数据与局限见
[v0.9.3 `/healthz` 多 Worker 基线](docs/benchmarks/v0.9.3-healthz-baseline.md)。不要将单机回环 RPS
外推为生产性能，也不要在没有 profile 证据时改动热路径。

## Docker 演示环境

第 23 章提供可复现的多阶段容器构建。最终 `runtime` 镜像只包含 PulseGate、CA 证书和一个
UID/GID 为 `10001` 的无 shell 运行用户；编译器、CMake、Git、Boost 头文件与源码都留在 build
stage。`compose.yaml` 使用同一份 Dockerfile 构建两个确定性的 Python mock upstream，并让网关通过
Docker DNS 轮询它们。

需要 Docker Engine 与 Compose v2。首次运行会下载 Ubuntu 24.04 基础镜像并在 build stage 获取固定的
yaml-cpp 源码：

```bash
docker compose config
docker compose up --build -d
docker compose ps
curl --noproxy '*' --include http://127.0.0.1:8080/livez
curl --noproxy '*' --include http://127.0.0.1:8080/api/demo
docker compose logs gateway
```

重复请求 `/api/demo`，响应会在 `upstream-a` 与 `upstream-b` 间轮转。Compose 以只读根文件系统运行
所有服务，把网关 YAML 通过 `:ro` 挂载，并给 gateway 配置 `init: true`、`STOPSIGNAL SIGTERM` 和
20 秒 stop grace period；`docker compose stop gateway` 可用于验证优雅停机。验收结束后清理：

```bash
docker compose down --volumes
```

若 Docker build 所在网络必须经 HTTP(S) 代理访问 Ubuntu 软件源或 GitHub，`Dockerfile` 只在 build
stage 接收标准代理参数；不要把具体代理地址写入仓库：

```bash
docker compose build \
  --build-arg HTTP_PROXY \
  --build-arg HTTPS_PROXY \
  --build-arg NO_PROXY
```

这些值仅用于构建时下载依赖，最终 runtime 镜像不包含它们。

运行时镜像也可以单独检查：

```bash
docker build --target runtime -t pulsegate:0.9.3 .
docker run --rm --read-only --tmpfs /tmp --user 10001:10001 pulsegate:0.9.3 --version
```

基础镜像使用 Ubuntu 的固定发行版标签 `24.04`，生产发布应进一步锁定到 digest，并配合镜像扫描或 SBOM
工具持续更新。不要使用未带版本的 `latest` 作为发布镜像唯一标签。

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
clang-tidy -p build/clang-tidy \
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
