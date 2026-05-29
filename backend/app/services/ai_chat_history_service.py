"""AI 对话历史服务层。"""

from __future__ import annotations

from uuid import UUID

from sqlalchemy import delete, select
from sqlalchemy.ext.asyncio import AsyncSession

from app.models.ai_chat import AiChatMessage
from app.schemas.ai_chat_history import AiChatHistoryMessage

DEFAULT_SESSION_ID = "miniapp-default"
MAX_HISTORY_MESSAGES = 80


def normalize_session_id(session_id: str | None) -> str:
    """规范化 sessionId，避免空值和过长值污染索引。"""
    value = (session_id or DEFAULT_SESSION_ID).strip()
    if not value:
        return DEFAULT_SESSION_ID
    return value[:128]


def to_history_message(row: AiChatMessage) -> AiChatHistoryMessage:
    """ORM → API schema。"""
    return AiChatHistoryMessage(
        id=row.id,
        role=row.role,
        content=row.content,
        source=row.source,
        fallback_reason=row.fallback_reason,
        model_used=row.model_used,
        device_sn=row.device_sn,
        sent_at=row.created_at,
    )


async def list_history(
    db: AsyncSession,
    *,
    home_id: UUID,
    session_id: str | None,
    limit: int = MAX_HISTORY_MESSAGES,
) -> tuple[str, list[AiChatHistoryMessage]]:
    """读取一个家庭下某个 session 的最近对话，按时间正序返回。"""
    normalized = normalize_session_id(session_id)
    stmt = (
        select(AiChatMessage)
        .where(
            AiChatMessage.home_id == home_id,
            AiChatMessage.session_id == normalized,
        )
        .order_by(AiChatMessage.created_at.desc())
        .limit(limit)
    )
    rows = list((await db.execute(stmt)).scalars().all())
    rows.reverse()
    return normalized, [to_history_message(row) for row in rows]


async def append_turn(
    db: AsyncSession,
    *,
    home_id: UUID,
    user_id: UUID | None,
    session_id: str | None,
    prompt: str,
    result: dict,
) -> str:
    """原子追加一轮 user/assistant 消息。"""
    normalized = normalize_session_id(session_id)
    db.add(
        AiChatMessage(
            home_id=home_id,
            user_id=user_id,
            session_id=normalized,
            role="user",
            content=prompt,
            source="miniapp",
            extra={},
        )
    )
    db.add(
        AiChatMessage(
            home_id=home_id,
            user_id=user_id,
            session_id=normalized,
            role="assistant",
            content=result.get("reply") or "",
            source=result.get("source"),
            fallback_reason=result.get("fallbackReason"),
            model_used=result.get("modelUsed"),
            device_sn=result.get("deviceSn"),
            extra={},
        )
    )
    return normalized


async def clear_history(
    db: AsyncSession,
    *,
    home_id: UUID,
    session_id: str | None,
) -> tuple[str, int]:
    """清空指定 session 的云端历史。"""
    normalized = normalize_session_id(session_id)
    result = await db.execute(
        delete(AiChatMessage).where(
            AiChatMessage.home_id == home_id,
            AiChatMessage.session_id == normalized,
        )
    )
    return normalized, int(result.rowcount or 0)


__all__ = [
    "DEFAULT_SESSION_ID",
    "append_turn",
    "clear_history",
    "list_history",
    "normalize_session_id",
]
