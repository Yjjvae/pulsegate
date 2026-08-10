# Local development

## 从空目录初始化 Git

```bash
git init
git branch -M main
git config user.name "你的名字"
git config user.email "你的 GitHub 邮箱"
git add .
git commit -m "build(cmake): bootstrap C++20 Boost.Asio project"
```

在 GitHub 创建名为 `pulsegate` 的空仓库后：

```bash
git remote add origin git@github.com:<用户名>/pulsegate.git
git push -u origin main
```

不要在命令、文档或配置中保存 GitHub Token 和 SSH 私钥。

## 阶段 0 验收

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug

cmake --preset release
cmake --build --preset release

cmake --preset asan
cmake --build --preset asan
ctest --preset asan

git status --short
```

预期结果：

- Debug 和 Release 构建成功；
- 两个 smoke test 通过；
- ASan/UBSan 没有报告错误；
- 编译警告为零；
- `git status` 不显示 `build/` 下的产物。

## 阶段 1：同步 HTTP 基线验收

构建并运行自动测试：

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure

cmake --preset asan
cmake --build --preset asan
ctest --preset asan --output-on-failure
```

手工启动服务器：

```bash
./build/debug/app/pulsegate_sync_baseline --listen 127.0.0.1:8080
```

保持服务器运行，另开一个终端发送请求：

```bash
curl --noproxy '*' --include http://127.0.0.1:8080/health
```

预期结果：

- 状态行为 `HTTP/1.1 200 OK`；
- `Content-Type` 为 `text/plain`；
- `Content-Length` 为 `12`；
- Body 为 `hello world\n`；
- 自动测试共 11 项并全部通过；
- ASan/UBSan 没有报告内存错误或未定义行为。

按 `Ctrl+C` 停止同步服务器。本阶段服务是刻意保留的串行基线：一个慢客户端会
阻塞后续连接，后面的异步章节将解决这个问题。

## 阶段 2：增量 HTTP 解析与单线程协程服务器验收

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure

cmake --preset release
cmake --build --preset release

cmake --preset asan
cmake --build --preset asan
ctest --preset asan --output-on-failure
```

手工运行当前异步主程序：

```bash
./build/debug/app/pulsegate --listen 127.0.0.1:8080
curl --noproxy '*' --include http://127.0.0.1:8080/healthz
```

验证 Keep-Alive 与顺序 Pipelining：

```bash
printf 'GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\nGET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n' \
  | nc 127.0.0.1 8080
```

预期结果：

- Buffer 和解析器单元测试覆盖字节级分片、Header/Body 限制和 pipelining 剩余字节；
- 一个仅发送 1 字节的客户端不会阻塞另一条连接的响应；
- 100 个连接能在单个 `io_context` 线程上完成；
- 解析错误被映射到 400、413、431 或 501，并关闭连接；
- `Listener::stop()` 取消等待中的 `async_accept`；
- ASan/UBSan 没有报告内存错误或未定义行为。

性能基准及环境信息记录在 [v0.2.0 单线程基线](benchmarks/v0.2.0-single-thread.md)。

## 阶段 3：超时、取消与连接生命周期验收

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure

cmake --preset asan
cmake --build --preset asan
ctest --preset asan --output-on-failure

cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan --output-on-failure
```

第 9 章新增的定向测试可以单独运行：

```bash
ctest --test-dir build/debug -R 'DeadlineTest|AsyncHttpServerTest.(Reclaims|StopWins|Enforces)' \
  --output-on-failure
```

预期结果：

- `Deadline` 的 `disarm()` 和重新 `arm()` 不会让旧 timer 触发新读操作；
- 慢速不完整 Header、未完成的 `Content-Length` Body 与已响应的 Keep-Alive
  分别以 `HeaderTimeout`、`BodyTimeout`、`IdleTimeout` 关闭；
- `HttpServer::stop()` 先停止 Listener，再 drain Session；正在读的慢客户端以
  `ServerShutdown` 取消，已开始写响应的请求可完成后再关闭；
- `SessionRegistry` 使用弱引用追踪存活连接，连接关闭只记一次原因，并拒绝超过
  `SessionLimits::max_connections` 的新连接；
- 测试结束后 Registry 的连接数回到零，`io_context.run()` 能自然返回。

本阶段的默认期限是 Header 10 秒、Body 30 秒、Keep-Alive 空闲 15 秒。它们由
`SessionLimits` 配置；测试使用 30 毫秒的局部配置，生产服务不应照搬这个值。

## 阶段 4：多线程 `io_context` 与 strand 验收

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure

cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan --output-on-failure
```

启动 4 个 I/O worker：

```bash
./build/debug/app/pulsegate --listen 127.0.0.1:8080 --threads 4
curl --noproxy '*' --include http://127.0.0.1:8080/healthz
```

定向验证：

```bash
ctest --test-dir build/debug \
  -R 'AsioRuntimeTest|AsyncHttpServerTest.(ServesTheSame|Serializes|AllowsDifferent|AcceptsExternal|DrainsRegistry)' \
  --output-on-failure
```

预期结果：

- `--threads` 必须是正整数；`AsioRuntime(0)` 在启动前拒绝配置；
- 1、2、4 worker 的 HTTP 行为一致；
- 每个接受到的 socket 使用独立 Session strand，因此同一 pipelined Session 的 handler
  最大并发数为 1；不同 Session 能在多个 worker 上并行推进；
- 外部线程调用 `HttpServer::stop()` 会被投递到关联 executor；`requestStop()` 仅释放
  work guard，让取消、drain 和清理 handler 自然执行；
- Registry 在 300 条多 worker 连接完成后归零，所有 Runtime worker 都会 join；
- 不要把阻塞操作放进 `io_context` worker。未来需要磁盘、DNS 或 CPU 工作时，使用有界的
  专用 `asio::thread_pool`，完成结果再切回请求的关联 executor。

`strand` 只保证关联 handler **不并发执行**，不保证固定在某个 OS 线程执行；它不能替代
Registry、Metrics 或 Cache 等跨 Session 共享数据的同步策略。

## 阶段 5：异步路由与静态资源验收

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure

ctest --test-dir build/debug \
  -R 'RouterTest|StaticFileHandlerTest|AsyncHttpServerTest.ServesDefaultAsyncRoutes' \
  --output-on-failure
```

手工准备一个非敏感目录并启动静态资源路由：

```bash
mkdir -p /tmp/pulsegate-public
printf 'hello static\n' > /tmp/pulsegate-public/index.txt
./build/debug/app/pulsegate --listen 127.0.0.1:8080 --threads 4 \
  --document-root /tmp/pulsegate-public

curl --noproxy '*' --include http://127.0.0.1:8080/livez
curl --noproxy '*' --include http://127.0.0.1:8080/api/version
curl --noproxy '*' --include -X POST --data-binary 'hello' http://127.0.0.1:8080/echo
curl --noproxy '*' --include http://127.0.0.1:8080/static/index.txt
curl --noproxy '*' --path-as-is --include http://127.0.0.1:8080/static/%2e%2e/secret
```

预期结果：

- Router 匹配和 404/405 在无 socket 的单元测试中验证；每个响应都有 `X-Request-Id`；
- `/livez` 返回 `alive\n`，`/readyz` 返回 `ready\n`，`POST /echo` 可接收拆分 Body；
- handler 异常映射为 500，不会终止 `io_context` worker；
- `/static/*` 在专用有界文件池读取，普通文件有 MIME、`Content-Length` 与 HEAD 语义；
- `..`、`%2e%2e`、NUL、反斜杠和 root 外符号链接返回 403；不存在为 404、超限为 413、
  文件队列满为 503；
- 静态文件 root 在启动期校验，生产环境不要把项目根目录或用户主目录作为 document root。

`curl` 默认会在请求发送前规范化部分路径；验证编码 `..` 时必须使用 `--path-as-is`，否则
服务端看到的是已变成 `/secret` 的路径，得到 404 并不能证明 traversal 防护生效。
