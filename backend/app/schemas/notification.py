"""Notification subscription schemas."""

from uuid import UUID

from pydantic import Field

from app.schemas.common import CamelModel, JsonDict


class NotificationSubscribeRequest(CamelModel):
    """订阅授权登记：channel 当前默认微信，target 是订阅模板 ID 或推送目标。"""

    user_id: UUID
    home_id: UUID | None = None
    channel: str = Field(default="wechat", max_length=32)
    target: str = Field(min_length=1, max_length=200)
    extra: JsonDict = Field(default_factory=dict)


class NotificationSubscribeResponse(CamelModel):
    ok: bool = True
    subscription_id: UUID
    status: str
