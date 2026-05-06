# Hical 生产部署

基于 Docker Compose 的 Hical + MySQL + Nginx 生产级部署方案。

## 目录结构

```
docker/prod/
├── Dockerfile            # 多阶段构建（Ubuntu 24.04 + Conan 2）
├── docker-compose.yml    # 三服务编排
├── nginx.conf            # Nginx 反向代理配置
├── init.sql              # MySQL 初始化脚本
├── prod_server.cpp       # 生产级入口程序
├── .env.example          # 环境变量示例
└── README.md
```

## 快速开始

```bash
cd docker/prod
docker compose up -d --build
```

验证：

```bash
# 健康检查
curl http://localhost/health
curl http://localhost/health/ready

# Prometheus 指标
curl http://localhost/metrics

# API
curl http://localhost/api/users
curl -X POST http://localhost/api/users \
  -H 'Content-Type: application/json' \
  -d '{"name":"test","email":"test@example.com"}'

# 日志管理
curl http://localhost/admin/log-level
```

## 日志

```bash
# 实时查看
docker compose logs -f hical

# 仅查看最近 100 行
docker compose logs --tail=100 hical
```

## 停止

```bash
# 停止服务
docker compose down

# 停止并清除数据卷
docker compose down -v
```

## 配置说明

### 环境变量

参考 `.env.example`，关键变量：

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `PORT` | 8080 | Hical 监听端口 |
| `IO_THREADS` | CPU 核数 | IO 线程数 |
| `LOG_LEVEL` | INFO | 日志级别 |
| `LOG_FORMAT` | json | 日志格式（json/text） |
| `MYSQL_HOST` | mysql | MySQL 主机 |
| `MYSQL_PASSWORD` | — | MySQL 密码 |
| `DB_MAX_CONNS` | 16 | 数据库最大连接数 |

### HTTPS

1. 准备证书文件（`fullchain.pem` + `privkey.pem`），放入 `certs/` 目录
2. 编辑 `nginx.conf`，取消 SSL 相关注释
3. 编辑 `docker-compose.yml`，取消 `443:443` 和 `certs` 挂载的注释

### 生产加固

- 修改所有默认密码
- 启用 HTTPS
- 限制 `/metrics` 和 `/admin/` 端点的访问范围
- 调整资源限制（cpus/memory）
