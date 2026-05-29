"""`/api/v1/ai/config` 路由：AI 配置 GET / POST。

- GET：返回当前 home 的 AI 配置（不含明文 api_key，只给 preview）；
- POST：upsert + 通过 MQTT 推送给当前 home 的所有 active 设备。
"""

from __future__ import annotations

import logging

from fastapi import APIRouter, Depends
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.deps import get_active_home, get_current_user
from app.db.session import get_db
from app.models.device import Device, DeviceBinding
from app.models.home import Home
from app.models.user import User
from app.schemas.ai_config import AiConfigData, AiConfigUpdateRequest
from app.schemas.common import ApiResponse
from app.services.ai_config_service import (
    build_device_payload,
    get_ai_config,
    get_ai_config_full,
    upsert_ai_config,
)
from app.services.mqtt_client import mqtt_client
from app.services.sync_service import record_server_event

router = APIRouter()
logger = logging.getLogger(__name__)


@router.get("/config", response_model=ApiResponse[AiConfigData])
async def read_ai_config(
    user: User = Depends(get_current_user),
    home: Home = Depends(get_active_home),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[AiConfigData]:
    """读 AI 配置（不返回明文 api_key）。"""
    data = await get_ai_config(db, home.id)
    return ApiResponse[AiConfigData](data=data)


@router.post("/config", response_model=ApiResponse[AiConfigData])
async def write_ai_config(
    payload: AiConfigUpdateRequest,
    user: User = Depends(get_current_user),
    home: Home = Depends(get_active_home),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[AiConfigData]:
    """写 AI 配置 + MQTT 推送给该 home 所有 active 设备。

    推送失败不影响响应（设备可能离线），仅记日志；
    设备下次上线 publish retained ai_config 时 backend 会比对时间戳重新协调。
    """
    data = await upsert_ai_config(db, home.id, payload, source="miniapp")
    await record_server_event(
        db,
        home_id=home.id,
        user_id=user.id,
        domain="ai_config",
        op="upsert",
        payload=payload.model_dump(mode="json", exclude_none=True, by_alias=True),
    )
    await db.commit()

    # 推送给所有 active 设备：保证多设备场景配置一致。
    devices_q = await db.execute(
        select(Device)
        .join(DeviceBinding, DeviceBinding.device_id == Device.id)
        .where(
            DeviceBinding.home_id == home.id,
            DeviceBinding.status == "active",
        )
    )
    config_for_push = await get_ai_config_full(db, home.id)
    asr_payload = {
        "asrApiBaseUrl": config_for_push.get("asrApiBaseUrl"),
        "asrModel": config_for_push.get("asrModel"),
        "asrApiKey": config_for_push.get("asrApiKey", ""),
        "asrTimeoutMs": config_for_push.get("asrTimeoutMs"),
        "configUpdatedAt": config_for_push.get("configUpdatedAt", 0),
    }
    tts_payload = {
        "ttsApiBaseUrl": config_for_push.get("ttsApiBaseUrl"),
        "ttsModel": config_for_push.get("ttsModel"),
        "ttsVoice": config_for_push.get("ttsVoice"),
        "ttsApiKey": config_for_push.get("ttsApiKey", ""),
        "ttsTimeoutMs": config_for_push.get("ttsTimeoutMs"),
        "configUpdatedAt": config_for_push.get("configUpdatedAt", 0),
    }
    pushed = 0
    for device in devices_q.scalars().all():
        try:
            await mqtt_client.publish_command(
                device.device_sn,
                "ai_config_update",
                build_device_payload(config_for_push),
            )
            await mqtt_client.publish_command(
                device.device_sn,
                "asr_config_update",
                asr_payload,
            )
            await mqtt_client.publish_command(
                device.device_sn,
                "tts_config_update",
                tts_payload,
            )
            pushed += 1
        except Exception as exc:  # noqa: BLE001
            logger.warning(
                "ai_config_update push failed: device=%s err=%s",
                device.device_sn,
                exc,
            )
    logger.info(
        "ai_config write done: home=%s pushed_to_devices=%d", home.id, pushed
    )
    return ApiResponse[AiConfigData](
        data=data,
        message=f"updated, pushed to {pushed} device(s)",
    )


__all__ = ["router"]
