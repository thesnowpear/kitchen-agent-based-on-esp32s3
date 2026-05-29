"""AI 配置双向同步的业务服务层。

数据流：
- 小程序 POST /ai/config → upsert_ai_config(source='miniapp') → 写 SystemConfig 表 →
  路由层根据 build_device_payload() 通过 MQTT 推给设备；
- 设备 retained publish ai_config → merge_from_device() → 比对 config_updated_at →
  谁新谁覆盖；若 backend 更新则反推；
- vision_service / ai_service 调 get_ai_config_full() 取当前 active config（含明文 key）。

存储字段：
- apiBaseUrl, apiKey, chatModel, visionModel, systemPrompt, timeoutMs, profileName
- visionModel 仅 backend 用；推送给设备时会被 build_device_payload 过滤掉。
- asrApiBaseUrl/asrModel/asrApiKey/asrTimeoutMs 与 ttsApiBaseUrl/ttsModel/ttsVoice/ttsApiKey/ttsTimeoutMs
  先随 SystemConfig 持久化，设备端后续通过专用 ASR/TTS 配置命令应用。

回落顺序：
1. SystemConfig 行里的最新 ai_config；
2. settings.siliconflow_* 环境变量。
"""

from __future__ import annotations

import logging
from datetime import datetime, timezone
from typing import Any
from uuid import UUID

from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.config import settings
from app.models.system_config import SystemConfig
from app.schemas.ai_config import AiConfigData, AiConfigUpdateRequest, _make_preview

logger = logging.getLogger(__name__)

# SystemConfig.config_key 的取值常量。
AI_CONFIG_KEY = "ai_config"

# AI config value 的字段白名单：避免设备 / 小程序往里塞奇怪键，影响 schema 演进。
_ALLOWED_VALUE_KEYS = {
    "apiBaseUrl",
    "apiKey",
    "chatModel",
    "visionModel",
    "systemPrompt",
    "timeoutMs",
    "profileName",
    "asrApiBaseUrl",
    "asrModel",
    "asrApiKey",
    "asrTimeoutMs",
    "ttsApiBaseUrl",
    "ttsModel",
    "ttsVoice",
    "ttsApiKey",
    "ttsTimeoutMs",
    "source",  # 最近写入来源（miniapp / device），便于审计
}

# 推送给设备 NVS 的字段：visionModel 不在内，设备本地用不到。
_DEVICE_PUSH_KEYS = {
    "apiBaseUrl",
    "apiKey",
    "chatModel",
    "systemPrompt",
    "timeoutMs",
    "profileName",
}


def _now() -> datetime:
    return datetime.now(timezone.utc)


def _normalize_value(raw: dict[str, Any] | None) -> dict[str, Any]:
    """裁剪到白名单字段，保证 schema 稳定；空 dict / None 统一返回 {}。"""
    if not isinstance(raw, dict):
        return {}
    return {k: v for k, v in raw.items() if k in _ALLOWED_VALUE_KEYS}


async def _load_row(db: AsyncSession, home_id: UUID) -> SystemConfig | None:
    result = await db.execute(
        select(SystemConfig).where(
            SystemConfig.home_id == home_id,
            SystemConfig.config_key == AI_CONFIG_KEY,
        )
    )
    return result.scalar_one_or_none()


def _to_data(row: SystemConfig | None) -> AiConfigData:
    """ORM 行 → 对外 AiConfigData（不带明文 api_key）。"""
    if row is None:
        # 没有 SystemConfig 行：回落到环境变量。
        env_key = settings.siliconflow_api_key
        return AiConfigData(
            api_base_url=settings.siliconflow_base_url,
            has_api_key=bool(env_key),
            api_key_preview=_make_preview(env_key),
            chat_model=settings.chat_model,
            vision_model=settings.vision_model,
            system_prompt=None,
            timeout_ms=30000,
            profile_name="default",
            asr_api_base_url="https://api.siliconflow.cn/v1/audio/transcriptions",
            asr_model="TeleAI/TeleSpeechASR",
            asr_timeout_ms=45000,
            asr_has_api_key=bool(env_key),
            asr_api_key_preview=_make_preview(env_key),
            tts_api_base_url="https://api.siliconflow.cn/v1/audio/speech",
            tts_model="fnlp/MOSS-TTSD-v0.5",
            tts_voice="fnlp/MOSS-TTSD-v0.5:alex",
            tts_timeout_ms=45000,
            tts_has_api_key=bool(env_key),
            tts_api_key_preview=_make_preview(env_key),
            config_updated_at=None,
            source="env_fallback",
        )
    value = _normalize_value(row.value)
    api_key = value.get("apiKey") or None
    asr_key = value.get("asrApiKey") or api_key
    tts_key = value.get("ttsApiKey") or api_key
    return AiConfigData(
        api_base_url=value.get("apiBaseUrl"),
        has_api_key=bool(api_key),
        api_key_preview=_make_preview(api_key),
        chat_model=value.get("chatModel"),
        vision_model=value.get("visionModel"),
        system_prompt=value.get("systemPrompt"),
        timeout_ms=int(value.get("timeoutMs") or 30000),
        profile_name=value.get("profileName") or "default",
        asr_api_base_url=value.get("asrApiBaseUrl") or "https://api.siliconflow.cn/v1/audio/transcriptions",
        asr_model=value.get("asrModel") or "TeleAI/TeleSpeechASR",
        asr_timeout_ms=int(value.get("asrTimeoutMs") or 45000),
        asr_has_api_key=bool(asr_key),
        asr_api_key_preview=_make_preview(asr_key),
        tts_api_base_url=value.get("ttsApiBaseUrl") or "https://api.siliconflow.cn/v1/audio/speech",
        tts_model=value.get("ttsModel") or "fnlp/MOSS-TTSD-v0.5",
        tts_voice=value.get("ttsVoice") or "fnlp/MOSS-TTSD-v0.5:alex",
        tts_timeout_ms=int(value.get("ttsTimeoutMs") or 45000),
        tts_has_api_key=bool(tts_key),
        tts_api_key_preview=_make_preview(tts_key),
        config_updated_at=row.config_updated_at,
        source=value.get("source") or "miniapp",
    )


async def get_ai_config(db: AsyncSession, home_id: UUID) -> AiConfigData:
    """对外接口：返回不含明文 api_key 的展示视图。"""
    row = await _load_row(db, home_id)
    return _to_data(row)


async def get_ai_config_full(db: AsyncSession, home_id: UUID) -> dict[str, Any]:
    """内部接口：返回**含明文 api_key** 的完整配置（含 visionModel）。

    用于 vision_service / ai_service 取实际调用模型 + key，以及 MQTT 推送时构造 payload。
    回落策略：SystemConfig 行 > settings.siliconflow_* env。
    """
    row = await _load_row(db, home_id)
    if row is not None:
        value = _normalize_value(row.value)
        return {
            "apiBaseUrl": value.get("apiBaseUrl") or settings.siliconflow_base_url,
            "apiKey": value.get("apiKey") or settings.siliconflow_api_key or "",
            "chatModel": value.get("chatModel") or settings.chat_model,
            "visionModel": value.get("visionModel") or settings.vision_model,
            "systemPrompt": value.get("systemPrompt") or "",
            "timeoutMs": int(value.get("timeoutMs") or 30000),
            "profileName": value.get("profileName") or "default",
            "asrApiBaseUrl": value.get("asrApiBaseUrl") or "https://api.siliconflow.cn/v1/audio/transcriptions",
            "asrModel": value.get("asrModel") or "TeleAI/TeleSpeechASR",
            "asrApiKey": value.get("asrApiKey") or value.get("apiKey") or settings.siliconflow_api_key or "",
            "asrTimeoutMs": int(value.get("asrTimeoutMs") or 45000),
            "ttsApiBaseUrl": value.get("ttsApiBaseUrl") or "https://api.siliconflow.cn/v1/audio/speech",
            "ttsModel": value.get("ttsModel") or "fnlp/MOSS-TTSD-v0.5",
            "ttsVoice": value.get("ttsVoice") or "fnlp/MOSS-TTSD-v0.5:alex",
            "ttsApiKey": value.get("ttsApiKey") or value.get("apiKey") or settings.siliconflow_api_key or "",
            "ttsTimeoutMs": int(value.get("ttsTimeoutMs") or 45000),
            "configUpdatedAt": int(row.config_updated_at.timestamp() * 1000),
            "source": value.get("source") or "miniapp",
        }
    # 没有行：回落 env 默认。configUpdatedAt=0 表示「从未配置」，
    # 设备 NVS 里非 0 的 timestamp 都会胜过这个值，避免 env 覆盖设备已有配置。
    return {
        "apiBaseUrl": settings.siliconflow_base_url,
        "apiKey": settings.siliconflow_api_key or "",
        "chatModel": settings.chat_model,
        "visionModel": settings.vision_model,
        "systemPrompt": "",
        "timeoutMs": 30000,
        "profileName": "default",
        "asrApiBaseUrl": "https://api.siliconflow.cn/v1/audio/transcriptions",
        "asrModel": "TeleAI/TeleSpeechASR",
        "asrApiKey": settings.siliconflow_api_key or "",
        "asrTimeoutMs": 45000,
        "ttsApiBaseUrl": "https://api.siliconflow.cn/v1/audio/speech",
        "ttsModel": "fnlp/MOSS-TTSD-v0.5",
        "ttsVoice": "fnlp/MOSS-TTSD-v0.5:alex",
        "ttsApiKey": settings.siliconflow_api_key or "",
        "ttsTimeoutMs": 45000,
        "configUpdatedAt": 0,
        "source": "env_fallback",
    }


def build_device_payload(full_config: dict[str, Any]) -> dict[str, Any]:
    """从 get_ai_config_full 输出里筛出推送给设备 NVS 的字段（剥掉 visionModel 等）。"""
    payload = {k: v for k, v in full_config.items() if k in _DEVICE_PUSH_KEYS}
    payload["configUpdatedAt"] = full_config.get("configUpdatedAt", 0)
    return payload


async def upsert_ai_config(
    db: AsyncSession,
    home_id: UUID,
    update: AiConfigUpdateRequest,
    source: str = "miniapp",
    config_updated_at: datetime | None = None,
) -> AiConfigData:
    """小程序 / 设备同步入口共用。

    - 任一字段为 None 表示沿用旧值；apiKey 显式传 "" 表示清空。
    - config_updated_at 由调用方传入（设备同步路径用设备的时间戳；小程序路径用 now）。
    - 返回新值（无明文 api_key）。
    """
    row = await _load_row(db, home_id)
    if row is None:
        merged: dict[str, Any] = {}
    else:
        merged = dict(_normalize_value(row.value))

    update_dict = update.model_dump(exclude_none=True, by_alias=True)
    # 显式空字符串 apiKey 走「清空」路径；by_alias 后键名已是 camelCase。
    if "apiKey" in update_dict and update_dict["apiKey"] == "":
        merged.pop("apiKey", None)
        update_dict.pop("apiKey", None)
    if "asrApiKey" in update_dict and update_dict["asrApiKey"] == "":
        merged.pop("asrApiKey", None)
        update_dict.pop("asrApiKey", None)
    if "ttsApiKey" in update_dict and update_dict["ttsApiKey"] == "":
        merged.pop("ttsApiKey", None)
        update_dict.pop("ttsApiKey", None)
    merged.update(update_dict)
    merged["source"] = source

    final_updated_at = config_updated_at or _now()

    if row is None:
        row = SystemConfig(
            home_id=home_id,
            config_key=AI_CONFIG_KEY,
            value=merged,
            config_updated_at=final_updated_at,
        )
        db.add(row)
    else:
        row.value = merged
        row.config_updated_at = final_updated_at

    await db.commit()
    await db.refresh(row)
    logger.info(
        "ai_config upserted: home=%s source=%s updated_at=%s has_key=%s",
        home_id,
        source,
        final_updated_at.isoformat(),
        bool(merged.get("apiKey")),
    )
    return _to_data(row)


async def merge_from_device(
    db: AsyncSession,
    home_id: UUID,
    device_payload: dict[str, Any],
) -> tuple[str, dict[str, Any] | None]:
    """处理设备 retained ai_config 上报。

    Return：
    - ("noop", None)              ：两端 timestamp 相等或都为 0；
    - ("updated_from_device", None)：设备 ts > backend，已经写入 backend；
    - ("should_push_to_device", payload)：backend ts > 设备，调用方应通过 MQTT 把 payload 推给设备。
    """
    device_ts_raw = device_payload.get("configUpdatedAt") or device_payload.get(
        "config_updated_at"
    )
    try:
        device_ts_ms = int(device_ts_raw) if device_ts_raw is not None else 0
    except (TypeError, ValueError):
        device_ts_ms = 0

    row = await _load_row(db, home_id)
    backend_ts_ms = (
        int(row.config_updated_at.timestamp() * 1000) if row is not None else 0
    )

    # ---- 设备时间戳更大：用设备的覆盖 backend ----
    if device_ts_ms > backend_ts_ms:
        def pick(camel: str, snake: str) -> Any:
            if camel in device_payload:
                return device_payload[camel]
            return device_payload.get(snake)

        api_key_value = pick("apiKey", "api_key")
        # 设备只携带 chat 用的 model；不会写 visionModel（backend 保留自己的）。
        update = AiConfigUpdateRequest(
            api_base_url=pick("apiBaseUrl", "api_base_url"),
            api_key=api_key_value if api_key_value is not None else None,
            chat_model=pick("chatModel", "model"),  # 设备 NVS 里只叫 model
            vision_model=None,  # 不动 backend 的 visionModel
            system_prompt=pick("systemPrompt", "system_prompt"),
            timeout_ms=pick("timeoutMs", "timeout_ms"),
            profile_name=pick("profileName", "profile_name"),
        )
        device_dt = datetime.fromtimestamp(device_ts_ms / 1000.0, tz=timezone.utc)
        await upsert_ai_config(
            db,
            home_id=home_id,
            update=update,
            source="device",
            config_updated_at=device_dt,
        )
        return ("updated_from_device", None)

    # ---- backend 时间戳更大：把 backend 的反推给设备 ----
    if backend_ts_ms > device_ts_ms:
        full_config = await get_ai_config_full(db, home_id)
        return ("should_push_to_device", build_device_payload(full_config))

    # ---- 一致 / 都为 0：什么都不做 ----
    return ("noop", None)


__all__ = [
    "AI_CONFIG_KEY",
    "build_device_payload",
    "get_ai_config",
    "get_ai_config_full",
    "merge_from_device",
    "upsert_ai_config",
]
