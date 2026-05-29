"""Device binding and status tables."""

from datetime import datetime
from uuid import UUID

from sqlalchemy import DateTime, ForeignKey, String, UniqueConstraint
from sqlalchemy.orm import Mapped, mapped_column

from app.db.base import Base
from app.models.common import JsonDict, TimestampMixin, UuidPkMixin, json_dict_column


class Device(TimestampMixin, UuidPkMixin, Base):
    """冰箱贴设备主表，device_sn 来自出厂或固件配置。"""

    __tablename__ = "devices"

    device_sn: Mapped[str] = mapped_column(String(80), unique=True, index=True)
    name: Mapped[str | None] = mapped_column(String(80))
    model: Mapped[str | None] = mapped_column(String(80))
    firmware_version: Mapped[str | None] = mapped_column(String(80))
    status: Mapped[str] = mapped_column(String(32), default="offline", index=True)
    last_seen_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    last_ip: Mapped[str | None] = mapped_column(String(64))
    extra: Mapped[JsonDict] = json_dict_column()


class DeviceBinding(TimestampMixin, UuidPkMixin, Base):
    """设备与家庭绑定关系，同一设备同一时间只应有一个 active 绑定。"""

    __tablename__ = "device_bindings"
    __table_args__ = (UniqueConstraint("device_id", "home_id", name="uq_device_home_binding"),)

    device_id: Mapped[UUID] = mapped_column(ForeignKey("devices.id"), index=True)
    home_id: Mapped[UUID] = mapped_column(ForeignKey("homes.id"), index=True)
    bound_by_user_id: Mapped[UUID] = mapped_column(ForeignKey("users.id"), index=True)
    status: Mapped[str] = mapped_column(String(32), default="active", index=True)
    unbound_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))


class DeviceStatusEvent(TimestampMixin, UuidPkMixin, Base):
    """设备状态事件，保留 MQTT 上报、HTTP 上报和诊断快照。"""

    __tablename__ = "device_status_events"

    device_id: Mapped[UUID | None] = mapped_column(ForeignKey("devices.id"), index=True)
    device_sn: Mapped[str] = mapped_column(String(80), index=True)
    event_type: Mapped[str] = mapped_column(String(80), index=True)
    payload: Mapped[JsonDict] = json_dict_column()
