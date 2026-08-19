# PulseGate 工作日志

本日志从教程第四章开始记录项目的实际推进过程。日期统一使用
`Asia/Shanghai` 时区；每条记录包含完成内容、验证结果、重要决策和遗留事项。

## 2026-08-18

### v1.0 发布候选准备

完成内容：

- 将 CMake 项目版本提升至 `1.0.0`，并将候选二进制与 `/api/version` 标识为 `1.0.0-rc.1`；
- 新增 `CHANGELOG.md`，说明候选包含的功能、CI 修复、已知限制和最近稳定 tag；
- 新增 `docs/release-v1.0.md`，固化候选验收、性能/Compose 验收、正式 tag/Release 步骤以及不可重写 tag 的回滚策略；
- README 更新为候选状态，并使用 `pulsegate:1.0.0-rc.1` 作为容器构建示例。

验证结果：

- Debug：123/123 CTest 通过；Release 可执行文件输出 `1.0.0-rc.1`；
- ASan/UBSan（包括 `detect_stack_use_after_return=1`）：123/123 CTest 通过；
- TSan：123/123 CTest 通过；
- Parser fuzz：在临时语料上完成 10,000 次 libFuzzer smoke，未报告 ASan/UBSan 错误；
- clang-format 和 `git diff --check` 通过。
- 已完成 `1/2/4/8` worker、每组 3 次、`wrk -t2 -c100 -d15s --latency` 的候选 `/healthz` 矩阵；原始数据被忽略，汇总见 [`v1.0.0-rc.1-healthz-baseline.md`](benchmarks/v1.0.0-rc.1-healthz-baseline.md)。

## 2026-08-19

### v1.0.0 正式发布分支

完成内容：

- 将二进制与 `/api/version` 从 `1.0.0-rc.1` 提升为正式 `1.0.0`，并同步单元/集成测试、README Docker 示例和 changelog；
- 将生成的 `docs/architecture-stage5.html` 加入 `.gitignore`，避免本地演示产物进入发布历史。

验证结果：

- Debug：123/123 CTest 通过；
- Release：构建成功，`pulsegate --version` 输出 `1.0.0`；
- ASan/UBSan：123/123 CTest 通过；
- 前一候选提交上的 TSan：123/123 CTest 通过，且包含文件读取回调生命周期修复。

### CI 修复：文件读取回调的执行器生命周期

完成内容：

- 手动触发的 TSan 在 `StaticFileHandlerTest.ServesBoundedFileAndRejectsTraversal` 中发现数据竞争：文件线程池工作完成后仍可能向已经退出的调用方 `io_context` 投递回调；
- `BoundedFileService::read` 为回调执行器持有 Asio work guard，并将其传递到线程池任务及回投回调，确保回调执行前事件循环不会退出。

验证结果：

- TSan 定向测试 `StaticFileHandlerTest.ServesBoundedFileAndRejectsTraversal` 通过；
- TSan 全量 CTest：123/123 通过，未报告 ThreadSanitizer 错误；
- clang-format 与 `git diff --check` 通过。

### 第 23 章补充：Docker 快速启动教学

完成内容：

- 新增 `docs/docker-quickstart.md`，用最短路径说明 Docker 检查、Compose 启动、验证、日志、配置覆盖和清理；
- 在 README 增加 Docker 快速教程入口；
- 教程明确区分宿主机 Docker 权限问题、构建阶段代理和运行镜像，避免将代理或本地配置提交进仓库。

### 第 24 章：GitHub Actions CI

完成内容：

- 新增最小权限的 `.github/workflows/ci.yml`，在推送 `main` 和向 `main` 提交 Pull Request 时触发；
- 添加 GCC 与 Clang 的 Debug、`-Werror` 构建和 CTest 矩阵；
- 添加 ASan/UBSan 构建与测试；TSan 改为每周定时或手动触发，避免拖慢普通 Pull Request；
- 添加 `clang-format --dry-run --Werror` 和基于 `compile_commands.json` 的 clang-tidy；
- 添加 Docker Compose 端到端冒烟测试：构建、健康检查、路由请求及单个上游停止后的故障切换；
- CI 复用现有 clang toolchain：优先选择本机 `clang++-21`，不存在时使用系统提供的 `clang++`。

重要决策：

- 工作流只授予 `contents: read`，不在默认 CI 中使用 Secret 或写入仓库；
- 并发工作流按分支取消旧运行，减少同一 PR 的重复资源消耗；
- 不把依赖于 GitHub Hosted Runner 波动的 RPS 数值作为 CI 门禁。

验证结果：

- GCC Debug + `-Werror`：123/123 CTest 通过；
- Clang Debug + `-Werror`：123/123 CTest 通过；
- Clang ASan/UBSan：123/123 CTest 通过，未报告内存错误或未定义行为；
- `clang-format --dry-run --Werror` 与 clang-tidy 通过；
- `docker compose config` 通过；本机 Docker 守护进程在本次验证时不可连接，完整 Compose 冒烟测试交由首个远程 CI 运行复验。

### 第 25 章：依赖管理

完成内容：

- 新增 `docs/dependencies.md`，记录每个当前依赖的用途、来源、固定版本/commit、许可证和升级流程；
- 将 GoogleTest 改为始终使用项目固定的 hash 校验 archive，不再因开发机是否安装系统 GTest 而改变测试依赖；
- 在 CMake 配置日志中输出实际 Boost 版本及固定的 yaml-cpp、GoogleTest revision；
- GitHub Actions 从会滚动的 `ubuntu-latest` 固定到 `ubuntu-24.04`，与 Docker 的 Ubuntu 24.04 基础环境保持一致；
- 明确当前不采用 Conan/vcpkg；未来迁移必须先写 ADR 并删除旧依赖来源。

验证结果：

- 从全新 `build/dependency-audit` 目录配置时，成功解析 Boost 1.90.0、yaml-cpp 0.8.0 固定 commit 与 GoogleTest 1.17.0 的 SHA-256 archive；
- `-Werror` Debug 构建成功，123/123 CTest 通过；
- 从全新 `build/dependency-release` 目录完成 Release 构建，只解析 yaml-cpp，确认不下载测试专用 GoogleTest；
- clang-format 检查和 Git diff 格式检查通过。

### CI 修复：第三方头文件告警

完成内容：

- 根据 PR #31 的远程失败日志定位到 yaml-cpp 0.8.0 的 `-Wshadow` 告警；项目的 `-Werror` 将该第三方头文件告警升级为构建错误；
- 将 `yaml-cpp` 的公开包含目录标记为 CMake `SYSTEM`，使其以编译器的 `-isystem` 方式引入；PulseGate 自身源码仍保持全部严格告警和 `-Werror`。

验证结果：

- 全新 Clang Debug + `-Werror` 构建成功，123/123 CTest 通过；编译命令确认 yaml-cpp 使用 `-isystem`；
- 全新 Clang ASan/UBSan 构建成功，123/123 CTest 通过，未报告 Sanitizer 错误；
- `git diff --check` 通过。

### CI 修复：协程生命周期

完成内容：

- ASan 在 `AsyncHttpServerTest.StopWinsOverPendingTimeoutAndClosesOnlyOnce` 中发现 `stack-use-after-return`；根因是顶层协程由带捕获的 lambda 创建，lambda 临时对象销毁后协程仍会访问其捕获；
- 将 `spawnGuarded` 改为直接接收 `boost::asio::awaitable<void>`，从接口上杜绝该不安全的协程 lambda 用法；
- Listener、HTTP Session 与 HealthChecker 的顶层协程改为直接传入成员协程，并由完成回调持有对象的 `shared_ptr`，保证整个协程生命周期内对象有效。

验证结果：

- Clang Debug + `-Werror`：123/123 CTest 通过；
- Clang ASan/UBSan（启用 `detect_stack_use_after_return=1`）：123/123 CTest 通过；
- clang-format 与 `git diff --check` 通过。

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

## 2026-08-10

### 第十章：多线程 `io_context` 与 strand

完成内容：

- 新增 `runtime::AsioRuntime`，管理一个 `io_context`、可选 work guard 和 N 个
  `std::jthread` worker；拒绝 0 线程，重复 start 抛出错误，析构时请求自然停止并 join；
- Runtime 捕获工作线程中未观察的异常，统一报告后释放 work guard；
- 主程序改为 Runtime 装配，新增 `--threads N`；默认使用硬件并发数，SIGINT/SIGTERM
  先请求 Server drain，再释放 Runtime work guard；
- 保留 Listener strand，并确认每次 accept 均创建绑定独立 strand 的 socket；Session 的
  Socket、Parser、Buffer、Timer 和状态继续只在该 executor 上修改；
- 新增 Runtime 单元测试与多 worker 回环测试，覆盖 1/2/4 worker、同 Session 串行、跨
  Session 并行、外部线程 stop 和 300 条连接后的 Registry 清空；
- 项目开发版本升级到 `0.4.0`，补充多线程验收命令和 worker 对照基准记录。

验证结果：

- Debug、ASan/UBSan、TSan 完整 CTest 通过；
- Release 和 `PULSEGATE_WARNINGS_AS_ERRORS=ON` 构建通过；
- Release `wrk -t2 -c100 -d15s --latency` worker 对照完成：1/2/4/8 worker 分别为
  121,216.78 / 238,339.27 / 311,228.23 / 259,467.59 RPS；4 worker 的 P99 为 437 µs，
  结果和环境记录在 `docs/benchmarks/v0.4.0-worker-comparison.md`。

重要决策：

- Runtime 只管理事件循环与 worker，不拥有 HTTP 协议、Session 或业务路由；`main()` 负责
  装配并保证 Server、Registry 和 signal handler 存活到 worker join 之后；
- 停机不调用 `io_context.stop()`：Server 先取消 Listener 并 drain Session，Runtime 再
  reset work guard，让已排队的取消和清理 handler 运行；
- strand 是单 Session 的串行化边界，不是全局锁，也不保证同一连接始终在同一个线程；
- 测试中的 `sleep_for` 只用作并发探针，生产 handler 禁止阻塞 I/O worker。

遗留事项：

- 阶段 5 将把同步 `RequestHandler` 升级为 async Router，并为静态资源引入受限的专用
  blocking pool；
- raw epoll 是可选面试实验，保持与主 Boost.Asio 工程分离。

## 2026-08-10

### 第十一章：异步路由与静态资源

完成内容：

- 新增不可变快照 `Router`：路由匹配不持全局锁，配置更新通过 copy-on-write 发布；支持精确
  和前缀路由、404、405/Allow、请求 ID，以及 handler 异常到 500 的统一映射；
- `HttpSession` 改为构造 `RequestContext` 并 `co_await Router::handle()`；在移动 Request 前
  固定 Keep-Alive/HEAD 策略，避免移动后 Header 丢失导致连接关闭语义错误；
- 默认注册 `/healthz`、`/livez`、`/readyz`、`/metrics`、`/api/version` 和 `POST /echo`；
- 新增 `BoundedFileService`：专用 `asio::thread_pool`、原子队列上限、完成后投递回请求
  executor；静态文件读取不阻塞 I/O worker，队列满映射 503；
- 新增 `StaticFileHandler` 和 `--document-root PATH`：百分号解码、拒绝 NUL/反斜杠/`..`、
  canonical root 检查、拒绝符号链接逃逸、常规文件/大小检查、MIME 与 403/404/413/503/500 映射；
- 项目开发版本升级到 `0.5.0`，新增 Router、静态文件与默认异步路由测试。

验证结果：

- Debug 完整 CTest：64/64 通过；
- 覆盖路由纯逻辑、延迟协程 handler、异常 500、默认 health/liveness/version/echo、文件服务
  Busy、静态正常文件、编码 traversal、NUL、404；
- Release 手工验证 `/livez`、`POST /echo`、`/static/index.txt` 都返回 200 与
  `X-Request-Id`；`curl --path-as-is /static/%2e%2e/secret` 返回 403。
- ASan/UBSan、TSan、Release 和 `-Werror` 验证在提交前运行。

重要决策：

- `RequestContext` 只在 handler 调用期间借用；下游 Session 以 `weak_ptr` 暴露，避免路由
  handler 与 Session 形成生命周期环；
- 文件候选路径的最终 canonical 与 regular-file 检查放在文件 worker 中，降低路径检查与
  打开之间的 TOCTOU 暴露；
- 该版本仍完整缓冲 256 KiB 内的静态文件；不把它描述为 sendfile 或流式大文件服务；
- 默认应用不隐式暴露目录；只有传入 `--document-root` 才注册 `/static/*`。

遗留事项：

- 下一阶段实现协程式反向代理、上游响应解析和上游 deadline；
- 静态文件的大文件流式传输、range、ETag、缓存控制及平台 sendfile adapter 暂不实现。

## 2026-08-10

### 第十二章：Boost.Asio 协程式反向代理

完成内容：

- 新增 `HttpResponseParser`：增量处理 Content-Length、chunked、HEAD、1xx 中间响应、204/304、
  EOF 定界 Body，以及 Header/Body 绝对上限、冲突长度与提前 EOF 错误；
- 新增显式状态机 `ProxySession`：Resolver、Socket、阶段 deadline、总 deadline 与取消均绑定到
  下游 Session executor；使用 `async_resolve`、`async_connect`、`async_write`、`async_read_some`；
- 新增 `/proxy/*` Router handler 与重复 `--proxy-upstream HOST:PORT` 参数；所有当前配置的上游
  采用原子 Round Robin 选择；
- 请求方向移除 hop-by-hop Header 及 `Connection` 点名字段，重建 `Host`、`X-Forwarded-For`、
  `X-Forwarded-Proto` 和 `X-Request-Id`；Upgrade 明确返回 501；
- `RequestContext` 增加 Session-owned 流式写入回调。ProxySession 上游 Header 一到即写下游
  chunked Header；每个下游 `async_write` 完成后才继续读上游，避免无界待写队列；
- 已开始下游响应后若上游失败，只关闭下游而不追加错误 HTTP 响应；Session 关闭或停机时可取消
  已登记的 resolver/socket 事务；
- 新增纯标准库 `tools/mock_upstream.py`，支持延迟、chunked、分片 Header、提前断开和停住 Body，
  便于重现代理边界；开发版本提升为 `0.6.0`。

验证结果：

- Debug、ASan/UBSan、TSan 完整 CTest 均为 74/74 通过；
- `-DPULSEGATE_WARNINGS_AS_ERRORS=ON` 构建通过；Release 构建成功且 `--version` 输出 `0.6.0`；
- 集成测试覆盖分片 chunked 上游、POST 分片 Body、两个上游 Round Robin、Header 清理和 request ID；
- Release 手工启动 Python mock 与网关后，`/proxy/demo` 返回 200、chunked 与 `X-Request-Id`；
  mock 同时记录相同 request ID。

重要决策：

- 连接池和主动健康检查明确保留到第 13 章；第 12 章每次事务关闭上游连接，避免在没有独占 Lease
  和跨 executor 归还语义前实现不安全的伪连接池；
- 当前请求 Body 由下游 Parser 完整且有上限地读取后转发，因此只将**响应方向**描述为流式；不把它
  描述成双向请求流式；
- 下游流式输出只能经 `HttpSession` 提供的回调执行，ProxySession 不直接持有或并发操作下游 Socket。

遗留事项：

- 下一章实现主动/被动健康检查、strand-owned 上游连接池和独占 Lease；
- 请求方向 chunked 与边读边写、重试策略、WebSocket/TLS 代理仍不在当前版本范围内。

## 2026-08-10

### 第十三章：主动健康检查与上游连接池

完成内容：

- 新增不可变 `HealthSnapshot` 与阈值状态机；代理热路径无锁读取健康快照，健康端点不可用时返回 503；
- 新增 `HealthChecker`：独立 Probe socket、可取消 interval/timeout、并发上限与端点轮转；停机时取消
  已安排的 Probe；
- 新增 pool strand、异步 `asyncAcquire()`、有界 waiter 队列、acquire timeout、空闲/寿命/复用次数
  限制、空闲清理 timer 及停机取消；
- 新增 move-only Lease；遗漏显式归还时只异步 discard，成功事务只有在 parser 确认连接可复用后才归还；
- 将 `UpstreamConnection` 的 resolver/socket 放入连接专属 strand；代理按端点懒创建池并在 SIGINT/SIGTERM
  时停止所有池；
- mock upstream 新增可选 `--keep-alive`，可从日志中的相同 `connection=N` 观察复用。

验证结果：

- Debug 构建通过；完整 CTest 在本机回环环境中为 **87/87 通过**，其中包括新增长连接复用场景；
- `-DPULSEGATE_WARNINGS_AS_ERRORS=ON` 的无测试构建通过；
- ASan/UBSan 与 TSan 完整 CTest 均为 **87/87 通过**；Release 构建成功且 `--version` 输出 `0.7.0`；
- 普通沙箱会禁止测试进程创建 socket；在允许本机回环 TCP 的测试环境中完成了完整 CTest。

重要决策：

- 连接的 socket 不跟随下游 Session strand 复用；它由 `UpstreamConnection` 的独立 strand 管理，Lease
  保证同一时刻只允许一个事务操作该连接；
- 对任何协议、超时、取消、残留字节或 `Connection: close` 情况采取 discard，而不是尝试复用。

遗留事项：

- 在允许网络 socket 的 CI/本机补充两次真实代理请求复用同一连接、Probe 恢复以及多 worker/TSan 验证；
- 代理 CLI 暂未暴露健康阈值和 pool 上限配置，当前采用代码默认值。

## 2026-08-10

### 第十四章：Token Bucket 限流

完成内容：

- 新增线程安全的 `TokenBucket`：初始 burst 满额、按 `steady_clock` 补充令牌，并提供
  `Retry-After` 所需的等待时间；构造及调用参数均拒绝非有限或非正值；
- 新增 `RateLimiter`：既可作为一个全局 Bucket，也可按 TCP 对端 IP 建立分片 Bucket 表；
  表在分片锁内 TTL 清理，并用原子 key 计数严格限制总 key 数，达到容量上限时快速拒绝；
- `Router` 在匹配 handler 前执行全局和可选路由级限流。拒绝返回 429、`Retry-After` 和
  `X-Request-Id`，因此不会创建上游代理连接；允许的限流路由仍保持原有 handler 异常到 500 的边界；
- 主程序新增 `--rate-limit/--rate-burst/--rate-per-client` 全局参数，以及
  `--proxy-rate-limit/--proxy-rate-burst/--proxy-rate-per-client` 的 `/proxy/*` 路由参数；
- `/metrics` 增加 `pulsegate_rate_limit_requests_total`，只用 global/route 与
  allowed/rejected/key_capacity 聚合，未将客户端 IP 作为 Prometheus 标签；
- 开发版本升级到 `0.8.0`。

验证结果：

- Token Bucket 的 burst、补充、并发不超发与非法参数均由 FakeClock/单元测试覆盖；
- 覆盖客户端 Bucket TTL、key 容量上限、统计聚合、全局/路由限流、允许路由的异常映射；
- 回环代理集成测试确认第二个被拒绝的 `/proxy/*` 请求不会到达 Mock Upstream；
- Debug、ASan/UBSan、TSan 完整 CTest 均为 **98/98 通过**；`-Werror` 与 Release 构建通过，
  Release `--version` 输出 `0.8.0`；
- CLI 帮助包含全局和 `/proxy/*` 限流选项；缺少配对的 burst 参数会以非零退出并给出明确错误。

重要决策：

- 客户端维度以 socket peer IP 为 key；除非日后引入受信任代理链配置，否则不读取
  `X-Forwarded-For`；
- 最大 key 数耗尽是显式的过载拒绝（429），而不是无界分配或静默退化为全局 Bucket；
- 指标只记录聚合结果，不引入会导致高基数的客户端标签。

遗留事项：

- 下一小节实现分片 LRU + TTL 响应缓存；缓存命中与限流顺序需要在设计时明确；
- 当前 CLI 暴露速率、burst 和客户端维度；分片数、TTL、最大 key 数保留为 API 安全默认值，
  后续如需运行时配置应同时补充上限校验和运维文档。

## 2026-08-11

### 第十五章：分片 LRU + TTL 响应缓存

完成内容：

- 新增 `ResponseCache`：每个 shard 在 mutex 下维护 LRU list 与索引；总字节预算按 shard 精确分配，
  插入会按字节数淘汰最久未使用条目；读取命中提升到 LRU 前端，过期条目惰性删除；
- Cache key 包含固定 plaintext `http` scheme、规范化 Host、路径、Query 与配置的 Vary 请求头；
  `Authorization`、`Cookie` 请求以及 `Set-Cookie`、`Cache-Control: private/no-store`、`Vary: *`
  响应拒绝缓存；仅完整 GET 200 响应填充，HEAD 可读取 GET 缓存但不会覆盖它；
- `Router::add()` 新增可选缓存配置。缓存 hit 在 handler 前短路并返回 `X-Cache: HIT`；成功填充为
  `MISS`，策略或容量跳过为 `BYPASS`，所有路径保留独立 `X-Request-Id`；
- `/proxy/*` 提供 `--proxy-cache-ttl-ms`、`--proxy-cache-max-bytes`、
  `--proxy-cache-entry-max-bytes`、`--proxy-cache-shards`。缓存 miss 令代理走完整响应路径；未启用
  缓存的代理仍保持原有 chunked 流式下游行为；
- `/metrics` 增加无高基数标签的 `pulsegate_response_cache_operations_total`，记录 hit、miss、
  store、eviction 与 expired；开发版本升级至 `0.8.1`。

验证结果：

- 单元测试覆盖 LRU 命中提升、按字节淘汰、TTL、相同 key 更新、无效容量配置、并发 get/put、
  敏感请求/响应、Cache key Host 规范化及超大 Body；
- Router 测试覆盖显式缓存路由、GET 填充、HEAD 命中但不发送 Body、敏感请求 bypass 与指标；
- 回环代理集成测试确认 GET 第一次 MISS、第二次 HIT，Mock Upstream 仅收到一次请求；
- Debug、ASan/UBSan、TSan 完整 CTest 均为 **106/106 通过**；`-Werror` 与 Release 构建通过，
  Release `--version` 输出 `0.8.1`；
- ASan 曾发现 Cache key 的 Query 分支把临时 `std::string` 转成悬空 `string_view`；已改为从
  `request.target` 的稳定 `string_view` 切片，并在修复后重新完成完整 ASan 回归。

重要决策：

- 以总字节数而非对象数量限制缓存；为避免配置承诺的对象无法写入，单对象上限不得超过任一 shard
  的预算，CLI 默认据此选择 shard 数；
- 当前缓存置于 Router handler 边界，因此只有路线明确配置后才生效；限流仍在缓存读取之前执行，避免
  缓存命中绕开过载保护；
- 代理缓存需要先得到完整、可验证的上游响应，故缓存 miss 不使用流式转发；这是与低首字节延迟之间
  明确记录的取舍。

遗留事项：

- 后续可实现后台分批过期清理与 SingleFlight，减少热点 miss 同时穿透上游；
- 缓存暂不解析上游的 `max-age`、ETag、条件请求或 revalidation；TTL 是显式本地配置。

## 2026-08-11

### 第十六章：熔断、背压与过载保护

完成内容：

- 新增每端点、线程安全的 `CircuitBreaker` 状态机：Closed 在连续失败阈值后进入 Open；冷却后转为
  Half-Open，并用独立 probe budget 防止恢复瞬间的并发洪峰；成功关闭并清零，失败立即重新打开；
- 代理选择端点时同时检查健康快照与熔断器许可。Open 请求快速返回 `503` 和向上取整的
  `Retry-After`，不占用上游连接池，也不访问故障端点；
- `ProxyLimits` 新增全局 `max_in_flight_requests`（默认 1024）和可配置的过载重试时间。CAS 准入失败或
  连接池 waiter 满/超时均明确返回 `503 Service Unavailable` 和 `Retry-After`；
- 将失败分类固定下来：连接、解析、超时及按策略计入的 5xx 同时写入健康状态和熔断器；客户端取消、
  停机、Upgrade 拒绝及本地准入拒绝只释放 Half-Open probe，不把本地问题误判成上游故障；
- 沿用流式代理“一次下游写完成后才读下一块上游数据”的反压边界，以及 `Buffer`、Session、Pool 和
  waiter 的绝对上限；开发版本提升至 `0.8.2`。

验证结果：

- 新增熔断器单元测试，覆盖阈值打开、冷却前拒绝、Half-Open 成功/失败、16 线程并发下仅一个 probe
  获准，以及中性结果释放 probe；
- 新增回环集成用例：连续三个 keep-alive 500 后第四个请求返回带 `Retry-After: 5` 的 503，且 Mock
  Upstream 未收到第四个请求；另覆盖关闭 5xx 故障计数后的持续 500，以及活动事务上限导致的池前 503；
- Debug、ASan/UBSan、TSan 的完整回环 CTest 均为 **114/114 通过**；`clang-format --dry-run --Werror`、
  `git diff --check`、独立 `-Werror` 构建和 Release 构建均通过，Release `--version` 输出 `0.8.2`。

重要决策：

- 健康检查和熔断器职责不同：前者是主动/被动端点可用性快照，后者是业务失败时的快速短路；二者都
  拒绝时优先向调用方返回熔断状态，避免把已知故障伪装为“没有健康节点”；
- 过载使用 503 而不是 429：429 表示调用方触发配额，503 表示网关或上游资源暂时耗尽；两者都带
  `Retry-After` 以便客户端采取退避；
- 本章不引入无限制后台队列或“每个分片都 post”的伪公平性。现有每次 async write 的自然让出和
  单连接有界 Buffer 是当前响应流的反压基础；更细的预算和输出高低水位将在配置/压测阶段按数据调优。

遗留事项：

- CLI 尚未公开熔断阈值、冷却、in-flight 或 Pool 上限；第 17 章配置加载器会一起提供类型校验；
- 指标模块将在第 18 章集中接入，届时补充 breaker state、admission rejection 和 backpressure pause
  的聚合指标，避免在当前阶段形成临时指标接口。

## 2026-08-11

### 第十七章：YAML 配置加载与安全重载边界

完成内容：

- 在 `cmake/Dependencies.cmake` 集中以固定 commit 引入 `yaml-cpp 0.8.0`，关闭其工具、测试和安装目标；
  同时显式声明与 CMake 4 的兼容策略下限，避免第三方项目的旧策略警告影响主工程；
- 新增强类型 `Config`、`ServerConfig`、`UpstreamConfig`、`RouteConfig` 与 `LoggingConfig`，
  `ConfigLoader` 将 YAML 文件的语法读取和业务校验分离，并收集带字段路径的全部错误后一次报告；
- 覆盖监听地址与端口、IO 线程、超时、连接/Header/Body/输出水位、路由限流与缓存、上游端点及连接池、
  日志级别与格式；提供 `config/pulsegate.example.yaml` 和 `pulsegate --config FILE` 启动入口；
- 验证跨字段约束：端口范围、正数时间/容量、Body 安全上限、输出低水位小于高水位、上游名称与端点唯一、
  路由引用存在的上游、缓存分片预算以及字段名中的毫秒单位；
- 新增 `ConfigManager`：SIGHUP 经 manager strand 去重，将解析和完整校验放到专用 worker，随后回到
  strand 原子发布不可变 `shared_ptr<const ConfigSnapshot>`；失败始终保留旧快照；
- 对会改变已运行 Server/Router/Proxy 行为的监听、线程、限制、路由和上游配置明确返回“需要重启”。
  当前只有日志元数据可安全发布，避免出现“配置已重载但实际没有生效”的半应用状态；开发版本升级至
  `0.9.0`。

验证结果：

- 新增配置单元测试，覆盖合法 YAML 与默认值、聚合错误路径、动态日志配置发布，以及静态端口变更拒绝后
  仍保留旧快照；
- Debug、ASan/UBSan、TSan 的完整回环 CTest 均为 **117/117 通过**；
  `clang-format --dry-run --Werror`、`git diff --check`、独立 `-Werror` 构建和 Release 构建均通过，
  Release `--version` 输出 `0.9.0`。
- 使用临时配置将监听端口改为 `127.0.0.1:18080`，验证 `pulsegate --config FILE` 可实际启动、
  `/livez` 返回 `200 alive`，并能通过 `SIGINT` 正常退出。

重要决策：

- 配置加载不把字段缺失、类型错误和跨字段错误混为一谈；先构造带默认值的候选对象，再统一验证，便于
  用户一次修复所有可定位问题；
- 重载以完整候选快照为提交单元。配置文件读取失败、YAML 语法错误或校验失败都不会修改正在服务的配置；
- 本阶段不声称热更新 listen socket、线程数、路由或上游池。它们需要组件级替换和优雅 drain，后续实现
  前一律要求重启，保证运行语义诚实且可预测。

遗留事项：

- 第 18 章接入指标与结构化日志时，将消费日志配置快照，并记录重载成功、失败和“需要重启”的次数；
- 未来可为路由和上游引入不可变运行时对象与 drain 协议，届时再逐步扩大允许热更新的配置范围。

## 2026-08-12

### 第十八章：日志与可观测性

完成内容：

- 新增项目自有的 `Observability` 边界，避免 HTTP、Router 和 Proxy 直接依赖某个第三方日志库；
- `AsyncLogger` 在专用 `std::jthread` 消费有界队列，输出 JSON 或 text access log。队满时丢弃新日志，
  不让文件/终端写入阻塞 `io_context`；每条记录只含 request ID、方法、去掉 Query 的路径、状态、
  steady-clock 耗时、字节数、上游和缓存状态，不含 Cookie、Authorization 或 Body；
- 接入 YAML `logging.level` 和 `logging.format`：配置启动时生效，SIGHUP 成功发布 logging 快照后更新
  logger；
- 新增低基数 `MetricsRegistry` 和 Prometheus text `/metrics`：HTTP 请求计数与直方图、连接接受/拒绝/
  活跃数、上游请求与连接耗时、缓存、限流、熔断状态、活跃协程、Pool waiter、输出 Buffer、日志丢弃；
- Server、Router 与 Proxy 在请求完成、限流/缓存决策、连接准入、代理结果和熔断状态变化处写入统一指标；
  指标不使用 request ID、IP、用户 ID 或完整 URL；开发版本升级至 `0.9.1`。

验证结果：

- 单元测试覆盖异步日志队列的有界丢弃，以及 Prometheus 计数器、直方图和 gauge 的标签与渲染；
- 集成测试验证 `/metrics` 的新 HTTP、连接和日志丢弃指标；Debug、ASan/UBSan、TSan 完整回环 CTest 均为
  **119/119 通过**；独立 `-Werror` 与 Release 构建、`clang-format --dry-run --Werror`、
  `git diff --check` 均通过，Release `--version` 输出 `0.9.1`。

重要决策：

- 先使用项目接口而不是引入 spdlog：当前需求只有 access log，依赖更小，也便于下一步替换 sink；
- 日志丢弃采用“丢弃新条目”的明确策略，并将其本身暴露为指标；吞吐优先时绝不把磁盘 I/O 回压到网络
  worker；
- Prometheus 使用 route name、状态类别等有限标签；直方图桶固定，避免将请求 ID 或原始 Query 变成
  高基数时间序列。

遗留事项：

- 活跃协程当前统计 HTTP Session 顶层协程；更细粒度的 resolver、proxy 和 timer coroutine 统计可在
  profile 驱动后再补充。

## 2026-08-12

### 第十九章：优雅停机

完成内容：

- `HttpServer::beginDrain(grace, callback)` 成为幂等的关闭入口：先停止 Listener、将 Registry 标为
  draining，再等待现有 Session；新连接不再被接受，正在读取下一条请求的 Keep-Alive 连接立即关闭；
- 默认 Router 的 `/readyz` 读取 Server drain 状态，进入 draining 后返回 `503 Service Unavailable`；
- 使用 `steady_timer` 实现 grace deadline。到期后通过 Registry 取消所有残留 Session，并等待 Registry
  实际清空后才调用完成回调；Session 关闭会取消当前 `ProxySession`，进一步释放 Resolver、连接池 waiter
  和上游 socket；
- CLI 的 SIGINT/SIGTERM 使用 15 秒 grace，YAML 启动路径使用 `server.graceful_shutdown_ms`。SIGHUP 仍会
  重挂 `signal_set` 等待配置重载；正常停止只在 drain 清理完成后释放 Runtime work guard；
- 开发版本升级至 `0.9.2`，README 增加优雅停机语义和本机 SIGTERM 验收命令。

验证结果：

- 集成测试覆盖：无活跃 Session 时停止 accept、半包读取连接关闭、慢代理在 deadline 内成功完成，以及慢代理
  超过 deadline 后被强制取消；
- Debug、ASan/UBSan、TSan 完整 CTest 均为 **123/123 通过**；Release 构建的 `--version` 输出 `0.9.2`；
  独立 `-Werror` 构建、`clang-format --dry-run --Werror`、`git diff --check` 均通过；
- 进程级验收启动 `127.0.0.1:18080`，确认 `/readyz` 初始返回 `ready`，再发送 `SIGTERM`，进程以 **0** 退出。

重要决策：

- deadline 到期不立即释放 work guard。先把取消投递给各 Session executor，再等 Registry 为空，防止
  `io_context` 过早返回而遗漏取消处理、连接清理或 access log；
- `io_context.stop()` 不作为正常优雅关闭路径。它仍只保留给无法自然收敛时的外部强制兜底。

## 2026-08-12

### 第二十、二十一章：测试体系与质量工具

完成内容：

- 盘点并保留既有的 123 项确定性单元/回环集成测试；其范围覆盖解析、超时、连接生命周期、多 worker strand、
  代理、连接池、限流、缓存、熔断、配置、观测与优雅停机；
- 新增 Clang 专用 `fuzz` CMake preset、HTTP Parser libFuzzer 目标、HTTP 词典与语料目录。fuzzer 同时以
  单缓冲和确定性分块输入驱动 Parser，并检查 Complete 状态与可移动的请求不变量；
- 新增可复用 Clang toolchain 文件：自动使用当前 `g++` 对应的 libstdc++ 安装目录，避免主机存在不完整新版
  GCC runtime 时 Clang 链接到错误目录；
- 新增 `clang-tidy` Clang 构建预设；`.clang-tidy` 启用 concurrency 分析，并明确排除项目有意采用的
  `#pragma once` 和会改变公共 ABI 的 enum-size 建议；静态分析先记录非阻断基线，尚不把历史告警设为 error；
- 新增 Debug-only `asio-tracking` 预设和 `PULSEGATE_ENABLE_ASIO_HANDLER_TRACKING` 开关；Release 非法启用时
  CMake 明确拒绝，防止调试跟踪污染性能测试；开发版本升级至 `0.9.3`，README 记录运行命令与 crash 回归流程。

验证结果：

- `clang++-21` 通过新 toolchain 成功配置并构建完整项目，`clang-tidy` 能消费其 compilation database；
  HTTP Parser 基线报告正常产生，当前不作为零告警门禁；
- 安装 `libclang-rt-21-dev` 后，修正运行时探测以匹配 Ubuntu 的架构后缀文件名
  `libclang_rt.fuzzer-x86_64.a`；`fuzz` preset 成功完成配置和构建；
- HTTP Parser 在临时语料副本上完成 **10,000 次** libFuzzer smoke run，覆盖增长至 144 个 feature，
  未发现 ASan/UBSan 错误。受控终端会被 LeakSanitizer 识别为 ptrace，因此该次 smoke 仅以
  `ASAN_OPTIONS=detect_leaks=0` 关闭 leak 检测；正常本机/CI 运行仍保持默认 leak 检测；
- 文档将 smoke 命令改为临时语料和 artifact 目录，避免 libFuzzer 的覆盖样本污染受版本控制的 seed corpus。

重要决策：

- 模糊测试使用单独 Clang 构建树，绝不与普通 ASan/TSan 混用；每个 future crash 都必须最小化并落入 corpus，
  再附一个确定性 GoogleTest 回归；
- clang-tidy 先做真实、可重复的报告而不是一开始强制全项目零告警。稳定检查会在 CI 章节按基线逐步收紧。

## 2026-08-12

### 第二十二章：性能测试与逐步优化

完成内容：

- 新增 `tools/benchmark.sh`：自动生成 logging=critical 的临时配置，启动 Release PulseGate、确认
  `/healthz`、预热、执行多轮 `wrk`，并保存原始输出、环境元数据和按秒 CPU/RSS 采样；
- 新增 `benchmarks/` 说明及被忽略的原始结果目录，防止机器相关结果污染仓库，同时将方法和结论固化到
  `docs/benchmarks/v0.9.3-healthz-baseline.md`；
- 在 WSL2 / Ryzen 7 8845H（16 逻辑 CPU）运行 1/2/4/8 worker、100 connections、2 wrk threads、
  5 秒预热、3×15 秒的完整 Release 回环矩阵；开发版本保持 `0.9.3`。

验证结果：

- 各组 3 次 wrk 输出均未出现 socket error 或 non-2xx；4 worker 的中位值为 **309,868.85 RPS**、
  **463 µs P99**，优于 1、2、8 worker；8 worker 降至 255,336.30 RPS，证明更多 worker 不等于更高吞吐；
- Release 构建成功；脚本经 `bash -n` 和一次短时 end-to-end smoke 验证；完整矩阵的原始数据保留在本机
  `/tmp`，版本化报告保留全部参数、环境和汇总结果；
- 当前 WSL2 环境没有匹配 `perf` 的可执行程序，因此未伪造 profile 结论，也未在没有热点证据时修改热路径。

重要决策：

- 基准使用中位数及三次原始值，不挑选最高 RPS；服务端和负载端同机的限制被明确写入报告；
- 默认 worker 数不因单个 `/healthz` 实验改动。真实 proxy、cache、日志和容器负载应以独立 profile 和
  before/after 矩阵决定；本章优先建立可信的证据链，而不是无依据的微优化。

## 2026-08-14

### 第二十三章：Docker 容器化

完成内容：

- 新增多阶段 `Dockerfile`：build stage 固定 Ubuntu 24.04、安装 CMake/Boost/编译工具并使用 Release
  preset 构建；runtime stage 只复制 `pulsegate` 二进制和配置，以 UID/GID `10001` 的无 shell 非 root
  用户运行，使用 exec-form entrypoint 和 `SIGTERM` stop signal；
- 新增 `.dockerignore`，排除 Git 元数据、构建结果、测试、文档、日志、本地配置和 benchmark 原始结果，
  缩小构建上下文且不把机密或机器相关文件带入镜像；
- 新增可直接运行的 `compose.yaml` 和 Docker DNS 配置文件。两个上游都由同一 Dockerfile 的
  `mock-upstream` target 构建，因此没有未替换的测试镜像或命令占位符；gateway 以只读配置挂载、只读
  根文件系统、`tmpfs`、`no-new-privileges`、init 和 20 秒优雅停机窗口运行；
- build stage 支持由调用方传入标准 `HTTP_PROXY`、`HTTPS_PROXY` 与 `NO_PROXY` 参数，以便企业/校园网络
  下载 Ubuntu 依赖和固定 yaml-cpp 提交；具体代理地址不进入 Dockerfile 或最终镜像；
- mock upstream 正确忽略 TCP-only health check 在未发送 HTTP 请求时的连接重置，避免把正常探测显示为
  Python traceback；
- README 记录构建、启动、轮询、日志、优雅停止、清理、非 root/只读运行和基础镜像 digest 固定策略。

验证结果：

- Release 二进制 `ldd` 仅显示 glibc、libstdc++、libm 和 libgcc；yaml-cpp 与 Boost 使用当前静态/头文件
  链接策略，runtime stage 无需复制额外的项目依赖动态库；
- Docker Engine 29.1.3 / Compose 2.40.3 实测完成多阶段构建；Ubuntu 24.04 基础镜像解析为
  `sha256:561618…f5c538ea`，最终 gateway 镜像为 **134 MB**；
- `docker compose up` 后 `/livez` 返回 `alive`，两个连续 `/api/*` 请求分别到达 `upstream-a` 和
  `upstream-b`；停掉 `upstream-a` 并等待健康检查摘除后，`curl --fail /api/after-stop` 仍成功，恢复后再次
  请求成功；
- runtime 容器实际为 UID/GID `10001:10001`、`ReadonlyRootfs=true`、`STOPSIGNAL=SIGTERM`；在只读
  文件系统中检查确认没有 gcc、CMake 或 `/src` 源码目录；`docker compose stop gateway` 的 exit code 为 0。

重要决策：

- mock upstream 作为 Dockerfile 明确 target 而非浮动的第三方演示镜像，确保 Compose 演示行为和本地学习
  工具一致；
- Ubuntu `24.04` 是固定发行版而非 `latest`，但不是不可变 digest；发布流水线应按扫描/SBOM 的更新节奏把
  它收紧为具体 digest。

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
