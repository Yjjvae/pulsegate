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

- 工作区存在未跟踪的 `.playwright-mcp/` 运行产物，暂不删除或提交；
- 后续确认该目录仅包含可再生成产物后，再决定是否加入 `.gitignore`。

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
