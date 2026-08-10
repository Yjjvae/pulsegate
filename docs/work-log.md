# PulseGate 工作日志

本日志从教程第四章开始记录项目的实际推进过程。日期统一使用
`Asia/Shanghai` 时区；每条记录包含完成内容、验证结果、重要决策和遗留事项。

## 2026-07-29

### 第四章：Git 与 GitHub 工作流准备

完成内容：

- 编写轻量 GitHub Flow、分支命名和 Conventional Commits 规范；
- 添加 Pull Request 模板；
- 添加 Bug 和 Feature Issue 模板；
- 添加 `.gitignore`，排除构建目录、编辑器配置、本地配置和性能分析产物；
- 编写 `CONTRIBUTING.md` 和本地开发说明。

重要决策：

- `main` 只接收通过 Pull Request 合入的完整变更；
- 功能分支使用 `feat/`、`fix/`、`test/`、`docs/`、`build/` 等前缀；
- 一个 Commit 只表达一个逻辑变化；
- 不向仓库提交 Token、私钥、密码、`.env` 和真实生产配置。

遗留事项：

- 当时工作区的 `.git` 是只读占位目录，尚不能创建真实 Git 历史；
- 远程仓库、Milestone 和 `main` Ruleset 尚未配置。

### 第五章：阶段 0 工程骨架

完成内容：

- 创建 target-based CMake 工程；
- 建立 `pulsegate_core` 与 `pulsegate_runtime` 依赖边界；
- 配置 C++20、严格标准模式和编译警告；
- 配置 Debug、Release、ASan/UBSan 和 TSan 独立 Preset；
- 接入 Boost 1.83+、Threads 和固定版本 GoogleTest；
- 添加最小应用入口、版本接口和 Boost.Asio smoke test；
- 添加 clang-format、clang-tidy、Sanitizer 和警告即错误配置。

验证结果：

- Debug 构建成功，2/2 测试通过；
- Release 构建成功；
- ASan/UBSan 构建成功，2/2 测试通过；
- TSan 构建成功，2/2 测试通过；
- 独立 `-Werror` 构建成功；
- 应用输出版本 `0.0.1`。

重要决策：

- Boost.System 使用 header-only 方式，不强制查找已在 Boost 1.89 移除的兼容二进制；
- GoogleTest 固定到完整 Commit，并校验下载归档的 SHA-256；
- 测试发现使用 `DISCOVERY_MODE PRE_TEST`，兼容 Sanitizer 环境；
- 不提前创建后续章节才需要的空模块目录。

## 2026-07-30

### 第四章：启用真实 Git 仓库

完成内容：

- 在项目根目录初始化 Git 仓库；
- 将默认分支设置为 `main`；
- 配置当前仓库的 Commit 姓名和邮箱；
- 检查 `.gitignore`，确认 `build/` 产物不会进入版本历史；
- 创建第一次 Commit：
  `2c2230e build(cmake): bootstrap C++20 Boost.Asio project`；
- 检查 Git 对象完整性，仓库状态正常。

验证结果：

- 仓库根目录正确；
- `main` 正确指向第一次 Commit；
- 本地 `user.name` 和 `user.email` 已配置；
- Git 对象完整性检查通过；
- `build/debug/app/pulsegate` 命中 `/build/` 忽略规则。

遗留事项：

- 尚未配置 GitHub `origin`；
- `main` 和 `v0.0.1` 尚未推送；
- GitHub `v0.1.0` Milestone、Issue 和 `main` Ruleset 尚未创建；
- `build-and-test` 必需检查要在 CI 章节的 Workflow 首次成功运行后启用。

## 2026-07-31

### 第四章：工作日志与本地版本发布

完成内容：

- 清理由误重定向产生的未跟踪 `git diff` 输出文件；
- 建立按日期维护的工作日志，并从第四章开始补录实际进度；
- 在 README 中添加工作日志入口；
- 为日志和导航创建独立文档 Commit；
- 创建本地 annotated tag `v0.0.1`，指向完整的阶段 0 快照。

验证结果：

- Debug 测试保持 2/2 通过；
- 文档暂存差异通过 `git diff --cached --check`；
- 工作区恢复干净状态；
- `v0.0.1` 正确指向 `main` 当前阶段 0 Commit；
- Git 对象完整性检查通过。

遗留事项：

- 尚未配置 GitHub `origin`；
- `main` 和 `v0.0.1` 尚未推送；
- GitHub `v0.1.0` Milestone、Issue 和 `main` Ruleset 尚未创建；
- `build-and-test` 必需检查要在 CI 章节的 Workflow 首次成功运行后启用。

## 2026-08-02

### 开发工具：Codex CLI MCP 与 GitHub 远程协作

完成内容：

- 将本地仓库关联到 `https://github.com/Yjjvae/http_server.git`；
- 确认本地 `main` 跟踪 `origin/main`；
- 为 Ubuntu Codex CLI 添加官方 Playwright MCP；
- 将 Playwright 配置为无界面的 Chromium 模式；
- 下载与当前 Playwright MCP 版本匹配的 Chromium、Headless Shell 和 FFmpeg；
- 添加 Context7 远程 MCP，并完成 OAuth 授权；
- 添加 GitHub 官方远程 MCP，使用环境变量读取细粒度访问令牌；
- 使用 GitHub MCP 读取账号信息、仓库根目录和最近 Commit；
- 使用 Context7 查询 Boost.Asio 的 `io_context` 文档。

验证结果：

- Chromium 与 Headless Shell 均能报告版本 `151.0.7922.10`；
- Context7 能识别 `/boostorg/asio` 并返回有效文档和代码示例；
- GitHub MCP 成功认证为 `Yjjvae`；
- GitHub MCP 能读取 `Yjjvae/http_server` 的目录和两个已有 Commit；
- 整个验证过程没有创建 Issue、Pull Request 或远程 Commit。

重要决策：

- Playwright 使用 `--headless --browser chromium`，适配 Ubuntu CLI 环境；
- GitHub Token 不写入 Codex 配置、项目文件或 Git 历史；
- GitHub MCP 通过 `GITHUB_PAT_TOKEN` 环境变量读取凭据；
- MCP 只按实际用途安装，避免重复添加文件系统、Shell 或 Git 工具。

遗留事项：

- GitHub MCP 写权限将在首次真实创建 Issue 或 Pull Request 时验证；
- 远程 `v0.0.1` 标签状态尚未独立确认；
- Playwright MCP 的完整网页操作将在需要端到端测试时验证。

## 2026-08-03

### 开发环境：VS Code 与 CMake 自动配置行为确认

完成内容：

- 检查项目中的 `CMakeLists.txt` 和 `CMakePresets.json`；
- 确认项目没有提交 `.vscode/settings.json`；
- 明确 VS Code 打开项目后自动配置来自 CMake Tools 的
  `cmake.configureOnOpen` 默认行为；
- 梳理“配置”和“构建”的边界，以及配置结果如何服务于代码补全；
- 补录 8 月 2 日和 8 月 3 日的工程工作日志。

验证结果：

- `CMakePresets.json` 中的 Debug、Release、ASan 和 TSan Preset 均保持完整；
- Debug Preset 的生成目录仍为 `build/debug`；
- 本次检查没有修改 CMake 配置或构建目标。

重要决策：

- 当前保留打开项目时自动配置，减少初学阶段手动同步构建信息的成本；
- 不为关闭自动配置而新增 `.vscode/settings.json`；
- 将 CMake 配置理解为“生成构建规则”，与实际编译步骤分开学习。

遗留事项：

- `.playwright-mcp/` 已确认为可再生成的运行产物，并加入 `.gitignore`。

### 第四章：GitHub 工作流收尾

完成内容：

- 将本地工作日志 Commit 推送到 `origin/main`；
- 在 GitHub 创建 `v0.1.0` Milestone；
- 为同步 TCP 接收、最小 GET 解析、HTTP 响应和回环集成测试创建 Issue；
- 创建 `docs/chapter-4-closeout` 分支；
- 使用真实文档变更演练分支、Commit、Push 和 Pull Request 流程；
- 添加仓库简介和 C++/Boost.Asio/CMake 相关 Topics；
- 将 Pull Request 合并策略收敛为 squash merge；
- 开启合并后自动删除远端分支；
- 核对远端 Tag、分支、Issue、PR、Release、Actions 和仓库合并设置。

验证结果：

- 远端 `v0.0.1` Tag 正确指向阶段 0 快照；
- `v0.1.0` Milestone 和四个 Issue 创建成功；
- Bug 与 Feature Issue 模板引用的默认标签均存在；
- GitHub MCP 对私有仓库具备读取、Issue 和 Pull Request 权限；
- 首个文档 Pull Request `#5` 已通过 squash merge 合入 `main`；
- 仓库只允许 squash merge，并会自动删除已合并分支；
- 本次没有修改 C++、CMake 或测试代码。

重要决策：

- 从后续功能开发开始使用短生命周期分支和 Pull Request；
- `v0.1.0` 的 Issue 直接对应下一阶段可验收的小任务；
- 主线只保留 squash 后的完整逻辑变化，不开放 merge commit 和 rebase merge；
- 仓库仍保持 Private，不在未确认公开范围前改变可见性；
- CI 状态检查等到创建 `build-and-test` Workflow 后再设为必需检查。

遗留事项：

- Private 免费仓库不能启用所需 Ruleset；公开仓库后再保护 `main`；
- `build-and-test` Workflow 按后续 CI 章节实现。

## 2026-08-10

### 第六章：Boost.Asio 同步 TCP/HTTP 基线

完成内容：

- 创建 `feat/sync-http-baseline` 功能分支；
- 新增 `pulsegate_net` 静态库，将网络实现与阶段 0 核心库分开；
- 使用 Boost.Asio 完成同步 accept、分段 read 和完整 write 流程；
- 新增独立程序 `pulsegate_sync_baseline`，支持
  `--listen HOST:PORT`、IPv4、方括号 IPv6 和端口 `0`；
- 设置 `SO_REUSEADDR` 与可配置 backlog，并提供包含操作上下文的启动错误；
- 实现最小 GET 请求行校验，以及 200、400、405 和 431 响应；
- 将请求头限制为 16 KiB，客户端 EOF、连接重置和提前关闭均隔离在单条连接；
- 新增 9 个真实回环网络集成测试；
- 更新 README、开发验收命令和第六章工作日志。

验证结果：

- Debug 与 Release 构建成功；
- Debug 下 11/11 测试通过；
- ASan/UBSan 下 11/11 测试通过，没有 Sanitizer 报告；
- `PULSEGATE_WARNINGS_AS_ERRORS=ON` 构建成功；
- 100 个串行 TCP 请求全部得到 `HTTP/1.1 200 OK`；
- `curl --noproxy '*'` 手工请求得到 `Content-Length: 12` 和
  `hello world\n`；
- 服务器析构后，同一端口能够重新绑定。

重要决策：

- 同步服务器单独构建为教学基线，现有 `pulsegate` 主程序不链接网络实现；
- 预期的连接错误使用 `boost::system::error_code`，启动配置错误由 `main()` 捕获；
- 使用 `boost::asio::write` 处理短写，不直接假设一次写操作能发送完整响应；
- 测试绑定 `127.0.0.1:0`，避免硬编码端口和并行测试冲突；
- 本阶段不引入线程池、异步回调或并发性能声明。

遗留事项：

- 同步模型会被慢客户端阻塞，这是下一阶段异步架构要解决的问题；
- 完整 HTTP/1.1 增量解析、持久连接和协议限制将在第七章实现；
- CI Workflow 与 `build-and-test` 必需检查仍按后续 CI 章节实现。

## 2026-08-10

### 第七章：HTTP/1.1 增量解析

完成内容：

- 新增带读写下标、可扩容且有绝对容量上限的 `net::Buffer`；
- 新增大小写不敏感的 Header 容器、请求对象和拥有字节的响应序列化；
- 实现 HTTP/1.0/1.1 请求行、Host、`Content-Length`、Keep-Alive 解析；
- 实现增量状态机，支持分片输入并保留 pipelining 的后续请求字节；
- 限制请求行、Header 总量、Header 数量、单 Header 和 Body 大小；
- 拒绝冲突 `Content-Length`、裸 CR/LF 注入和未支持的
  `Transfer-Encoding`；
- 新增 Buffer、Parser 和 Response 共 21 个单元测试。

重要决策：

- Parser 不依赖 Boost.Asio Socket，只消费 `Buffer`，因此协议测试无需网络；
- Header 名称统一小写存储，值保留原内容；
- v0.2 明确拒绝所有请求 `Transfer-Encoding`，不伪装支持 chunked；
- 响应在 `async_write` 前序列化为独立 `std::string`，保证协程挂起期间内存有效。

### 第八章：单线程 Boost.Asio 协程服务器

完成内容：

- 新增 `spawnGuarded`，为顶层协程统一收口未知异常；
- 新增带 strand 的 `Listener`，支持跨线程 `stop()`、accept 重试退避和端口 0；
- 新增 `HttpSession`，使用 `async_read_some`、`async_write` 和 Parser 完成
  Keep-Alive/Pipelining 循环；
- 将主程序升级为单线程 C++20 Coroutine HTTP Server，并处理 SIGINT/SIGTERM；
- 提供 `/healthz`，并实现 404、405 和 Parser 错误的 HTTP 映射；
- 新增 10 个回环集成测试，覆盖慢客户端、100 连接、提前断开、停止 accept、
  分片和 pipelining；
- 项目版本升级到 `0.2.0`，补充阶段验收和基准记录模板。

验证结果：

- 第 7、8 章定向测试 31/31 通过；
- Debug、ASan/UBSan、TSan 下完整测试均为 42/42 通过；
- Release 和 `PULSEGATE_WARNINGS_AS_ERRORS=ON` 构建成功；
- Release 手工 `curl --noproxy '*' http://127.0.0.1:18081/healthz` 返回
  `HTTP/1.1 200 OK`、`Content-Length: 3` 和 `ok\n`；
- Release `wrk -t2 -c100 -d30s --latency` 基准完成：133,476.87 RPS，
  平均延迟 744.30 µs，P99 1.58 ms，4,004,433 requests 且无 socket error。

遗留事项：

- 当前没有超时、取消原因分类和完整 Session Registry，这些是第九章范围；
- 多线程 `io_context`、业务路由和完整 HTTP/1.1 chunked 解析尚未引入；
- 基准结果必须来自 Release 可执行程序，不能用 Debug 或同步基线替代。

## 2026-08-10

### 第九章：超时、取消与连接生命周期

完成内容：

- 新增 `net::Deadline`：基于 `steady_timer`、generation ticket 和共享所有权，旧的
  `async_wait` 即使在取消后恢复，也不会误取消下一次读操作；
- 新增 `ScopeExit`，保证读协程无论正常完成、网络错误或因取消返回都会 disarm deadline；
- 为 `HttpSession` 建立 `Created → Running/Draining → Closing → Closed` 生命周期和
  唯一的 `StopReason`；关闭只执行一次，并依次取消 timer、cancel/shutdown/close socket、
  从 Registry 移除并记录原因；
- 为 Header、`Content-Length` Body 和 Keep-Alive 空闲读分别配置期限，默认值为
  10 秒、30 秒和 15 秒；
- 新增 mutex 保护的弱引用 `SessionRegistry`，支持连接上限、统一 drain、强制关闭和
  关闭原因计数；
- `HttpServer::stop()` 先停止 Listener，再请求 Session drain。正在读的慢客户端以
  `ServerShutdown` 取消，已开始响应的请求在写完后关闭；主程序收到信号后不再直接
  `io_context.stop()`，而是让取消处理与清理 handler 正常执行；
- 项目开发版本升级到 `0.3.0`，新增 2 个 Deadline 单元测试和 5 个回环集成测试。

验证结果：

- Debug 完整 CTest：49/49 通过；
- 定向覆盖：Header timeout、Body timeout、Idle timeout、stop/timeout 竞争、连接上限、
  Deadline disarm 和重新 arm；
- 已使用 `std::string_view` 写入无 `Connection: close` 的测试请求，避免 C 字符串数组
  末尾的 `NUL` 被 `asio::buffer` 当作额外协议字节，从而误分类为 Header timeout。

重要决策：

- `HttpSession::run()` 在协程帧中显式持有 `shared_ptr<HttpSession>`：成员协程自身只保存
  裸 `this`，不能把 Registry 的 `weak_ptr` 当作生命周期保证；
- Registry 不强持有 Session，避免结束连接被容器意外延长寿命；线程安全由 Mutex 提供，
  为第十章多线程 `io_context` 保留边界；
- 协议 deadline 使用 `steady_clock`，不依赖会跳变的 wall clock；
- 资源上限拒绝发生在 accept 后、Session 启动前：直接关闭 socket 并记录
  `ResourceLimit`，不为未解析请求伪造 HTTP 响应。

遗留事项：

- 第十章将引入多线程 `io_context`、工作线程管理和更严格的 strand 验证；
- 目前没有写操作 deadline；反向代理和上游 DNS/连接/响应期限会在引入上游客户端后实现；
- 发布 `v0.3.0` 标签、PR 合并和 CI 检查将在本阶段工程收尾时完成。

## 后续记录模板

```markdown
## YYYY-MM-DD

### 第 N 章：主题

完成内容：

- ...

验证结果：

- ...

重要决策：

- ...

遗留事项：

- ...
```
