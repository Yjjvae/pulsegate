# Contributing to PulseGate

## 开发流程

1. 从最新 `main` 创建短生命周期分支；
2. 在本地完成实现、测试和文档；
3. 检查差异中没有密钥、构建产物和无关修改；
4. 使用语义化提交信息；
5. 推送分支并创建 Pull Request；
6. 等待构建与测试检查通过后再合并。

```bash
git switch main
git pull --ff-only
git switch -c feat/<short-topic>

cmake --preset debug
cmake --build --preset debug
ctest --preset debug

git status --short
git diff
git add <明确的文件>
git commit -m "feat(scope): describe one logical change"
git push -u origin feat/<short-topic>
```

## 分支和提交

分支使用 `feat/`、`fix/`、`docs/`、`test/`、`perf/` 或 `build/` 前缀。

提交格式：

```text
<type>(<optional-scope>): <imperative summary>
```

一个提交只表达一个逻辑变化。推荐类型包括 `feat`、`fix`、`test`、`perf`、
`refactor`、`docs`、`build` 和 `ci`。

## Pull Request

Pull Request 必须写清：

- 变更动机；
- 已完成和明确未完成的内容；
- 可复现的测试命令；
- 兼容性或性能影响；
- 风险和回滚方式。

禁止向仓库提交 Token、SSH 私钥、密码、`.env` 或真实生产配置。
