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
