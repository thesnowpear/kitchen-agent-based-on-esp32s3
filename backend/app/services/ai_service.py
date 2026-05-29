"""AI 对话服务：设备转发为主，云端 SiliconFlow 降级。

链路：
1. 查当前 home 的活跃设备 + 库存/临期摘要作为上下文；
2. 有 active device → MQTT 转发 ai_chat 命令到设备，等 cmd_ack（最多 settings.ai_device_timeout_seconds）；
3. 无 active device / 转发失败 / 超时 → 调 SiliconFlow OpenAI 兼容文本接口降级。

设备端 fridge_mqtt_protocol.c 收到 ai_chat 命令后会调用本地 ai_client + ai_context，
回复通过 cmd_ack 的 message 字段送回（截断到 4 KB）。
"""

from __future__ import annotations

import asyncio
import json
import logging
from datetime import date, timedelta
from typing import Any
from uuid import UUID, uuid4

import httpx
from sqlalchemy import and_, select
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.config import settings
from app.models.device import Device, DeviceBinding
from app.models.inventory import InventoryItem
from app.services.mqtt_client import mqtt_client

logger = logging.getLogger(__name__)


# ---------- 上下文构造 ----------


async def _build_context_summary(home_id: UUID, db: AsyncSession) -> dict[str, Any]:
    """组装 AI 上下文：近期库存 + 临期清单 + 总数。

    设备端 ai_chat 收到的 context 是 JSON 字符串；云端降级时同样的结构会拼进 system prompt。
    """
    recent_q = (
        select(InventoryItem)
        .where(
            InventoryItem.home_id == home_id,
            InventoryItem.status == "active",
        )
        .order_by(InventoryItem.updated_at.desc())
        .limit(20)
    )
    recent_items = (await db.execute(recent_q)).scalars().all()

    today = date.today()
    cutoff = today + timedelta(days=3)
    expiring_q = (
        select(InventoryItem)
        .where(
            and_(
                InventoryItem.home_id == home_id,
                InventoryItem.status == "active",
                InventoryItem.expire_date.is_not(None),
                InventoryItem.expire_date <= cutoff,
            )
        )
        .order_by(InventoryItem.expire_date.asc())
        .limit(10)
    )
    expiring_items = (await db.execute(expiring_q)).scalars().all()

    def _serialize(item: InventoryItem) -> dict[str, Any]:
        return {
            "name": item.name,
            "quantity": float(item.quantity) if item.quantity is not None else None,
            "unit": item.unit,
            "zone": item.zone,
            "slot": item.slot,
            "expireDate": item.expire_date.isoformat() if item.expire_date else None,
        }

    return {
        "recentItems": [_serialize(i) for i in recent_items],
        "expiringSoon": [_serialize(i) for i in expiring_items],
        "totalCount": len(recent_items),
    }


async def _resolve_active_device(home_id: UUID, db: AsyncSession) -> Device | None:
    """找当前 home 第一台 active 绑定的设备。"""
    q = (
        select(Device)
        .join(DeviceBinding, DeviceBinding.device_id == Device.id)
        .where(
            DeviceBinding.home_id == home_id,
            DeviceBinding.status == "active",
        )
        .order_by(DeviceBinding.created_at.asc())
        .limit(1)
    )
    return (await db.execute(q)).scalar_one_or_none()


# ---------- 设备转发 ----------


async def chat_via_device(
    device_sn: str,
    prompt: str,
    context_summary: dict[str, Any],
    timeout: float,
) -> dict[str, Any] | None:
    """通过 MQTT 让设备 AI 回复。成功返 dict；超时/失败返 None 由调用方降级。"""
    request_id = uuid4().hex
    fut = mqtt_client.register_ack_waiter(request_id)
    try:
        await mqtt_client.publish_command(
            device_sn,
            "ai_chat",
            {
                "request_id": request_id,
                "prompt": prompt,
                # context 序列化为字符串，便于设备端 cJSON 直接当一个字段处理。
                "context": json.dumps(context_summary, ensure_ascii=False),
            },
        )
    except Exception as exc:  # noqa: BLE001
        mqtt_client.unregister_ack_waiter(request_id)
        logger.warning("chat_via_device publish failed: %s", exc)
        return None

    try:
        ack = await asyncio.wait_for(fut, timeout=timeout)
    except asyncio.TimeoutError:
        mqtt_client.unregister_ack_waiter(request_id)
        return None

    if not isinstance(ack, dict):
        return None
    if ack.get("message") == "command received by device":
        logger.info("device returned generic ai_chat ack; falling back to cloud")
        return None
    if not ack.get("ok", True):
        # 设备明确 ack 失败：把 message 当 reply 透传，让前端展示设备反馈。
        return {
            "source": "device",
            "reply": ack.get("message", ""),
            "fallbackReason": None,
            "modelUsed": None,
            "deviceSn": device_sn,
        }
    return {
        "source": "device",
        "reply": ack.get("message", ""),
        "fallbackReason": None,
        "modelUsed": None,
        "deviceSn": device_sn,
    }


# ---------- 云端降级 ----------


async def chat_via_cloud(
    prompt: str,
    context_summary: dict[str, Any],
    fallback_reason: str,
    ai_config: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """SiliconFlow OpenAI 兼容文本接口降级。

    ai_config 由 chat_with_context 调 ai_config_service.get_ai_config_full(db, home_id) 取得，
    含 chatModel / apiBaseUrl / apiKey；为 None 时回落 settings 默认（向后兼容老调用方）。

    无 key / 网络错误 / 模型出错都返 reply="" + 一条 fallbackReason，由路由层置 ok=False。
    """
    if ai_config is None:
        ai_config = {
            "apiBaseUrl": settings.siliconflow_base_url,
            "apiKey": settings.siliconflow_api_key or "",
            "chatModel": settings.chat_model,
        }
    api_key = ai_config.get("apiKey") or settings.siliconflow_api_key
    base_url = ai_config.get("apiBaseUrl") or settings.siliconflow_base_url
    chat_model = ai_config.get("chatModel") or settings.chat_model

    if not api_key:
        return {
            "source": "cloud_fallback",
            "reply": "",
            "fallbackReason": f"cloud_error: SILICONFLOW_API_KEY not set ({fallback_reason})",
            "modelUsed": None,
            "deviceSn": None,
        }

    system_prompt = (
        "你是《冰箱小精灵》的智能助手。用户的冰箱当前库存与临期信息以 JSON 给出，"
        "请基于这些数据回答用户问题，例如菜谱推荐、储存建议、临期提醒等。\n\n"
        f"当前库存摘要：{json.dumps(context_summary, ensure_ascii=False)}"
    )
    body = {
        "model": chat_model,
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": prompt},
        ],
        "temperature": 0.4,
        "max_tokens": 600,
    }
    url = f"{base_url.rstrip('/')}/chat/completions"
    headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }

    try:
        async with httpx.AsyncClient(timeout=30.0) as client:
            response = await client.post(url, json=body, headers=headers)
            response.raise_for_status()
            data = response.json()
    except httpx.HTTPStatusError as exc:
        return {
            "source": "cloud_fallback",
            "reply": "",
            "fallbackReason": f"cloud_error: HTTP {exc.response.status_code} ({fallback_reason})",
            "modelUsed": chat_model,
            "deviceSn": None,
        }
    except httpx.HTTPError as exc:
        return {
            "source": "cloud_fallback",
            "reply": "",
            "fallbackReason": f"cloud_error: {exc.__class__.__name__} ({fallback_reason})",
            "modelUsed": chat_model,
            "deviceSn": None,
        }
    except Exception as exc:  # noqa: BLE001
        return {
            "source": "cloud_fallback",
            "reply": "",
            "fallbackReason": f"cloud_error: {exc} ({fallback_reason})",
            "modelUsed": chat_model,
            "deviceSn": None,
        }

    try:
        reply = data["choices"][0]["message"]["content"]
    except (KeyError, IndexError, TypeError) as exc:
        return {
            "source": "cloud_fallback",
            "reply": "",
            "fallbackReason": f"cloud_error: bad response shape ({exc})",
            "modelUsed": chat_model,
            "deviceSn": None,
        }

    return {
        "source": "cloud_fallback",
        "reply": reply or "",
        "fallbackReason": fallback_reason,
        "modelUsed": chat_model,
        "deviceSn": None,
    }


# ---------- 路由入口 ----------


async def chat_with_context(prompt: str, home_id: UUID, db: AsyncSession) -> dict[str, Any]:
    """对外统一入口：先尝试设备转发，失败 / 无设备时云端降级。

    设备转发不需要 backend 知道 AI key（设备本地用自己 NVS 的 ai key）；
    云端降级才需要 key，从 SystemConfig（双向同步过的）或环境变量取。
    """
    context_summary = await _build_context_summary(home_id, db)
    device = await _resolve_active_device(home_id, db)

    if device is not None and mqtt_client.is_connected():
        result = await chat_via_device(
            device.device_sn,
            prompt,
            context_summary,
            timeout=float(settings.ai_device_timeout_seconds),
        )
        if result is not None:
            return result
        fallback_reason = "device_timeout"
    elif device is None:
        fallback_reason = "no_active_device"
    else:
        fallback_reason = "mqtt_disconnected"

    # 云端降级前先取 SystemConfig 里的 AI 配置（双向同步过的最新版本）。
    # 延迟 import 避免与 ai_config_service 循环依赖。
    from app.services.ai_config_service import get_ai_config_full

    ai_config = await get_ai_config_full(db, home_id)
    return await chat_via_cloud(prompt, context_summary, fallback_reason, ai_config=ai_config)


__all__ = [
    "chat_with_context",
    "chat_via_device",
    "chat_via_cloud",
]
