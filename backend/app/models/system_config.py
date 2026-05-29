"""系统级配置表：当前只用于 AI 配置双向同步。

设计：
- 每个 `(home_id, config_key)` 唯一一行。
- value 存 JSONB，schema 由业务层定义（见 schemas/ai_config.py）。
- config_updated_at 是业务时间戳（由小程序 / 设备 / backend 写入时更新），与 ORM updated_at 区分；
  用于 last-write-wins 冲突解决，比较两端 timestamp 决定谁覆盖谁。

API key 等敏感字段直接明文存进 value JSONB。应付比赛/本地开发期可接受，
上公网前必须：a) 启用 PostgreSQL 字段级加密；b) MQTT 走 TLS；c) NVS 加密。
"""

from datetime import datetime
from uuid import UUID

from sqlalchemy import DateTime, ForeignKey, String, UniqueConstraint
from sqlalchemy.orm import Mapped, mapped_column

from app.db.base import Base
from app.models.common import JsonDict, TimestampMixin, UuidPkMixin, json_dict_column


class SystemConfig(TimestampMixin, UuidPkMixin, Base):
    """家庭级系统配置单元，按 (home_id, config_key) 唯一。"""

    __tablename__ = "system_config"
    __table_args__ = (
        UniqueConstraint("home_id", "config_key", name="uq_system_config_home_key"),
    )

    # 当前只支持挂在家庭下；后续可加 user 级或全局级（home_id=NULL）。
    home_id: Mapped[UUID] = mapped_column(ForeignKey("homes.id"), index=True)
    # 配置类别，当前只有 'ai_config'。
    config_key: Mapped[str] = mapped_column(String(64), index=True)
    # 业务载荷，schema 由 schemas/ai_config.py 等定义。
    value: Mapped[JsonDict] = json_dict_column()
    # 业务级时间戳：作为双向同步的 last-write-wins 依据。
    config_updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), nullable=False
    )
