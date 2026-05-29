"""库存条目 / 事件相关的请求 / 响应 schema。

外层统一 ApiResponse[T] 包壳，因此这里业务模型不带 ok 字段。
新增 InventoryListData / RefreshData / BindRequest 兼容旧路径。
"""

from datetime import date, datetime
from uuid import UUID

from pydantic import Field

from app.schemas.common import CamelModel, JsonDict


class InventoryItemSchema(CamelModel):
    """库存条目：zone/slot 是 ui-reference 引入的结构化位置；location 仍保留为可读文本。"""

    id: UUID
    name: str
    category: str | None = None
    quantity: float
    unit: str
    zone: str | None = None
    slot: str | None = None
    location: str | None = None
    expire_date: date | None = None
    status: str
    source: str
    confidence: int | None = None
    updated_at: datetime | None = None


class InventoryListData(CamelModel):
    """新版 GET /inventory 的 data 字段。

    保持和 ReminderListData / DeviceListResponse 一致的命名约定 items。
    """

    items: list[InventoryItemSchema]


class InventoryListResponse(CamelModel):
    """旧版列表响应壳（保留以避免破坏 import；新代码改用 ApiResponse[InventoryListData]）。"""

    items: list[InventoryItemSchema]


class InventoryUpdateRequest(CamelModel):
    """新增或更新库存：zone/slot 任一为空表示沿用旧值或随后由位置推荐器补齐。

    home_id 在新版路由里由 Depends(get_active_home) 推断，请求体可以不传；
    保留字段是为了让旧 POST /inventory/update 兼容路径不破。
    """

    home_id: UUID | None = None
    item_id: UUID | None = None
    device_id: UUID | None = None
    # 名称是少数几个真正必填的字段；其它字段尽量给默认值，方便手动添加场景。
    name: str = Field(min_length=1, max_length=120)
    category: str | None = Field(default=None, max_length=80)
    quantity: float = Field(default=1, ge=0)
    unit: str = Field(default="份", min_length=1, max_length=24)
    zone: str | None = Field(default=None, max_length=32)
    slot: str | None = Field(default=None, max_length=8)
    location: str | None = Field(default=None, max_length=80)
    expire_date: date | None = None
    status: str = Field(default="active", max_length=32)
    source: str = Field(default="manual", max_length=32)
    confidence: int | None = Field(default=None, ge=0, le=100)
    extra: JsonDict = Field(default_factory=dict)


class InventoryPatchRequest(CamelModel):
    """PUT /inventory/{item_id}：全部字段可选，未传则保留原值。"""

    name: str | None = Field(default=None, min_length=1, max_length=120)
    category: str | None = Field(default=None, max_length=80)
    quantity: float | None = Field(default=None, ge=0)
    unit: str | None = Field(default=None, min_length=1, max_length=24)
    zone: str | None = Field(default=None, max_length=32)
    slot: str | None = Field(default=None, max_length=8)
    location: str | None = Field(default=None, max_length=80)
    expire_date: date | None = None
    status: str | None = Field(default=None, max_length=32)
    source: str | None = Field(default=None, max_length=32)
    confidence: int | None = Field(default=None, ge=0, le=100)
    extra: JsonDict | None = None


class InventoryUpdateResponse(CamelModel):
    """旧版 update 接口响应壳（保留以避免破坏 import）。"""

    item_id: UUID


class InventoryEventRequest(CamelModel):
    """库存事件载荷，event_type 见 services 内部约定（inventory.update/scan/refresh 等）。"""

    home_id: UUID
    device_id: UUID | None = None
    item_id: UUID | None = None
    event_type: str = Field(min_length=1, max_length=64)
    actor_type: str = Field(default="user", max_length=32)
    actor_id: str | None = Field(default=None, max_length=128)
    version: int = Field(default=1, ge=1)
    payload: JsonDict = Field(default_factory=dict)


class InventoryEventResponse(CamelModel):
    """旧版 event 接口响应壳（保留以避免破坏 import）。"""

    event_id: UUID


class RefreshData(CamelModel):
    """POST /inventory/refresh 的 data 字段。

    - queued：是否真的把刷新命令推进了下游（MQTT 客户端未接入时仅返回 false）。
    - nextRefreshAt：服务端建议的下次刷新时刻（可为 None，由小程序自行计时）。
    """

    queued: bool = False
    next_refresh_at: datetime | None = None
