"""Home and membership tables."""

from uuid import UUID

from sqlalchemy import ForeignKey, String, UniqueConstraint
from sqlalchemy.orm import Mapped, mapped_column, relationship

from app.db.base import Base
from app.models.common import TimestampMixin, UuidPkMixin


class Home(TimestampMixin, UuidPkMixin, Base):
    """家庭空间，一个家庭下可绑定多个冰箱贴设备。"""

    __tablename__ = "homes"

    name: Mapped[str] = mapped_column(String(80))
    owner_user_id: Mapped[UUID] = mapped_column(ForeignKey("users.id"), index=True)

    members: Mapped[list["HomeMember"]] = relationship(back_populates="home")


class HomeMember(TimestampMixin, UuidPkMixin, Base):
    """家庭成员关系，后续可扩展 owner/admin/member 权限。"""

    __tablename__ = "home_members"
    __table_args__ = (UniqueConstraint("home_id", "user_id", name="uq_home_member_user"),)

    home_id: Mapped[UUID] = mapped_column(ForeignKey("homes.id"), index=True)
    user_id: Mapped[UUID] = mapped_column(ForeignKey("users.id"), index=True)
    role: Mapped[str] = mapped_column(String(32), default="owner")

    home: Mapped[Home] = relationship(back_populates="members")
