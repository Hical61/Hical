# Docker

本目录包含 Hical 框架的所有 Docker 相关配置。

## 目录结构

```
docker/
├── test/               # 多平台编译测试（复刻 CI）
│   ├── gcc14.Dockerfile
│   ├── clang20.Dockerfile
│   ├── clang20-tidy.Dockerfile
│   └── README.md
├── prod/               # 生产级部署（Hical + MySQL + Nginx）
│   ├── Dockerfile
│   ├── docker-compose.yml
│   ├── nginx.conf
│   ├── init.sql
│   ├── prod_server.cpp
│   ├── .env.example
│   └── README.md
├── bench_main.cpp      # 压测服务器入口（benchmark/ 引用）
└── README.md           # 本文件
```

## 多平台测试

在项目根目录执行：

```bash
docker compose -f docker-compose.test.yml up --build --abort-on-container-exit
```

详见 [test/README.md](test/README.md)。

## 生产部署

```bash
cd docker/prod
docker compose up -d --build
```

详见 [prod/README.md](prod/README.md)。
