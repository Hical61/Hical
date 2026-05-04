# Docker 多平台测试

在本地通过 Docker 复刻 GitHub Actions CI 的 Linux 测试矩阵，无需 Linux 机器即可验证编译和测试。

## 前提条件

- [Docker Desktop](https://www.docker.com/products/docker-desktop/) 已安装并运行
- Docker Compose V2（Docker Desktop 自带）

## 测试矩阵

| 服务           | Dockerfile                | 编译器   | 构建类型 | 说明                    |
| -------------- | ------------------------- | -------- | -------- | ----------------------- |
| `gcc14`        | `gcc14.Dockerfile`        | GCC 14   | Release  | 核心测试（470 个用例）  |
| `clang20`      | `clang20.Dockerfile`      | Clang 20 | Debug    | ASan + UBSan 内存安全   |
| `clang20-tidy` | `clang20-tidy.Dockerfile` | Clang 20 | Debug    | clang-tidy 静态分析     |

所有参数（编译器版本、cmake flags、ctest 超时、sanitizer 选项）均与 `.github/workflows/ci.yml` 保持一致。

> **注意：** Ubuntu 24.04 自带 Boost 1.83，不满足 DB 中间件所需的 >= 1.85，因此 `HICAL_WITH_DATABASE` 未启用。DB 测试由 GitHub Actions CI 覆盖。

## 快速开始

所有命令在**项目根目录**执行：

```bash
# 全部平台测试（一键）
docker compose -f docker-compose.test.yml up --build --abort-on-container-exit
```

## 按需运行

```bash
# 仅 GCC 14
docker compose -f docker-compose.test.yml up --build gcc14

# 仅 Clang 20 Sanitizer
docker compose -f docker-compose.test.yml up --build clang20

# 仅 clang-tidy 静态分析
docker compose -f docker-compose.test.yml up --build clang20-tidy
```

## 清理

```bash
# 停止并移除容器
docker compose -f docker-compose.test.yml down

# 同时清理构建镜像
docker compose -f docker-compose.test.yml down --rmi local
```

## 设计说明

### 编译 vs 测试的分离

- **编译**在 `docker build` 阶段完成，利用 Docker 层缓存 — 源码不变时不会重新编译
- **测试**在 `docker run`（CMD）阶段执行

### 镜像源

Dockerfile 内使用清华镜像（Ubuntu APT）和 USTC 镜像（LLVM 20 APT），无需代理即可在国内网络环境构建。

### 与 CI 的对应关系

| CI 矩阵                              | Docker 服务    | 差异                                                   |
| ------------------------------------- | -------------- | ------------------------------------------------------ |
| ubuntu-24.04 / gcc-14                 | `gcc14`        | 去掉 ccache（容器内无缓存意义）；未启用 DB 模块        |
| ubuntu-24.04 / clang-20 (Sanitizer)   | `clang20`      | 同上                                                   |
| ubuntu-24.04 / clang-20 (clang-tidy)  | `clang20-tidy` | 独立容器，仅静态分析不跑测试                           |
| windows / msys2-gcc                   | —              | Docker 不支持 Windows 容器编译                         |
| windows / msvc                        | —              | Docker 不支持 Windows 容器编译                         |

### 为什么禁用 io_uring

`HICAL_DISABLE_IO_URING=ON` — Docker 默认 seccomp 策略限制 `io_uring` 系统调用，与 CI 行为一致。

## 故障排查

**构建失败 — APT 源超时**

Dockerfile 默认使用清华/USTC 国内镜像源。如果镜像源临时不可用，可以手动替换为其他源（如阿里云 `mirrors.aliyun.com`）。

**Sanitizer 误报**

ASan/UBSan 在容器环境下偶尔会有环境相关的误报。如果本地复现但 CI 不报，可以对比 CI 日志确认。
