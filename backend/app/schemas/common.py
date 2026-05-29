"""Shared Pydantic schemas."""

from datetime import datetime
from typing import Any, Generic, TypeVar
from uuid import UUID

from pydantic import BaseModel, ConfigDict, Field
from pydantic.alias_generators import to_camel


# 业务对象 schema 公共基类：
# - alias_generator=to_camel 把 ORM/Python 端的 snake_case 字段在序列化时输出为 camelCase，
#   让微信小程序端（前端默认 camelCase）无需做命名转换。
# - populate_by_name=True 允许同时通过原名（snake_case）和别名（camelCase）写入，
#   方便后端内部 service 层仍然使用 snake_case。
# - from_attributes=True 让 SQLAlchemy ORM 实例可以直接 model_validate 出 schema。
class CamelModel(BaseModel):
    """所有业务 schema 的基类，统一 camelCase 输出 + ORM 直读。"""

    model_config = ConfigDict(
        alias_generator=to_camel,
        populate_by_name=True,
        from_attributes=True,
    )


T = TypeVar("T")


# 统一响应包壳：小程序 utils/request.ts 解 `body.data` 这条逻辑直接生效。
# - ok: 业务级成功标志，区分于 HTTP 状态码。
# - data: 业务数据载荷（泛型 T）；无数据接口（例如 ack 类）可不传，默认 None。
# - message: 错误信息或者 "ok"；调试用提示。
# - requestId: 透传给客户端的请求标识，便于跨端日志追踪。
class ApiResponse(CamelModel, Generic[T]):
    """统一响应壳，供所有 v1 路由复用。"""

    ok: bool = True
    data: T | None = None
    message: str = "ok"
    request_id: str | None = None


# 旧版兼容：device.py 的 device_unbind 仍用 `response_model=ApiResponse`，
# 不指定泛型参数等价于 ApiResponse[Any]，运行时不会破坏旧调用方。


class IdResponse(ApiResponse[None]):
    """仅返回一个 id 的轻量响应（保留以避免破坏潜在使用方）。"""

    id: UUID


class TimestampedSchema(CamelModel):
    """带 id + 时间戳的 ORM 读取基础结构。"""

    id: UUID
    created_at: datetime | None = None
    updated_at: datetime | None = None


class PageQuery(BaseModel):
    """通用分页查询参数（注意：query 参数不需要 camelCase 转换）。"""

    limit: int = Field(default=50, ge=1, le=200)
    offset: int = Field(default=0, ge=0)


# JSON 任意键值对的简短别名，避免在多处重复声明 dict[str, Any]。
JsonDict = dict[str, Any]
