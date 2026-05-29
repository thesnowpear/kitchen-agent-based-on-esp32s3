"""Reminder tables."""

from datetime import datetime
from uuid import UUID

from sqlalchemy import DateTime, ForeignKey, String
from sqlalchemy.orm import Mapped, mapped_column

from app.db.base import Base
from app.models.common import JsonDict, TimestampMixin, UuidPkMixin, json_dict_column


class Reminder(TimestampMixin, UuidPkMixin, Base):
    """临期、库存不足和设备异常提醒。"""

    __tablename__ = "reminders"

    home_id: Mapped[UUID] = mapped_column(ForeignKey("homes.id"), index=True)
    device_id: Mapped[UUID | None] = mapped_column(ForeignKey("devices.id"), index=True)
    item_id: Mapped[UUID | None] = mapped_column(ForeignKey("inventory_items.id"), index=True)
    reminder_type: Mapped[str] = mapped_column(String(64), index=True)
    title: Mapped[str] = mapped_column(String(160))
    content: Mapped[str | None] = mapped_column(String(500))
    status: Mapped[str] = mapped_column(String(32), default="pending", index=True)
    due_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    acked_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    extra: Mapped[JsonDict] = json_dict_column()
