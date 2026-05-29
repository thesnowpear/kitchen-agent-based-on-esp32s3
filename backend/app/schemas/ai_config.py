"""AI 配置双向同步的请求 / 响应 schema。

字段约定与设备端 fridge_ai_client.h 对齐 + backend 额外需要的 visionModel：
- apiBaseUrl ↔ api_base_url（≤ 160）
- apiKey    ↔ api_key（≤ 256，明文，最终通过 MQTT 传给设备 NVS）
- chatModel ↔ 设备端 model（≤ 64）——设备只跑 chat，vision 在后端
- visionModel ← 仅 backend 用（/inventory/scan）；设备端不感知，不会通过 MQTT 推送
- systemPrompt ↔ system_prompt（≤ 8192）
- timeoutMs ↔ timeout_ms
- profileName ↔ profile_name（≤ 32）
- asrApiBaseUrl / asrModel / asrApiKey / asrTimeoutMs ← 语音识别配置
- ttsApiBaseUrl / ttsModel / ttsVoice / ttsApiKey / ttsTimeoutMs ← 语音生成配置

GET 响应不回显明文 api_key，只给 has_api_key + apiKeyPreview；POST 允许传明文。
"""

from datetime import datetime

from pydantic import Field

from app.schemas.common import CamelModel


def _make_preview(api_key: str | None) -> str | None:
    """生成 api_key 的展示用预览：前 4 后 4，中间打码。"""
    if not api_key:
        return None
    if len(api_key) <= 8:
        return "*" * len(api_key)
    return f"{api_key[:4]}{'*' * 8}{api_key[-4:]}"


class AiConfigData(CamelModel):
    """AI 配置对外展示 schema（GET 响应 / POST 响应）。

    设计上不暴露 apiKey 明文：有 hasApiKey 标志 + apiKeyPreview 给小程序显示「已配置 xxxx****yyyy」。
    """

    api_base_url: str | None = None
    has_api_key: bool = False
    api_key_preview: str | None = None
    chat_model: str | None = None
    vision_model: str | None = None
    system_prompt: str | None = None
    timeout_ms: int = 30000
    profile_name: str = "default"
    asr_api_base_url: str | None = None
    asr_model: str | None = None
    asr_timeout_ms: int = 45000
    asr_has_api_key: bool = False
    asr_api_key_preview: str | None = None
    tts_api_base_url: str | None = None
    tts_model: str | None = None
    tts_voice: str | None = None
    tts_timeout_ms: int = 45000
    tts_has_api_key: bool = False
    tts_api_key_preview: str | None = None
    config_updated_at: datetime | None = None
    # 'miniapp' | 'device' | 'env_fallback'：让前端能展示「当前配置来源」。
    source: str = "env_fallback"


class AiConfigUpdateRequest(CamelModel):
    """小程序 POST /ai/config 请求体；所有字段都可选。

    apiKey 为空字符串「""」表示**清空 key**，与设备端 fridge_ai_client_clear_key() 语义对齐；
    apiKey 为 None 表示不修改。
    """

    api_base_url: str | None = Field(default=None, max_length=160)
    api_key: str | None = Field(default=None, max_length=256)
    chat_model: str | None = Field(default=None, max_length=64)
    vision_model: str | None = Field(default=None, max_length=64)
    system_prompt: str | None = Field(default=None, max_length=8192)
    timeout_ms: int | None = Field(default=None, ge=1000, le=300000)
    profile_name: str | None = Field(default=None, max_length=32)
    asr_api_base_url: str | None = Field(default=None, max_length=192)
    asr_model: str | None = Field(default=None, max_length=64)
    asr_api_key: str | None = Field(default=None, max_length=256)
    asr_timeout_ms: int | None = Field(default=None, ge=1000, le=300000)
    tts_api_base_url: str | None = Field(default=None, max_length=192)
    tts_model: str | None = Field(default=None, max_length=64)
    tts_voice: str | None = Field(default=None, max_length=32)
    tts_api_key: str | None = Field(default=None, max_length=256)
    tts_timeout_ms: int | None = Field(default=None, ge=1000, le=300000)


__all__ = ["AiConfigData", "AiConfigUpdateRequest", "_make_preview"]
