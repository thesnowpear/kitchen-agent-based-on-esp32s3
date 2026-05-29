"""家庭相关路由：

- POST /home/create：旧版显式建家庭（保留兼容）。
- GET /home/overview：新版首屏聚合接口，一次返回设备、库存计数、临期清单、提醒数。

外层统一 ApiResponse[T] 包壳。
"""

from datetime import date, datetime, timedelta
from uuid import UUID

from fastapi import APIRouter, Depends
from sqlalchemy import func, select
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.deps import get_active_home, get_current_user
from app.db.session import get_db
from app.models.device import Device, DeviceBinding
from app.models.home import Home, HomeMember
from app.models.inventory import InventoryItem
from app.models.reminder import Reminder
from app.models.user import User
from app.schemas.common import ApiResponse
from app.schemas.device import DeviceSummary
from app.schemas.home import HomeCreateRequest, HomeCreateResponse, HomeOverview
from app.schemas.inventory import InventoryItemSchema

router = APIRouter()


# 临期阈值：到期日 <= 今天 + 3 天即视为临期。
_EXPIRING_DAYS = 3
# 临期列表在 overview 中最多吐 5 条，避免 payload 膨胀；完整列表走 GET /inventory。
_EXPIRING_LIST_LIMIT = 5


@router.post("/create", response_model=ApiResponse[HomeCreateResponse])
async def home_create(
    payload: HomeCreateRequest,
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[HomeCreateResponse]:
    """显式创建家庭空间，并把创建人写入 owner 成员关系。"""
    home = Home(name=payload.name, owner_user_id=payload.user_id)
    db.add(home)
    await db.flush()
    db.add(HomeMember(home_id=home.id, user_id=payload.user_id, role="owner"))
    await db.commit()
    return ApiResponse[HomeCreateResponse](
        data=HomeCreateResponse(home_id=home.id, name=home.name),
    )


@router.get("/overview", response_model=ApiResponse[HomeOverview])
async def home_overview(
    home: Home = Depends(get_active_home),
    db: AsyncSession = Depends(get_db),
    # _user 仅用于触发鉴权链：未登录请求会在 deps 层被 401 拦掉。
    _user: User = Depends(get_current_user),
) -> ApiResponse[HomeOverview]:
    """首屏概览：聚合"主设备 + 库存计数 + 临期列表 + 提醒数 + 最近同步时间"。

    所有子查询都走当前活跃家庭范围；多家庭支持本期不做。
    """
    # 1) 主设备（按 DeviceBinding.created_at asc 排第一）。
    device_row = await db.execute(
        select(Device)
        .join(DeviceBinding, DeviceBinding.device_id == Device.id)
        .where(DeviceBinding.home_id == home.id, DeviceBinding.status == "active")
        .order_by(DeviceBinding.created_at.asc())
        .limit(1)
    )
    primary_device = device_row.scalar_one_or_none()
    device_summary: DeviceSummary | None = None
    if primary_device is not None:
        device_summary = DeviceSummary(
            id=primary_device.id,
            device_sn=primary_device.device_sn,
            name=primary_device.name,
            model=primary_device.model,
            firmware_version=primary_device.firmware_version,
            status=primary_device.status,
            last_seen_at=primary_device.last_seen_at,
        )

    # 2) 库存数量（仅 status != 'deleted'）。
    inventory_count = (
        await db.execute(
            select(func.count(InventoryItem.id)).where(
                InventoryItem.home_id == home.id,
                InventoryItem.status != "deleted",
            )
        )
    ).scalar_one()

    # 3) 临期阈值：到期日 <= 今天 + 3 天 且 status='active'。
    today = date.today()
    expiring_threshold = today + timedelta(days=_EXPIRING_DAYS)
    expiring_count = (
        await db.execute(
            select(func.count(InventoryItem.id)).where(
                InventoryItem.home_id == home.id,
                InventoryItem.status == "active",
                InventoryItem.expire_date.is_not(None),
                InventoryItem.expire_date <= expiring_threshold,
            )
        )
    ).scalar_one()

    # 4) 临期清单：取前 5 条，按到期日 asc。
    expiring_rows = await db.execute(
        select(InventoryItem)
        .where(
            InventoryItem.home_id == home.id,
            InventoryItem.status == "active",
            InventoryItem.expire_date.is_not(None),
            InventoryItem.expire_date <= expiring_threshold,
        )
        .order_by(InventoryItem.expire_date.asc())
        .limit(_EXPIRING_LIST_LIMIT)
    )
    expiring_list = [
        InventoryItemSchema.model_validate(item) for item in expiring_rows.scalars().all()
    ]

    # 5) 待办提醒数：status='pending'。
    pending_reminder_count = (
        await db.execute(
            select(func.count(Reminder.id)).where(
                Reminder.home_id == home.id,
                Reminder.status == "pending",
            )
        )
    ).scalar_one()

    # 6) 最近同步时间：本家庭所有设备 last_seen_at 的最大值（无设备 / 全 NULL → None）。
    last_sync_row = await db.execute(
        select(func.max(Device.last_seen_at))
        .join(DeviceBinding, DeviceBinding.device_id == Device.id)
        .where(DeviceBinding.home_id == home.id, DeviceBinding.status == "active")
    )
    last_sync_at: datetime | None = last_sync_row.scalar_one_or_none()

    overview = HomeOverview(
        device=device_summary,
        inventory_count=int(inventory_count or 0),
        expiring_count=int(expiring_count or 0),
        pending_reminder_count=int(pending_reminder_count or 0),
        expiring_list=expiring_list,
        last_sync_at=last_sync_at,
    )
    return ApiResponse[HomeOverview](data=overview)


# 让外部 import 名称稳定；UUID 仅用于类型注解，未使用时静默忽略。
_ = UUID
