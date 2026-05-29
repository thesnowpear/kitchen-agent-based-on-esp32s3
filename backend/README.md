# 冰箱小精灵 Backend v1

这是“冰箱小精灵”远程服务的 FastAPI 单体骨架。当前目标是把 v1 接口、数据模型、配置和本地运行依赖搭好，方便后续接入真实微信登录、PostgreSQL、EMQX、Nginx 和设备同步链路。

## 技术栈

- Python 3.11+
- FastAPI + Pydantic Settings
- SQLAlchemy 2.x async ORM
- PostgreSQL asyncpg 驱动
- EMQX MQTT Broker
- Nginx 反向代理

## 目录结构

```text
backend/
  app/
    api/v1/          # v1 路由
    core/            # 配置与应用生命周期
    db/              # SQLAlchemy engine/session/base
    models/          # 数据库表定义
    schemas/         # Pydantic 请求/响应模型
    services/        # 微信、设备、库存、MQTT 等业务占位服务
    main.py          # FastAPI 启动入口
  deploy/
    nginx/           # Nginx 骨架配置
    emqx/            # EMQX 配置占位
  docker-compose.yml
  Dockerfile
  requirements.txt
  .env.example
```

## 本地启动

复制环境变量模板：

```powershell
Copy-Item .env.example .env
```

使用 Docker Compose 启动 FastAPI、Postgres、EMQX、Nginx：

```powershell
docker compose up --build
```

服务地址：

- FastAPI: `http://localhost:8000`
- API 文档: `http://localhost:8000/docs`
- Nginx 代理: `http://localhost:6005`
- EMQX Dashboard: `http://localhost:18083`
- PostgreSQL: `localhost:5432`

不使用 Docker 时，可以直接运行：

```powershell
python -m venv .venv
. .venv\Scripts\Activate.ps1
pip install -r requirements.txt
uvicorn app.main:app --reload --host 0.0.0.0 --port 8000
```

## v1 接口

所有业务接口默认挂在 `/api/v1` 下。

- `POST /api/v1/wx/login`
- `POST /api/v1/home/create`
- `POST /api/v1/device/bind`
- `POST /api/v1/device/unbind`
- `GET /api/v1/device/list`
- `GET /api/v1/device/status`
- `GET /api/v1/inventory/list`
- `POST /api/v1/inventory/update`
- `POST /api/v1/inventory/event`
- `GET /api/v1/reminder/list`
- `POST /api/v1/reminder/ack`
- `POST /api/v1/mqtt/event/ingest`
- `POST /api/v1/mqtt/command/publish`
- `POST /api/v1/notification/subscribe`
- `GET /healthz`

## 当前占位说明

- 微信 `code2Session` 目前由 `app/services/wx_auth.py` 占位实现：未配置 `WX_APPID/WX_SECRET` 时会生成稳定的 demo openid，方便前端先联调。
- MQTT 发布目前由 `app/services/mqtt_gateway.py` 记录命令请求并返回待发布主题；后续可接入 `gmqtt`、`paho-mqtt` 或 EMQX HTTP API。
- 数据库模型已经定义，但未引入 Alembic 迁移。后续进入真实部署前建议补 `alembic init`，由迁移管理表结构。
- 本地开发默认 `AUTO_CREATE_TABLES=true`，应用启动时会尝试创建表；生产环境应关闭该开关并改用 Alembic。
- API Key、微信密钥、数据库口令都通过环境变量配置，禁止写入日志或提交真实值。
