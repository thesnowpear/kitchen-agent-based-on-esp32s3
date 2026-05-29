"""三端同步状态与事件表。

同步层以家庭为单位维护单调递增的 server_revision。小程序、服务器和设备都把
本地变更包装为事件上报，后端按 client_event_id 幂等落库，并用后写覆盖策略生成
当前快照。
"""

from uuid import UUID

from sqlalchemy import BigInteger, ForeignKey, Integer, String, UniqueConstraint
from sqlalchemy.orm import Mapped, mapped_column

from app.db.base import Base
from app.models.common import JsonDict, TimestampMixin, UuidPkMixin, json_dict_column


class SyncState(TimestampMixin, UuidPkMixin, Base):
    """家庭级同步游标。"""

    __tablename__ = "sync_states"
    __table_args__ = (UniqueConstraint("home_id", name="uq_sync_states_home"),)

    home_id: Mapped[UUID] = mapped_column(ForeignKey("homes.id"), index=True)
    current_revision: Mapped[int] = mapped_column(BigInteger, default=0)
    last_source: Mapped[str | None] = mapped_column(String(32))
    last_client_event_id: Mapped[str | None] = mapped_column(String(128))


class SyncEvent(TimestampMixin, UuidPkMixin, Base):
    """同步事件日志，作为增量拉取和审计依据。"""

    __tablename__ = "sync_events"
    __table_args__ = (
        UniqueConstraint("home_id", "client_event_id", name="uq_sync_events_home_client_event"),
    )

    home_id: Mapped[UUID] = mapped_column(ForeignKey("homes.id"), index=True)
    user_id: Mapped[UUID | None] = mapped_column(ForeignKey("users.id"), index=True)
    device_id: Mapped[UUID | None] = mapped_column(ForeignKey("devices.id"), index=True)
    device_sn: Mapped[str | None] = mapped_column(String(80), index=True)
    client_event_id: Mapped[str] = mapped_column(String(128), index=True)
    domain: Mapped[str] = mapped_column(String(64), index=True)
    op: Mapped[str] = mapped_column(String(64), index=True)
    source: Mapped[str] = mapped_column(String(32), default="miniapp", index=True)
    server_revision: Mapped[int] = mapped_column(BigInteger, index=True)
    client_revision: Mapped[int | None] = mapped_column(BigInteger)
    schema_version: Mapped[int] = mapped_column(Integer, default=1)
    payload: Mapped[JsonDict] = json_dict_column()

