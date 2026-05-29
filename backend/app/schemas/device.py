"""设备相关的请求 / 响应 schema。

外层统一 ApiResponse[T] 包壳，因此这里业务模型不带 ok 字段。
新增 BindRequest（小程序绑定页用的 bindCode 简化版）。
"""

from datetime import datetime
from uuid import UUID

from pydantic import Field

from app.schemas.common import CamelModel, JsonDict


class DeviceBindRequest(CamelModel):
    """旧版绑定请求：保留以兼容现有调用方。

    新版小程序应改走 /devices/bind + BindRequest。
    """

    user_id: UUID
    home_id: UUID
    device_sn: str = Field(min_length=1, max_length=80)
    name: str | None = Field(default=None, max_length=80)
    model: str | None = Field(default=None, max_length=80)


class DeviceUnbindRequest(CamelModel):
    """解绑请求，device_id / device_sn 二选一。"""

    user_id: UUID
    home_id: UUID
    device_id: UUID | None = None
    device_sn: str | None = Field(default=None, max_length=80)


class DeviceSummary(CamelModel):
    """设备摘要：用于列表 / primary / 绑定结果展示。"""

    id: UUID
    device_sn: str
    name: str | None = None
    model: str | None = None
    firmware_version: str | None = None
    status: str
    last_seen_at: datetime | None = None


class DeviceListResponse(CamelModel):
    """旧版列表响应体（保留 alias，但不再带 ok 字段）。"""

    items: list[DeviceSummary]


class DeviceStatusResponse(CamelModel):
    """设备状态详情，extra 透传 MQTT 上报的扩展字段。"""

    device_sn: str
    status: str
    last_seen_at: datetime | None = None
    firmware_version: str | None = None
    extra: JsonDict = Field(default_factory=dict)


class BindRequest(CamelModel):
    """新版 /devices/bind 用的请求体。

    bindCode：用户在小程序里输入或扫描得到的设备绑定码；
    不区分大小写，"DEMO" 视为一键演示设备。
    """

    bind_code: str = Field(min_length=1, max_length=80)
