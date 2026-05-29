"""User and WeChat session tables."""

from datetime import datetime
from uuid import UUID

from sqlalchemy import DateTime, ForeignKey, String
from sqlalchemy.orm import Mapped, mapped_column, relationship

from app.db.base import Base
from app.models.common import TimestampMixin, UuidPkMixin


class User(TimestampMixin, UuidPkMixin, Base):
    """小程序用户主表，openid/unionid 只保存标识，不保存敏感凭证。"""

    __tablename__ = "users"

    display_name: Mapped[str | None] = mapped_column(String(80))
    avatar_url: Mapped[str | None] = mapped_column(String(500))
    primary_openid: Mapped[str | None] = mapped_column(String(128), unique=True, index=True)

    wx_sessions: Mapped[list["WxSession"]] = relationship(back_populates="user")


class WxSession(TimestampMixin, UuidPkMixin, Base):
    """微信 code2Session 结果表，只保存会话摘要和过期时间。"""

    __tablename__ = "wx_sessions"

    user_id: Mapped[UUID] = mapped_column(ForeignKey("users.id"), index=True)
    openid: Mapped[str] = mapped_column(String(128), index=True)
    unionid: Mapped[str | None] = mapped_column(String(128), index=True)
    session_key_ciphertext: Mapped[str | None] = mapped_column(String(512))
    expires_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))

    user: Mapped[User] = relationship(back_populates="wx_sessions")
