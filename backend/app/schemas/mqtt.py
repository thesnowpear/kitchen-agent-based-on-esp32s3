"""MQTT event and command schemas."""

from uuid import UUID

from pydantic import Field

from app.schemas.common import CamelModel, JsonDict


class MqttEventIngestRequest(CamelModel):
    """EMQX Webhook 或内部 MQTT 消费者把上报事件转 HTTP 喂进来时使用。"""

    device_sn: str = Field(min_length=1, max_length=80)
    event_type: str = Field(min_length=1, max_length=80)
    topic: str | None = Field(default=None, max_length=200)
    qos: int = Field(default=0, ge=0, le=2)
    payload: JsonDict = Field(default_factory=dict)


class MqttEventIngestResponse(CamelModel):
    ok: bool = True
    event_id: UUID


class MqttCommandPublishRequest(CamelModel):
    """下发设备命令：command 取值见 services 约定（ai_chat / inventory_refresh / ota_check 等）。"""

    device_sn: str = Field(min_length=1, max_length=80)
    command: str = Field(min_length=1, max_length=80)
    request_id: str | None = Field(default=None, max_length=128)
    payload: JsonDict = Field(default_factory=dict)
    qos: int = Field(default=1, ge=0, le=2)


class MqttCommandPublishResponse(CamelModel):
    ok: bool = True
    topic: str
    queued: bool = True
    request_id: str | None = None
