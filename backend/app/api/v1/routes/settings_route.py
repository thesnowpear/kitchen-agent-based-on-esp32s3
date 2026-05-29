"""用户级设置（隐私 + 偏好）路由。

- GET /settings：无记录则返默认值，不会自动 insert（避免给只读访问写库）。
- PUT /settings：upsert UserSettings；privacy / preferences 任一可缺省。

外层统一 ApiResponse[T] 包壳。
"""

from fastapi import APIRouter, Depends
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.deps import get_current_user
from app.db.session import get_db
from app.models.user import User
from app.models.user_settings import UserSettings
from app.schemas.common import ApiResponse, JsonDict
from app.schemas.settings import (
    DEFAULT_PRIVACY,
    SettingsUpdateRequest,
    UserSettingsData,
)

router = APIRouter()


def _record_to_data(record: UserSettings | None) -> UserSettingsData:
    """无记录时返回默认值；有记录时直接渲染。

    DEFAULT_PRIVACY 用浅拷贝避免不同请求共享同一个 dict 实例。
    """
    if record is None:
        return UserSettingsData(
            privacy=dict(DEFAULT_PRIVACY),
            preferences={},
            updated_at=None,
        )
    # 把数据库里可能为 None 的 JSON 字段统一规范成 dict，避免 schema 校验失败。
    privacy: JsonDict = dict(DEFAULT_PRIVACY)
    if record.privacy:
        privacy.update(record.privacy)
    preferences: JsonDict = dict(record.preferences or {})
    return UserSettingsData(
        privacy=privacy,
        preferences=preferences,
        updated_at=record.updated_at,
    )


@router.get("", response_model=ApiResponse[UserSettingsData])
async def settings_get(
    user: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[UserSettingsData]:
    """读取当前用户的设置；无记录时返回默认值（不写库）。"""
    row = await db.execute(select(UserSettings).where(UserSettings.user_id == user.id))
    record = row.scalar_one_or_none()
    return ApiResponse[UserSettingsData](data=_record_to_data(record))


@router.put("", response_model=ApiResponse[UserSettingsData])
async def settings_update(
    payload: SettingsUpdateRequest,
    user: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[UserSettingsData]:
    """upsert 当前用户的设置。

    payload.privacy / preferences 缺省表示"不更新"；想清空请显式传 {}。
    """
    row = await db.execute(select(UserSettings).where(UserSettings.user_id == user.id))
    record = row.scalar_one_or_none()

    if record is None:
        # 首次写入：先用默认值兜底，再用 payload 覆盖。
        privacy: JsonDict = dict(DEFAULT_PRIVACY)
        if payload.privacy is not None:
            privacy.update(payload.privacy)
        preferences: JsonDict = dict(payload.preferences or {})
        record = UserSettings(
            user_id=user.id,
            privacy=privacy,
            preferences=preferences,
        )
        db.add(record)
    else:
        # 浅合并而非整体替换；想"清空"某个 key 可以显式传 {} 或新的完整 dict。
        if payload.privacy is not None:
            merged_privacy: JsonDict = dict(record.privacy or {})
            merged_privacy.update(payload.privacy)
            record.privacy = merged_privacy
        if payload.preferences is not None:
            merged_preferences: JsonDict = dict(record.preferences or {})
            merged_preferences.update(payload.preferences)
            record.preferences = merged_preferences

    await db.commit()
    await db.refresh(record)
    return ApiResponse[UserSettingsData](data=_record_to_data(record))
