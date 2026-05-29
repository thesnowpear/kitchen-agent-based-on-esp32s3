"""新版 /reminders 路由（复数前缀，RESTful）。

- GET    /reminders                          当前活跃家庭的提醒列表。
- POST   /reminders/{reminder_id}/confirm    确认或忽略某条提醒；user_id 由 token 推断。

外层统一 ApiResponse[T] 包壳。
"""

from datetime import datetime, timezone
from uuid import UUID

from fastapi import APIRouter, Depends, HTTPException
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.deps import get_active_home, get_current_user
from app.db.session import get_db
from app.models.home import Home
from app.models.reminder import Reminder
from app.models.user import User
from app.schemas.common import ApiResponse
from app.schemas.reminder import (
    ReminderConfirmRequest,
    ReminderListData,
    ReminderSchema,
)
from app.services.sync_device_bridge import push_cloud_snapshot_to_devices
from app.services.sync_service import get_status, record_server_event

router = APIRouter()


def _reminder_to_schema(item: Reminder) -> ReminderSchema:
    """ORM → DTO，集中字段映射。"""
    return ReminderSchema(
        id=item.id,
        reminder_type=item.reminder_type,
        title=item.title,
        content=item.content,
        status=item.status,
        due_at=item.due_at,
        acked_at=item.acked_at,
    )


@router.get("", response_model=ApiResponse[ReminderListData])
async def reminders_list(
    home: Home = Depends(get_active_home),
    db: AsyncSession = Depends(get_db),
    _user: User = Depends(get_current_user),
) -> ApiResponse[ReminderListData]:
    """当前活跃家庭的提醒列表，按 due_at asc nulls last。"""
    result = await db.execute(
        select(Reminder)
        .where(Reminder.home_id == home.id)
        .order_by(Reminder.due_at.asc().nullslast())
    )
    items = [_reminder_to_schema(item) for item in result.scalars().all()]
    return ApiResponse[ReminderListData](data=ReminderListData(items=items))


@router.post("/{reminder_id}/confirm", response_model=ApiResponse[ReminderSchema])
async def reminders_confirm(
    reminder_id: UUID,
    payload: ReminderConfirmRequest,
    home: Home = Depends(get_active_home),
    db: AsyncSession = Depends(get_db),
    # user 暂未显式落库 actor_id，但鉴权链必须触发；保留变量避免 lint 警告。
    _user: User = Depends(get_current_user),
) -> ApiResponse[ReminderSchema]:
    """确认 / 忽略提醒。reminder 必须属于当前活跃家庭，否则视为 404。"""
    reminder = await db.get(Reminder, reminder_id)
    if reminder is None or reminder.home_id != home.id:
        raise HTTPException(status_code=404, detail="reminder not found")
    reminder.status = payload.status
    reminder.acked_at = datetime.now(timezone.utc)
    await record_server_event(
        db,
        home_id=home.id,
        user_id=_user.id,
        domain="reminder",
        op="confirm",
        payload={"id": str(reminder.id), "status": reminder.status},
    )
    await db.commit()
    await db.refresh(reminder)
    await push_cloud_snapshot_to_devices(
        db,
        home_id=home.id,
        server_revision=(await get_status(db, home.id)).server_revision,
        domains={"reminder"},
        request_device_inventory=False,
    )
    return ApiResponse[ReminderSchema](data=_reminder_to_schema(reminder))
