"""Inventory item and event tables."""

from datetime import date
from uuid import UUID

from sqlalchemy import Date, ForeignKey, Integer, Numeric, String
from sqlalchemy.orm import Mapped, mapped_column

from app.db.base import Base
from app.models.common import JsonDict, TimestampMixin, UuidPkMixin, json_dict_column


class InventoryItem(TimestampMixin, UuidPkMixin, Base):
    """库存条目，AI 识别结果必须经用户确认后才进入 active 状态。"""

    __tablename__ = "inventory_items"

    home_id: Mapped[UUID] = mapped_column(ForeignKey("homes.id"), index=True)
    device_id: Mapped[UUID | None] = mapped_column(ForeignKey("devices.id"), index=True)
    name: Mapped[str] = mapped_column(String(120), index=True)
    category: Mapped[str | None] = mapped_column(String(80), index=True)
    quantity: Mapped[float] = mapped_column(Numeric(10, 2), default=1)
    unit: Mapped[str] = mapped_column(String(24), default="份")
    # 结构化位置：参考 ui-reference 的「区域 + 九宫格 slot」模型。
    # zone 取值 freezer/left/right/door 或 custom_*；slot 取值 A1~C3。
    # 旧 location 字段保留为可读文本（例如「左侧冷藏 中·左」），由 service 层从 zone/slot 渲染回填。
    zone: Mapped[str | None] = mapped_column(String(32), index=True)
    slot: Mapped[str | None] = mapped_column(String(8))
    location: Mapped[str | None] = mapped_column(String(80))
    expire_date: Mapped[date | None] = mapped_column(Date)
    status: Mapped[str] = mapped_column(String(32), default="active", index=True)
    source: Mapped[str] = mapped_column(String(32), default="manual")
    confidence: Mapped[int | None] = mapped_column(Integer)
    extra: Mapped[JsonDict] = json_dict_column()


class InventoryEvent(TimestampMixin, UuidPkMixin, Base):
    """库存变更事件，用于设备端、云端和小程序之间做幂等同步。"""

    __tablename__ = "inventory_events"

    home_id: Mapped[UUID] = mapped_column(ForeignKey("homes.id"), index=True)
    device_id: Mapped[UUID | None] = mapped_column(ForeignKey("devices.id"), index=True)
    item_id: Mapped[UUID | None] = mapped_column(ForeignKey("inventory_items.id"), index=True)
    event_type: Mapped[str] = mapped_column(String(64), index=True)
    actor_type: Mapped[str] = mapped_column(String(32), default="user")
    actor_id: Mapped[str | None] = mapped_column(String(128))
    payload: Mapped[JsonDict] = json_dict_column()
    version: Mapped[int] = mapped_column(Integer, default=1)
