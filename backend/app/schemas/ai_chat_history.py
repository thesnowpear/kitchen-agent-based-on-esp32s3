"""AI 对话历史接口 schema。"""

from datetime import datetime
from uuid import UUID

from pydantic import Field

from app.schemas.common import CamelModel


class AiChatHistoryMessage(CamelModel):
    """单条云端 AI 对话消息。"""

    id: UUID
    role: str
    content: str
    source: str | None = None
    fallback_reason: str | None = None
    model_used: str | None = None
    device_sn: str | None = None
    sent_at: datetime


class AiChatHistoryData(CamelModel):
    """GET /ai/history 的 data 字段。"""

    session_id: str
    messages: list[AiChatHistoryMessage]


class AiChatHistoryClearData(CamelModel):
    """DELETE /ai/history 的 data 字段。"""

    session_id: str
    deleted_count: int = Field(ge=0)


__all__ = [
    "AiChatHistoryClearData",
    "AiChatHistoryData",
    "AiChatHistoryMessage",
]
