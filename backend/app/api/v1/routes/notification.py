"""Notification routes."""

from fastapi import APIRouter, Depends
from sqlalchemy.ext.asyncio import AsyncSession

from app.db.session import get_db
from app.models.notification import NotificationSubscription
from app.schemas.notification import NotificationSubscribeRequest, NotificationSubscribeResponse

router = APIRouter()


@router.post("/subscribe", response_model=NotificationSubscribeResponse)
async def notification_subscribe(
    payload: NotificationSubscribeRequest,
    db: AsyncSession = Depends(get_db),
) -> NotificationSubscribeResponse:
    """保存通知订阅授权，后续由提醒服务触发真实推送。"""
    subscription = NotificationSubscription(**payload.model_dump(), status="active")
    db.add(subscription)
    await db.commit()
    return NotificationSubscribeResponse(subscription_id=subscription.id, status=subscription.status)
