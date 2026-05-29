"""User settings tables."""

from uuid import UUID

from sqlalchemy import ForeignKey
from sqlalchemy.orm import Mapped, mapped_column

from app.db.base import Base
from app.models.common import JsonDict, TimestampMixin, UuidPkMixin, json_dict_column


class UserSettings(TimestampMixin, UuidPkMixin, Base):
    """小程序用户级设置（每个用户唯一一行）。

    privacy 字段使用 JSONB 灵活存储：allowCloudSync / allowUsageDiagnostics / allowReminderPush
    等开关；后续如有更多偏好（AI 模型偏好、提醒频率）也都直接进 JSON，避免列爆炸。
    """

    __tablename__ = "user_settings"

    user_id: Mapped[UUID] = mapped_column(
        ForeignKey("users.id"), unique=True, index=True
    )
    # 隐私开关集合，键名沿用小程序 PrivacySettings 接口字段（camelCase）。
    privacy: Mapped[JsonDict] = json_dict_column()
    # 其他偏好（模型选择、提醒频率、UI 主题等），同样 JSONB 扁平存。
    preferences: Mapped[JsonDict] = json_dict_column()
