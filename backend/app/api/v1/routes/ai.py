"""`/api/v1/ai/chat` 路由：AI 对话接口。

逻辑全部在 ai_service.chat_with_context 内：先 MQTT 转发设备，超时 / 无设备时降级 SiliconFlow。
本路由仅负责入参校验、token 鉴权和 ApiResponse 壳层封装。
"""

from __future__ import annotations

import logging

from fastapi import APIRouter, Depends, Query
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.deps import get_active_home, get_current_user
from app.db.session import get_db
from app.models.home import Home
from app.models.user import User
from app.schemas.ai_chat import AiChatRequest, AiChatResponseData
from app.schemas.ai_chat_history import AiChatHistoryClearData, AiChatHistoryData
from app.schemas.common import ApiResponse
from app.services.ai_chat_history_service import (
    append_turn,
    clear_history,
    list_history,
)
from app.services.ai_service import chat_with_context

router = APIRouter()
logger = logging.getLogger(__name__)


@router.post("/chat", response_model=ApiResponse[AiChatResponseData])
async def ai_chat(
    payload: AiChatRequest,
    user: User = Depends(get_current_user),
    home: Home = Depends(get_active_home),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[AiChatResponseData]:
    """转发 AI 对话给设备或云端模型，返回回复内容与 source 标识。

    返回 ok=False 仅在云端降级也失败（无 key / 网络错 / 模型 5xx）。
    设备转发超时但云端成功 → ok=True 且 source=cloud_fallback + fallbackReason='device_timeout'。
    """
    result = await chat_with_context(payload.prompt, home.id, db)
    session_id = await append_turn(
        db,
        home_id=home.id,
        user_id=user.id,
        session_id=payload.session_id,
        prompt=payload.prompt,
        result=result,
    )
    await db.commit()

    data = AiChatResponseData(
        source=result["source"],
        reply=result["reply"],
        fallback_reason=result.get("fallbackReason"),
        model_used=result.get("modelUsed"),
        device_sn=result.get("deviceSn"),
        session_id=session_id,
    )

    if not result.get("reply"):
        # reply 为空表示云端降级也没拿到内容，返 ok=False 让小程序展示错误。
        logger.warning(
            "ai_chat empty reply: user=%s home=%s source=%s reason=%s",
            user.id,
            home.id,
            result.get("source"),
            result.get("fallbackReason"),
        )
        return ApiResponse[AiChatResponseData](
            ok=False,
            message=result.get("fallbackReason") or "no reply from ai",
            data=data,
        )

    return ApiResponse[AiChatResponseData](data=data)


@router.get("/history", response_model=ApiResponse[AiChatHistoryData])
async def ai_history(
    session_id: str | None = Query(default=None, alias="sessionId", max_length=128),
    user: User = Depends(get_current_user),
    home: Home = Depends(get_active_home),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[AiChatHistoryData]:
    """读取小程序/云端 AI 对话历史。"""
    normalized, messages = await list_history(
        db,
        home_id=home.id,
        session_id=session_id,
    )
    return ApiResponse[AiChatHistoryData](
        data=AiChatHistoryData(session_id=normalized, messages=messages)
    )


@router.delete("/history", response_model=ApiResponse[AiChatHistoryClearData])
async def ai_history_clear(
    session_id: str | None = Query(default=None, alias="sessionId", max_length=128),
    user: User = Depends(get_current_user),
    home: Home = Depends(get_active_home),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[AiChatHistoryClearData]:
    """清空小程序/云端 AI 对话历史。"""
    normalized, deleted_count = await clear_history(
        db,
        home_id=home.id,
        session_id=session_id,
    )
    await db.commit()
    return ApiResponse[AiChatHistoryClearData](
        data=AiChatHistoryClearData(
            session_id=normalized,
            deleted_count=deleted_count,
        )
    )


__all__ = ["router"]
