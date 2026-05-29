"""三端数据备份与同步 API schema。"""

from datetime import datetime
from uuid import UUID

from pydantic import Field

from app.schemas.ai_config import AiConfigData
from app.schemas.common import CamelModel, JsonDict
from app.schemas.fridge_zone import FridgeZoneConfig
from app.schemas.inventory import InventoryItemSchema
from app.schemas.reminder import ReminderSchema


class SyncEventSchema(CamelModel):
    """服务端返回给小程序/设备的同步事件。"""

    id: UUID
    client_event_id: str
    domain: str
    op: str
    source: str
    server_revision: int
    client_revision: int | None = None
    device_sn: str | None = None
    payload: JsonDict = Field(default_factory=dict)
    created_at: datetime | None = None


class SyncPushEvent(CamelModel):
    """客户端上报的一条同步事件。"""

    client_event_id: str = Field(min_length=1, max_length=128)
    domain: str = Field(min_length=1, max_length=64)
    op: str = Field(min_length=1, max_length=64)
    source: str = Field(default="miniapp", max_length=32)
    device_sn: str | None = Field(default=None, max_length=80)
    client_revision: int | None = Field(default=None, ge=0)
    payload: JsonDict = Field(default_factory=dict)


class SyncPushRequest(CamelModel):
    """POST /sync/push 请求体。"""

    events: list[SyncPushEvent] = Field(default_factory=list, max_length=50)


class SyncPushData(CamelModel):
    """POST /sync/push 返回数据。"""

    server_revision: int
    accepted: int
    duplicates: int
    events: list[SyncEventSchema] = Field(default_factory=list)


class SyncDevicePushRequest(CamelModel):
    """POST /sync/device-push 请求体。

    domains 为空时表示把当前云端快照中设备已支持的域全部下发。
    request_device_inventory 为 true 时会先要求设备上报本地库存快照，便于手动同步时把设备侧改动带回云端。
    accept_clean_device_snapshot 仅用于初次设备种子同步：允许后端接收 dirty=false 的设备快照。
    push_cloud_snapshot=false 时只请求设备上报，不立刻下发云端快照，避免初次同步时旧云端数据抢先覆盖设备。
    """

    domains: list[str] = Field(default_factory=list)
    request_device_inventory: bool = True
    accept_clean_device_snapshot: bool = False
    push_cloud_snapshot: bool = True


class SyncDevicePushData(CamelModel):
    """POST /sync/device-push 返回数据。"""

    device_count: int
    command_count: int
    queued_count: int
    domains: list[str] = Field(default_factory=list)
    errors: list[str] = Field(default_factory=list)


class SyncStatusData(CamelModel):
    """GET /sync/status 返回当前家庭同步摘要。"""

    server_revision: int
    last_synced_at: datetime | None = None
    device_revision: int | None = None
    pending_events: int = 0
    domains: dict[str, int] = Field(default_factory=dict)
    mqtt_connected: bool = False
    mqtt_broker: str | None = None


class ShoppingListData(CamelModel):
    """云端备份的购物清单文档。"""

    items: list[JsonDict] = Field(default_factory=list)
    updated_at: datetime | None = None


class RecipeCacheData(CamelModel):
    """云端备份的菜谱缓存文档。"""

    items: list[JsonDict] = Field(default_factory=list)
    updated_at: datetime | None = None


class AiHistorySnapshot(CamelModel):
    """云端 AI 历史快照。"""

    session_id: str
    messages: list[JsonDict] = Field(default_factory=list)


class SyncSnapshotData(CamelModel):
    """GET /sync/snapshot 返回可直接覆盖本地缓存的快照。"""

    server_revision: int
    generated_at: datetime
    inventory: list[InventoryItemSchema] = Field(default_factory=list)
    fridge_zones: list[FridgeZoneConfig] = Field(default_factory=list)
    ai_config: AiConfigData | None = None
    reminders: list[ReminderSchema] = Field(default_factory=list)
    shopping_list: ShoppingListData = Field(default_factory=ShoppingListData)
    recipe_cache: RecipeCacheData = Field(default_factory=RecipeCacheData)
    settings: JsonDict = Field(default_factory=dict)
    ai_history: AiHistorySnapshot | None = None


class SyncPullData(CamelModel):
    """GET /sync/pull 返回增量事件和当前快照。"""

    server_revision: int
    events: list[SyncEventSchema] = Field(default_factory=list)
    snapshot: SyncSnapshotData | None = None
