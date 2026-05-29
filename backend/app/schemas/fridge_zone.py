"""冰箱分区配置 schema。

分区配置属于家庭级 UI/库存配置。标准分区固定保留，自定义分区使用 custom_* key，
这样库存表现有 zone 字段无需迁移即可引用。
"""

from pydantic import Field

from app.schemas.common import CamelModel


class FridgeZoneConfig(CamelModel):
    """单个冰箱分区配置。"""

    key: str = Field(min_length=1, max_length=32)
    label: str = Field(min_length=1, max_length=32)
    hint: str = Field(default="", max_length=64)
    custom: bool = False
    width: int = Field(default=1, ge=1, le=3)
    height: int = Field(default=1, ge=1, le=3)


class FridgeZoneListData(CamelModel):
    """GET /fridge/zones 的 data 字段。"""

    zones: list[FridgeZoneConfig]
    config_updated_at: str | None = None
    source: str = "default"


class FridgeZoneUpdateRequest(CamelModel):
    """PUT /fridge/zones 请求体。"""

    zones: list[FridgeZoneConfig]


__all__ = [
    "FridgeZoneConfig",
    "FridgeZoneListData",
    "FridgeZoneUpdateRequest",
]
