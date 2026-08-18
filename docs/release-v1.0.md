# v1.0 发布准备

本文件定义 `v1.0.0` 发布前的验收范围、可复现命令和回滚路径。当前候选为
`1.0.0-rc.1`；它可以用于测试和演示，但在本清单全部完成前不能创建 `v1.0.0` tag 或 GitHub Release。

## 发布边界

- 二进制和 `/api/version`：`1.0.0-rc.1`；正式 tag 后改为 `1.0.0`；
- CMake 包版本：`1.0.0`；
- 基础镜像：Ubuntu `24.04`。正式镜像发布时必须记录实际 image digest；
- 支持范围与明确限制见 [README](../README.md) 和 [CHANGELOG](../CHANGELOG.md)。

## 候选验收

以下命令必须从干净的候选 commit 执行。构建目录与性能原始数据由 `.gitignore` 排除。

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug

cmake --preset release
cmake --build --preset release
./build/release/app/pulsegate --version

cmake --preset asan
cmake --build --preset asan
ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1:halt_on_error=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
ctest --preset asan

cmake --preset tsan
cmake --build --preset tsan
TSAN_OPTIONS=halt_on_error=1 ctest --preset tsan --output-on-failure
```

远程合并前，PR 必须全部通过：GCC Debug、Clang Debug、ASan/UBSan、clang-format、clang-tidy 和
Docker Compose smoke test。TSan 是每周定时或手动任务；正式发布候选需保留其最近一次成功记录。

## 性能与部署验收

```bash
tools/benchmark.sh --workers 1,2,4,8 --trials 3 \
  --connections 100 --load-threads 2 --warmup 5s --duration 15s

docker compose config
docker compose up --build -d
curl --noproxy '*' --fail http://127.0.0.1:8080/livez
curl --noproxy '*' --fail http://127.0.0.1:8080/api/release-check
docker compose stop upstream-a
sleep 8
curl --noproxy '*' --fail http://127.0.0.1:8080/api/failover-check
docker compose down --volumes
```

将基准的候选 commit、硬件、内核、编译器、负载参数、三次原始 RPS/P99 和局限写入新的版本化报告；
不要提交 `benchmarks/results/` 的机器相关原始文件。对当前已有数据，可参考
[v1.0.0-rc.1 `/healthz` 候选基线](benchmarks/v1.0.0-rc.1-healthz-baseline.md) 和
[v0.9.3 `/healthz` 基线](benchmarks/v0.9.3-healthz-baseline.md)。

## 正式发布步骤

1. 确认候选 PR 已合并，且 `main` 的最新 CI 全绿；
2. 将 `kVersion` 从 `1.0.0-rc.1` 改为 `1.0.0`，更新 changelog 的发布日期与候选验收记录；
3. 从该 commit 创建 annotated tag `v1.0.0`，推送 tag；
4. 创建 GitHub Release，正文使用 `CHANGELOG.md` 的 `1.0.0` 条目，并附上 commit SHA、Docker image tag/digest、
   已知限制和回滚指引；
5. 用独立目录或主机拉取 tag，重复 Release 构建、`--version`、Compose smoke 和 `/api/version` 验证；
6. 将 Release URL、tag SHA、镜像 digest 和验收结果补充到工作日志。

## 回滚

- 若候选 CI 或部署验收失败：不创建 tag；修复在新的 `fix/` 分支和 PR 中完成；
- 若已发布镜像异常：停止推广该 digest，重新部署上一稳定版本 `v0.8.2` 对应镜像/二进制，并保留日志与指标；
- 若 `v1.0.0` tag 已发布：不要移动或重写 tag；通过 `v1.0.1` 发布修复，并在 Release Notes 明确升级与回退说明。
