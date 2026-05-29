"""Notification subscription tables."""

from uuid import UUID

from sqlalchemy import ForeignKey, String, UniqueConstraint
from sqlalchemy.orm import Mapped, mapped_column

from app.db.base import Base
from app.models.common import JsonDict, TimestampMixin, UuidPkMixin, json_dict_column


class NotificationSubscription(TimestampMixin, UuidPkMixin, Base):
    """小程序订阅消息或其他推送渠道的授权记录。"""

    __tablename__ = "notification_subscriptions"
    __table_args__ = (UniqueConstraint("user_id", "channel", "target", name="uq_notification_target"),)

    user_id: Mapped[UUID] = mapped_column(ForeignKey("users.id"), index=True)
    home_id: Mapped[UUID | None] = mapped_column(ForeignKey("homes.id"), index=True)
    channel: Mapped[str] = mapped_column(String(32), default="wechat")
    target: Mapped[str] = mapped_column(String(200))
    status: Mapped[str] = mapped_column(String(32), default="active", index=True)
    extra: Mapped[JsonDict] = json_dict_column()
