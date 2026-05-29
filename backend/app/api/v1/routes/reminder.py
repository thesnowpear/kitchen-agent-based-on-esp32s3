"""旧版 /reminder 路由（单数前缀），保留作为兼容 alias。

新版 RESTful 见 routes/reminders_alias.py（复数前缀 /reminders）。
"""

from datetime import datetime, timezone
from uuid import UUID

from fastapi import APIRouter, Depends, HTTPException, Query
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.db.session import get_db
from app.models.reminder import Reminder
from app.schemas.common import ApiResponse
from app.schemas.reminder import (
    ReminderAckRequest,
    ReminderAckResponse,
    ReminderListResponse,
    ReminderSchema,
)

router = APIRouter()


def _reminder_to_schema(item: Reminder) -> ReminderSchema:
    """ORM → DTO（集中渲染，避免每个 route 重复字段映射）。"""
    return ReminderSchema(
        id=item.id,
        reminder_type=item.reminder_type,
        title=item.title,
        content=item.content,
        status=item.status,
        due_at=item.due_at,
        acked_at=item.acked_at,
    )


@router.get("/list", response_model=ApiResponse[ReminderListResponse])
async def reminder_list(
    home_id: UUID = Query(...),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[ReminderListResponse]:
    """旧版列表：按 home_id query 显式查询。按 due_at asc nulls last 排。"""
    result = await db.execute(
        select(Reminder).where(Reminder.home_id == home_id).order_by(Reminder.due_at.asc().nullslast())
    )
    items = [_reminder_to_schema(item) for item in result.scalars().all()]
    return ApiResponse[ReminderListResponse](data=ReminderListResponse(items=items))


@router.post("/ack", response_model=ApiResponse[ReminderAckResponse])
async def reminder_ack(
    payload: ReminderAckRequest,
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[ReminderAckResponse]:
    """旧版 ack：reminder_id 必传；status 仅允许 acked / dismissed。"""
    if payload.reminder_id is None:
        raise HTTPException(status_code=400, detail="reminder_id is required")
    reminder = await db.get(Reminder, payload.reminder_id)
    if reminder is None:
        raise HTTPException(status_code=404, detail="reminder not found")
    reminder.status = payload.status
    reminder.acked_at = datetime.now(timezone.utc)
    await db.commit()
    return ApiResponse[ReminderAckResponse](
        data=ReminderAckResponse(reminder_id=reminder.id, status=reminder.status),
    )
