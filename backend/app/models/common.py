"""Shared ORM columns and enum helpers."""

from datetime import datetime
from typing import Any
from uuid import UUID, uuid4

from sqlalchemy import DateTime, func
from sqlalchemy.dialects.postgresql import JSONB
from sqlalchemy.orm import Mapped, mapped_column


class TimestampMixin:
    """为业务表提供统一创建和更新时间。"""

    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        server_default=func.now(),
        nullable=False,
    )
    updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        server_default=func.now(),
        onupdate=func.now(),
        nullable=False,
    )


class UuidPkMixin:
    """使用 UUID 作为主键，方便设备端和云端事件合并。"""

    id: Mapped[UUID] = mapped_column(primary_key=True, default=uuid4)


def jsonb_default() -> dict[str, Any]:
    return {}


JsonDict = dict[str, Any]


def json_dict_column() -> Mapped[JsonDict]:
    """创建独立 JSONB 列，避免多个模型复用同一个 mapped_column 实例。"""
    return mapped_column(JSONB, default=jsonb_default, nullable=False)
