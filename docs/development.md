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
