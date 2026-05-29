"""家庭空间的请求 / 响应 schema。

外层统一 ApiResponse[T] 包壳，因此这里的业务模型不带 ok 字段。
新增 HomeOverview，给小程序首屏一次性吐回设备摘要 + 库存计数 + 临期列表 + 待办提醒数。
"""

from datetime import datetime
from uuid import UUID

from pydantic import Field

from app.schemas.common import CamelModel
from app.schemas.device import DeviceSummary
from app.schemas.inventory import InventoryItemSchema


class HomeCreateRequest(CamelModel):
    """显式创建家庭空间的请求体。"""

    user_id: UUID
    name: str = Field(default="我的冰箱", min_length=1, max_length=80)


class HomeCreateResponse(CamelModel):
    """创建家庭后返回 home_id 和名称（旧版兼容，无 ok 字段）。"""

    home_id: UUID
    name: str


class HomeOverview(CamelModel):
    """首屏概览：一次返回小程序"主页"需要的全部聚合数据。

    - device：当前家庭排第一的活跃绑定设备，无设备时为 None（小程序据此引导绑定）。
    - inventoryCount：未删除库存条目数量。
    - expiringCount：临期条目数量（expire_date <= today+3 且 status='active'）。
    - pendingReminderCount：未处理提醒数量（status='pending'）。
    - expiringList：临期条目前 5 条，按到期日 asc。
    - lastSyncAt：本家庭下所有设备 last_seen_at 的最大值，作为"最近同步时间"。
    """

    device: DeviceSummary | None = None
    inventory_count: int = 0
    expiring_count: int = 0
    pending_reminder_count: int = 0
    expiring_list: list[InventoryItemSchema] = Field(default_factory=list)
    last_sync_at: datetime | None = None
