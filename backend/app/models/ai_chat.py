"""AI 对话历史表。

本表只保存小程序/后端这条云端链路的短期对话，用于多端查看和继续会话。
设备端 NVS/Flash 内的短期历史仍由固件维护，后续通过 MQTT 同步任务再合并。
"""

from uuid import UUID

from sqlalchemy import ForeignKey, String, Text
from sqlalchemy.orm import Mapped, mapped_column

from app.db.base import Base
from app.models.common import JsonDict, TimestampMixin, UuidPkMixin, json_dict_column


class AiChatMessage(TimestampMixin, UuidPkMixin, Base):
    """云端 AI 会话消息，按 home + session_id 分组。"""

    __tablename__ = "ai_chat_messages"

    home_id: Mapped[UUID] = mapped_column(ForeignKey("homes.id"), index=True)
    user_id: Mapped[UUID | None] = mapped_column(ForeignKey("users.id"), index=True)
    session_id: Mapped[str] = mapped_column(String(128), index=True)
    role: Mapped[str] = mapped_column(String(16), index=True)
    content: Mapped[str] = mapped_column(Text)
    source: Mapped[str | None] = mapped_column(String(32))
    fallback_reason: Mapped[str | None] = mapped_column(String(256))
    model_used: Mapped[str | None] = mapped_column(String(128))
    device_sn: Mapped[str | None] = mapped_column(String(80), index=True)
    extra: Mapped[JsonDict] = json_dict_column()
