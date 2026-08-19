# PulseGate 面试演示脚本

这份脚本用于 8–10 分钟的项目演示。重点是展示：异步网关、反向代理、上游故障隔离、可观测性、容器化和工程化交付，而不是逐个点击所有功能。

> 面试前务必完整运行一次。首次 Docker 构建会下载基础镜像并编译 C++，不要留到面试现场。

## 0. 面试前准备

在项目根目录执行：

```bash
cd /path/to/pulsegate
git status --short
git log -1 --oneline
docker compose down --volumes
docker compose up --build -d
docker compose ps
```

预期：

- `git status --short` 没有输出，表示工作区干净；
- `gateway` 为 `running`；
- `upstream-a`、`upstream-b` 为 `running (healthy)`。

若已提前构建镜像，现场只需执行：

```bash
docker compose up -d
```

## 1. 部署说明

```bash
docker compose config -q && echo "Compose 配置有效"
```

预期：

```text
Compose 配置有效
```

讲解：

> PulseGate 基于 C++20、Boost.Asio 和 Boost.Beast 实现。Compose 启动一个网关和两个 mock upstream；网关对外暴露 8080，通过 Docker DNS 将 `/api/` 流量代理到上游集群。

## 2. 健康检查与版本

```bash
curl --noproxy '*' --include http://127.0.0.1:8080/livez
curl --noproxy '*' --include http://127.0.0.1:8080/readyz
curl --noproxy '*' --include http://127.0.0.1:8080/api/version
```

预期重点：

```text
HTTP/1.1 200 OK

alive
```

```text
HTTP/1.1 200 OK

ready
```

```text
HTTP/1.1 200 OK

1.0.0
```

讲解：

- `/livez` 表示进程仍存活；
- `/readyz` 表示可以接收流量；网关进入 draining 后会返回 `503`；
- `/api/version` 用于确认演示的是正式 `v1.0.0`。

## 3. 反向代理与双上游

```bash
for id in 1 2 3 4; do
  curl --noproxy '*' -sS \
    -H "X-Request-Id: interview-demo-$id" \
    http://127.0.0.1:8080/api/demo
done

docker compose logs --tail=30 upstream-a upstream-b
```

预期响应：

```text
hello from mock upstream
```

两个 mock upstream 默认响应相同；用日志确认请求被分发到两个节点。日志中应能找到 `request_id=interview-demo-*`。

讲解：

> `/api/` 命中 YAML 路由后进入反向代理。网关维护上游端点与连接池，并将请求 ID 传递给上游，便于追踪请求链路。

## 4. 故障摘除与故障转移

停止一个上游：

```bash
docker compose stop upstream-a
docker compose ps
sleep 8

curl --noproxy '*' --fail --show-error \
  -H "X-Request-Id: failover-demo" \
  http://127.0.0.1:8080/api/failover-demo

docker compose logs --tail=20 upstream-b
```

预期：

- `upstream-a` 已退出，`gateway` 和 `upstream-b` 仍运行；
- 请求仍返回 `hello from mock upstream`；
- `upstream-b` 日志出现 `request_id=failover-demo`。

默认健康检查每 2 秒一次，连续 3 次失败后摘除节点，因此等待 8 秒。恢复环境：

```bash
docker compose start upstream-a
sleep 5
docker compose ps
```

讲解：

> 健康检查使用独立探测连接维护上游可用性。连续失败的节点被摘除，后续流量只进入健康节点，避免持续向已知故障节点发请求。

## 5. Prometheus 指标

```bash
curl --noproxy '*' -sS http://127.0.0.1:8080/metrics \
  | rg '^(pulsegate_ready|pulsegate_http_requests_total|pulsegate_upstream_requests_total|pulsegate_accepted_connections_total|pulsegate_circuit_state)'
```

预期会出现类似：

```text
pulsegate_ready 1
pulsegate_http_requests_total{...} 8
pulsegate_upstream_requests_total{...} 5
pulsegate_accepted_connections_total 10
```

讲解：

> 指标采用 Prometheus text 格式。标签保持低基数：不会把用户 IP、完整 URL 或请求 ID 写入指标标签，避免监控系统被高基数数据拖垮。

默认 Compose 演示聚焦代理、健康检查和指标。限流、缓存、熔断由独立模块和测试覆盖，不应在未开启对应配置时宣称正在展示它们。

## 6. 容器安全与优雅停止

```bash
gateway_id=$(docker compose ps -q gateway)
docker inspect "$gateway_id" \
  --format 'user={{.Config.User}} readonly_rootfs={{.HostConfig.ReadonlyRootfs}}'
```

预期：

```text
user=10001:10001 readonly_rootfs=true
```

讲解：

> Dockerfile 使用多阶段构建；运行镜像不包含编译器、CMake 或源码。容器用非 root 用户运行，根文件系统只读，减少运行时攻击面。

最后再演示优雅停止：

```bash
docker compose stop gateway
docker compose ps

docker compose up -d gateway
curl --noproxy '*' --fail http://127.0.0.1:8080/livez
```

预期：gateway 正常退出并可重新启动，最后返回：

```text
alive
```

## 7. 工程化证据

打开以下页面即可：

- [v1.0.0 Release](https://github.com/Yjjvae/pulsegate/releases/tag/v1.0.0)；
- 发布 PR #35；
- GitHub Actions：GCC、Clang、ASan/UBSan、clang-format、clang-tidy、Docker Compose smoke test。

收尾话术：

> 这个项目不仅实现了 HTTP 服务，还把异步网络、上游故障隔离、可观测性、容器部署、质量门禁和版本发布串成了可复现的工程闭环。

## 常见突发问题

| 现象 | 处理方式 |
| --- | --- |
| Docker 守护进程未启动 | 运行 `docker info`，确认输出包含 Server 信息。 |
| 当前用户没有 Docker 权限 | 重新登录，使 docker 用户组生效；临时用 `sudo docker` 仅用于排查。 |
| 8080 端口被占用 | `ss -ltnp | rg ':8080'`；停止旧进程，或把 Compose 的端口映射改为 `18080:8080`。 |
| curl 访问本地地址异常 | 使用 `--noproxy '*'`，避免系统代理拦截回环请求。 |
| 停掉上游后出现一次 502 | 健康检查尚未完成摘除；等待 8 秒后重试。 |
| Docker build 下载依赖失败 | 仅在必要时传入 `HTTP_PROXY`、`HTTPS_PROXY`、`NO_PROXY`；不要把具体代理地址提交到仓库。 |
| 演示环境状态混乱 | `docker compose down --volumes` 后重新执行 `docker compose up -d`。 |

## 演示结束

```bash
docker compose down --volumes
```

该命令只删除本项目的容器和网络，不会删除源码或本地 Docker 镜像。
