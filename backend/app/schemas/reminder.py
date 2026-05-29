"""提醒相关的请求 / 响应 schema。

外层统一 ApiResponse[T] 包壳，因此这里业务模型不带 ok 字段。
新增 ReminderListData / ReminderConfirmRequest 给 /reminders 复数 RESTful 路由用。
"""

from datetime import datetime
from uuid import UUID

from pydantic import Field

from app.schemas.common import CamelModel


class ReminderSchema(CamelModel):
    """提醒：reminder_type 区分临期 / 库存 / 设备异常。"""

    id: UUID
    reminder_type: str
    title: str
    content: str | None = None
    status: str
    due_at: datetime | None = None
    acked_at: datetime | None = None


class ReminderListResponse(CamelModel):
    """旧版列表响应壳（不再带 ok 字段，保留以兼容 import）。"""

    items: list[ReminderSchema]


class ReminderListData(CamelModel):
    """新版 GET /reminders 的 data 字段。"""

    items: list[ReminderSchema]


class ReminderAckRequest(CamelModel):
    """确认提醒，status 仅允许 acked 或 dismissed。

    user_id 在新版 /reminders/{id}/confirm 里由 token 推断，请求体里可不传；
    保留字段是为了让旧 /reminder/ack 路径继续工作。
    """

    reminder_id: UUID | None = None
    user_id: UUID | None = None
    status: str = Field(default="acked", pattern="^(acked|dismissed)$")


class ReminderConfirmRequest(CamelModel):
    """POST /reminders/{reminder_id}/confirm 的请求体。"""

    status: str = Field(default="acked", pattern="^(acked|dismissed)$")


class ReminderAckResponse(CamelModel):
    """旧版 ack 接口响应壳（保留以避免破坏 import）。"""

    reminder_id: UUID
    status: str
