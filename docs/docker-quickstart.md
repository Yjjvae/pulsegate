# PulseGate Docker 容器化：从零到跑通

本教程不要求先在宿主机编译 C++。在项目根目录执行 Docker 命令即可：Docker 会在构建容器中安装 Boost、CMake 等依赖并编译，运行容器只保留网关二进制和配置。

## 1. 一次性准备

确认 Docker Engine 和 Compose 可用：

```bash
docker --version
docker compose version
docker info
```

最后一条能显示 `Server` 信息才表示 Docker 守护进程已启动。若提示没有权限访问 `/var/run/docker.sock`，先用 `sudo docker info` 确认服务正常；之后将当前用户加入 `docker` 用户组并重新登录：

```bash
sudo usermod -aG docker "$USER"
```

> 不要每次都用 `sudo docker`。确认组权限生效后，后续命令都使用普通用户执行。

进入项目根目录并检查 Compose 文件：

```bash
cd /home/yjavae/projects/cpp/http_server
docker compose config
```

`docker compose config` 只校验并展开配置，不会启动容器。

## 2. 最快启动

执行：

```bash
docker compose up --build -d
docker compose ps
```

第一次会下载 Ubuntu 基础镜像、安装构建依赖并编译，耗时较长；后续未改动相关文件时会使用缓存。`-d` 表示在后台运行。

预期会看到三个服务：

| 服务 | 作用 | 宿主机端口 |
| --- | --- | --- |
| `gateway` | PulseGate 网关 | `8080` |
| `upstream-a` | 第一个演示上游 | 不暴露 |
| `upstream-b` | 第二个演示上游 | 不暴露 |

等待 `upstream-a`、`upstream-b` 变为 `healthy` 后，验证网关：

```bash
curl --fail http://127.0.0.1:8080/livez
curl --include http://127.0.0.1:8080/api/hello
```

第一条应输出 `alive`；第二条应得到来自某个 mock 上游的 HTTP 200 响应。多执行几次第二条，可在日志中观察请求被转发给两个上游。

## 3. 看日志与排错

```bash
# 持续查看网关日志，Ctrl+C 只退出日志，不停止服务
docker compose logs -f gateway

# 查看全部服务最近 100 行日志
docker compose logs --tail=100

# 查看容器状态、健康检查和退出码
docker compose ps
```

启动失败时，先执行 `docker compose logs --tail=100`。端口 `8080` 被占用时，可停止占用程序，或把 `compose.yaml` 中 `8080:8080` 的左侧改为未占用端口，例如 `18080:8080`，随后以 `http://127.0.0.1:18080` 访问。

## 4. 修改配置后重启

Compose 将 `config/pulsegate.docker.yaml` 以只读方式挂载到容器的 `/etc/pulsegate/config.yaml`。因此可以先复制一份本地配置，再修改：

```bash
cp config/pulsegate.docker.yaml config/pulsegate.local.yaml
```

接着把 `compose.yaml` 中 gateway 的挂载路径改为 `./config/pulsegate.local.yaml:/etc/pulsegate/config.yaml:ro`，然后重建该服务：

```bash
docker compose up -d --build gateway
```

`config/pulsegate.local.yaml` 已被 `.gitignore` 忽略，适合放本机地址、调试参数等不应提交的内容。改动 C++ 代码、`Dockerfile`、CMake 或依赖时，也使用同一条 `docker compose up -d --build`。

## 5. 停止与清理

日常停止演示环境：

```bash
docker compose down
```

这会停止并删除本项目创建的容器和网络，不会删除源代码或 Docker 镜像。若需要连本项目镜像也重新构建，可额外执行：

```bash
docker compose build --no-cache
```

`--no-cache` 会明显变慢，只在怀疑缓存导致问题时使用。

## 6. 这套容器化做了什么

```text
Dockerfile 的 build 阶段
  Ubuntu + CMake + Boost + 编译器 → 编译 Release 版 pulsegate
                                      │
                                      ▼
Dockerfile 的 runtime 阶段
  仅复制 pulsegate + Docker 配置，以 UID 10001 非 root 运行
                                      │
                                      ▼
compose.yaml
  gateway:8080 ──► upstream-a:9000 / upstream-b:9000
```

- **多阶段构建**：最终运行镜像不含源码、CMake 和编译器，体积和攻击面更小。
- **非 root 用户**：网关与 mock 上游均使用 UID `10001` 运行。
- **只读根文件系统**：服务只在临时的 `/tmp` 可写，减少意外写入。
- **健康检查与依赖顺序**：网关等待两个上游健康后再启动。
- **优雅停止**：`docker stop` 发送 `SIGTERM`，网关会进入已有的优雅停机流程。

## 7. 公司网络使用代理（可选）

只有 Docker 构建阶段无法下载 apt 依赖时才需要代理。不要把代理地址写进 `Dockerfile` 或提交到 Git。一次性传入环境变量即可：

```bash
docker compose build \
  --build-arg HTTP_PROXY="$HTTP_PROXY" \
  --build-arg HTTPS_PROXY="$HTTPS_PROXY" \
  --build-arg NO_PROXY="$NO_PROXY"
```

这些参数只用于 build 阶段，不会写入最终运行镜像。构建完成后照第 2 节执行 `docker compose up -d`。

## 常用命令速查

```bash
docker compose config              # 校验配置
docker compose up --build -d       # 构建并后台启动
docker compose ps                  # 查看服务状态
docker compose logs -f gateway     # 跟踪网关日志
curl --fail http://127.0.0.1:8080/livez
docker compose down                # 停止并删除演示环境
```
