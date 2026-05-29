"""用户级设置（隐私 + 偏好）相关 schema。

外层统一 ApiResponse[T] 包壳，因此这里业务模型不带 ok 字段。
"""

from datetime import datetime

from pydantic import Field

from app.schemas.common import CamelModel, JsonDict


# 默认隐私开关：云同步默认开（方便首屏可用），诊断默认关（最小化数据上报），
# 提醒推送默认开（小程序临期提醒是核心价值之一）。
DEFAULT_PRIVACY: JsonDict = {
    "allowCloudSync": True,
    "allowUsageDiagnostics": False,
    "allowReminderPush": True,
}


class PrivacySettings(CamelModel):
    """隐私开关。字段名沿用 camelCase（直接对齐小程序 PrivacySettings 接口）。

    所有开关默认值与 DEFAULT_PRIVACY 保持一致，供 schema 单独使用时参考。
    """

    allow_cloud_sync: bool = True
    allow_usage_diagnostics: bool = False
    allow_reminder_push: bool = True


class UserSettingsData(CamelModel):
    """GET / PUT /settings 的 data 字段。

    privacy / preferences 都用任意 JSON dict 透传，避免每加一个开关就要改 schema。
    updated_at 由 ORM 触发器自动维护，无记录时为 None。
    """

    privacy: JsonDict = Field(default_factory=lambda: dict(DEFAULT_PRIVACY))
    preferences: JsonDict = Field(default_factory=dict)
    updated_at: datetime | None = None


class SettingsUpdateRequest(CamelModel):
    """PUT /settings 请求体：privacy / preferences 任一可缺省。

    缺省字段表示"不更新"，会和现有 JSON 浅合并；
    若希望删除某个键，需要传新的完整 dict 覆盖。
    """

    privacy: JsonDict | None = None
    preferences: JsonDict | None = None


__all__ = [
    "DEFAULT_PRIVACY",
    "PrivacySettings",
    "UserSettingsData",
    "SettingsUpdateRequest",
]
