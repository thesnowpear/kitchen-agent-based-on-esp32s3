"""AI 聊天接口的请求/响应 schema。

外层走 ApiResponse[T] 统一壳，因此这里仅描述业务数据 AiChatResponseData。
"""

from pydantic import Field

from app.schemas.common import CamelModel


class AiChatRequest(CamelModel):
    """`/api/v1/ai/chat` 请求体。

    - prompt：用户输入的对话内容，必填；
    - session_id：预留字段，本期 backend 不持久化对话，所有上下文由设备/云端各自维护。
    """

    prompt: str = Field(min_length=1, max_length=2000)
    session_id: str | None = Field(default=None, max_length=128)


class AiChatResponseData(CamelModel):
    """AI 回复 data 字段。

    - source: 'device' 表示通过 MQTT 转发到冰箱贴的本地 AI 拿到的回复；
              'cloud_fallback' 表示降级到 SiliconFlow 云端 OpenAI 兼容接口；
    - reply: AI 回复正文，可能被截断到 4 KB；
    - fallback_reason: 仅在 source=cloud_fallback 时有意义，例如 'device_timeout' / 'no_active_device' / 'cloud_error: ...'；
    - model_used: 实际命中的云端模型名（设备转发时为 None）；
    - device_sn: 转发成功时返回设备 SN（云端降级时为 None），便于前端展示「来自设备」徽标。
    """

    source: str
    reply: str
    fallback_reason: str | None = None
    model_used: str | None = None
    device_sn: str | None = None


__all__ = ["AiChatRequest", "AiChatResponseData"]
