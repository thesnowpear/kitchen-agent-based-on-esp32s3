"""Application settings loaded from environment variables."""

from functools import lru_cache

from pydantic import Field, field_validator
from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    """集中管理服务配置，真实密钥只允许通过环境变量注入。"""

    model_config = SettingsConfigDict(env_file=".env", env_file_encoding="utf-8", extra="ignore")

    app_name: str = "fridge-spirit-backend"
    app_env: str = "local"
    app_debug: bool = True
    api_v1_prefix: str = "/api/v1"

    database_url: str = "postgresql+asyncpg://fridge:fridge_dev_password@localhost:5432/fridge_spirit"
    auto_create_tables: bool = False

    wx_appid: str | None = None
    wx_secret: str | None = None
    wx_code2session_url: str = "https://api.weixin.qq.com/sns/jscode2session"

    # MQTT 经纪人配置：
    # - mqtt_topic_prefix 必须与设备端 components/mqtt_protocol/fridge_mqtt_protocol.c
    #   写死的前缀 "fridge" 保持一致；若小程序/后端这里改了，设备消息会收不到。
    # - mqtt_keepalive_seconds 与设备端 FRIDGE_MQTT_DEFAULT_KEEPALIVE_SECONDS 对齐即可。
    mqtt_broker_host: str = "localhost"
    mqtt_broker_port: int = 1883
    mqtt_username: str | None = None
    mqtt_password: str | None = None
    mqtt_client_id: str = "fridge-spirit-backend"
    mqtt_topic_prefix: str = "fridge"
    mqtt_keepalive_seconds: int = 60

    jwt_secret: str = Field(default="change-me-in-local-dev", min_length=8)
    access_token_ttl_seconds: int = 604800
    cors_origins: list[str] = ["http://localhost:5173", "http://localhost:8080"]

    # ====== SiliconFlow / 视觉识别相关 ======
    # 默认走 SiliconFlow 的 OpenAI 兼容路径；如果切到其它服务商，覆盖 base_url 即可。
    # 注意：firmware 端 ai_client 也在调 SiliconFlow，但 key 各自独立配置，互不影响。
    siliconflow_base_url: str = "https://api.siliconflow.cn/v1"
    # key 未配置时 /inventory/scan 与 /ai/chat 云端降级都会返 ApiResponse(ok=False, ...)，不抛 5xx，便于小程序友好提示。
    siliconflow_api_key: str | None = None
    # 视觉模型：用于 /inventory/scan 多模态识别。
    vision_model: str = "Qwen/Qwen2-VL-7B-Instruct"
    # 文本对话模型：用于 /ai/chat 设备转发超时后的云端降级。
    chat_model: str = "Qwen/Qwen2.5-32B-Instruct"
    # 上传图片大小上限（字节），超出直接拒绝，避免 OOM / 长尾请求。
    inventory_scan_max_bytes: int = 5 * 1024 * 1024
    # AI 设备转发等待 cmd_ack 的超时上限；超时即触发云端降级。
    ai_device_timeout_seconds: int = 30

    @field_validator("cors_origins", mode="before")
    @classmethod
    def parse_cors_origins(cls, value: str | list[str]) -> list[str]:
        """兼容逗号分隔环境变量，避免本地 .env 写法太重。"""
        if isinstance(value, str):
            return [item.strip() for item in value.split(",") if item.strip()]
        return value


@lru_cache
def get_settings() -> Settings:
    return Settings()


settings = get_settings()
