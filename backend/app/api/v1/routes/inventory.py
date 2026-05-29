"""库存相关路由（/inventory 单数前缀）。

新版 RESTful：
- GET    /inventory                  当前家庭未删除条目（可选 ?zone= 过滤）
- POST   /inventory                  新增 / 更新条目（home_id 走 token 推断）
- PUT    /inventory/{item_id}        编辑单条
- DELETE /inventory/{item_id}        软删（status='deleted'）
- POST   /inventory/refresh          触发设备重新上报库存（task #7 之前为 placeholder）

旧版兼容 alias（同一 router 内挂载，路径不变）：
- POST   /inventory/list             → 转发到 GET /inventory（注意：实际请求方法依然是 POST）
- POST   /inventory/update           → 老 update 路径
- POST   /inventory/event            → 老 event 路径

外层统一 ApiResponse[T] 包壳。
"""

from datetime import datetime, timedelta, timezone
from uuid import UUID

from fastapi import APIRouter, Depends, HTTPException, Query
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.deps import get_active_home, get_current_user
from app.db.session import get_db
from app.models.home import Home
from app.models.device import Device, DeviceBinding
from app.models.inventory import InventoryEvent, InventoryItem
from app.models.user import User
from app.schemas.common import ApiResponse
from app.schemas.inventory import (
    InventoryEventRequest,
    InventoryEventResponse,
    InventoryItemSchema,
    InventoryListData,
    InventoryListResponse,
    InventoryPatchRequest,
    InventoryUpdateRequest,
    InventoryUpdateResponse,
    RefreshData,
)
from app.services.sync_device_bridge import push_cloud_snapshot_to_devices
from app.services.sync_service import get_status, record_server_event

router = APIRouter()


# 把 ORM 实例渲染为对外的 schema。从 ORM 直读避免漏字段，集中在一处便于扩展。
def _item_to_schema(item: InventoryItem) -> InventoryItemSchema:
    return InventoryItemSchema(
        id=item.id,
        name=item.name,
        category=item.category,
        quantity=float(item.quantity) if item.quantity is not None else 0.0,
        unit=item.unit,
        zone=item.zone,
        slot=item.slot,
        location=item.location,
        expire_date=item.expire_date,
        status=item.status,
        source=item.source,
        confidence=item.confidence,
        updated_at=item.updated_at,
    )


# ============== 新版 RESTful 路径 ==============


@router.get("", response_model=ApiResponse[InventoryListData])
async def inventory_list_v2(
    zone: str | None = Query(default=None, max_length=32),
    home: Home = Depends(get_active_home),
    db: AsyncSession = Depends(get_db),
    _user: User = Depends(get_current_user),
) -> ApiResponse[InventoryListData]:
    """新版列表：当前活跃家庭未删除条目，按 updated_at desc。"""
    # 仅过滤 zone（前端的"区域 tab"）；zone=None 视为不过滤。
    stmt = select(InventoryItem).where(
        InventoryItem.home_id == home.id,
        InventoryItem.status != "deleted",
    )
    if zone is not None:
        stmt = stmt.where(InventoryItem.zone == zone)
    stmt = stmt.order_by(InventoryItem.updated_at.desc())
    result = await db.execute(stmt)
    items = [_item_to_schema(item) for item in result.scalars().all()]
    return ApiResponse[InventoryListData](data=InventoryListData(items=items))


@router.post("", response_model=ApiResponse[InventoryItemSchema])
async def inventory_create_v2(
    payload: InventoryUpdateRequest,
    home: Home = Depends(get_active_home),
    db: AsyncSession = Depends(get_db),
    _user: User = Depends(get_current_user),
) -> ApiResponse[InventoryItemSchema]:
    """新增或更新库存条目；home_id 优先用 token 推断的活跃家庭，请求体里的 home_id 仅作老兼容。"""
    home_id = home.id  # 始终以 token 推断的活跃家庭为准，避免越权写入别的家庭。
    item = await _upsert_item(db, payload, home_id=home_id)
    await record_server_event(
        db,
        home_id=home_id,
        user_id=_user.id,
        domain="inventory",
        op="upsert",
        payload=payload.model_dump(mode="json", by_alias=True),
    )
    await db.commit()
    await db.refresh(item)
    await push_cloud_snapshot_to_devices(
        db,
        home_id=home_id,
        server_revision=(await get_status(db, home_id)).server_revision,
        domains={"inventory", "fridge_zones"},
        request_device_inventory=False,
    )
    return ApiResponse[InventoryItemSchema](data=_item_to_schema(item))


@router.put("/{item_id}", response_model=ApiResponse[InventoryItemSchema])
async def inventory_patch_v2(
    item_id: UUID,
    payload: InventoryPatchRequest,
    home: Home = Depends(get_active_home),
    db: AsyncSession = Depends(get_db),
    _user: User = Depends(get_current_user),
) -> ApiResponse[InventoryItemSchema]:
    """部分更新一条库存：未传字段保留原值。"""
    item = await db.get(InventoryItem, item_id)
    if item is None or item.home_id != home.id:
        # 找不到或不属于当前家庭都视为 404，避免泄露其它家庭的存在性。
        raise HTTPException(status_code=404, detail="inventory item not found")

    # 只用 model_dump(exclude_unset=True) 拿用户真正传了的字段，避免把缺省 None 当成"清空"语义。
    patch_data = payload.model_dump(exclude_unset=True, by_alias=False)
    for key, value in patch_data.items():
        setattr(item, key, value)

    # 写一条变更事件，便于后续审计 / 同步到设备端。
    db.add(
        InventoryEvent(
            home_id=item.home_id,
            device_id=item.device_id,
            item_id=item.id,
            event_type="inventory.update",
            actor_type="user",
            payload=patch_data,
        )
    )
    await record_server_event(
        db,
        home_id=home.id,
        user_id=_user.id,
        domain="inventory",
        op="patch",
        payload={"id": str(item.id), **payload.model_dump(mode="json", exclude_unset=True, by_alias=True)},
    )

    await db.commit()
    await db.refresh(item)
    await push_cloud_snapshot_to_devices(
        db,
        home_id=home.id,
        server_revision=(await get_status(db, home.id)).server_revision,
        domains={"inventory", "fridge_zones"},
        request_device_inventory=False,
    )
    return ApiResponse[InventoryItemSchema](data=_item_to_schema(item))


@router.delete("/{item_id}", response_model=ApiResponse[None])
async def inventory_delete_v2(
    item_id: UUID,
    home: Home = Depends(get_active_home),
    db: AsyncSession = Depends(get_db),
    _user: User = Depends(get_current_user),
) -> ApiResponse[None]:
    """软删：把 status 置为 'deleted'，保留事件流，便于后续恢复 / 审计。"""
    item = await db.get(InventoryItem, item_id)
    if item is None or item.home_id != home.id:
        raise HTTPException(status_code=404, detail="inventory item not found")
    item.status = "deleted"
    db.add(
        InventoryEvent(
            home_id=item.home_id,
            device_id=item.device_id,
            item_id=item.id,
            event_type="inventory.delete",
            actor_type="user",
            payload={"status": "deleted"},
        )
    )
    await record_server_event(
        db,
        home_id=home.id,
        user_id=_user.id,
        domain="inventory",
        op="delete",
        payload={"id": str(item.id), "status": "deleted"},
    )
    await db.commit()
    await push_cloud_snapshot_to_devices(
        db,
        home_id=home.id,
        server_revision=(await get_status(db, home.id)).server_revision,
        domains={"inventory", "fridge_zones"},
        request_device_inventory=False,
    )
    return ApiResponse[None](message="item deleted")


@router.post("/refresh", response_model=ApiResponse[RefreshData])
async def inventory_refresh_v2(
    home: Home = Depends(get_active_home),
    db: AsyncSession = Depends(get_db),
    _user: User = Depends(get_current_user),
) -> ApiResponse[RefreshData]:
    """触发设备重新上报库存。

    若 MQTT 在线，会向当前家庭第一台 active 设备发送 inventory_refresh。
    设备端尚未实现该命令时只会通用 ACK；这里仅表示“云端已尝试下发”。
    """
    queued = False
    # 写一条事件以备审计；即便 MQTT 失败也保留请求轨迹。
    db.add(
        InventoryEvent(
            home_id=home.id,
            device_id=None,
            item_id=None,
            event_type="inventory.refresh",
            actor_type="user",
            payload={"home_id": str(home.id)},
        )
    )
    try:
        from app.services.mqtt_client import mqtt_client

        device = (
            await db.execute(
                select(Device)
                .join(DeviceBinding, DeviceBinding.device_id == Device.id)
                .where(
                    DeviceBinding.home_id == home.id,
                    DeviceBinding.status == "active",
                )
                .order_by(DeviceBinding.created_at.asc())
                .limit(1)
            )
        ).scalar_one_or_none()
        if device is not None and mqtt_client.is_connected():
            await mqtt_client.publish_command(
                device.device_sn,
                command="inventory_refresh",
                payload={"homeId": str(home.id)},
            )
            queued = True
    except Exception as exc:  # noqa: BLE001  本期就是要吃掉一切异常并降级
        # 用 print 避免引入额外 logger 依赖；正式接入 MQTT 时再换 logging。
        print(f"[inventory_refresh] mqtt placeholder skipped: {exc!r}")

    await db.commit()
    return ApiResponse[RefreshData](
        data=RefreshData(
            queued=queued,
            next_refresh_at=datetime.now(timezone.utc) + timedelta(seconds=60),
        ),
        message="refresh enqueued (mqtt placeholder)" if not queued else "refresh enqueued",
    )


# ============== 旧版兼容 alias（保持 import 不破） ==============


@router.get("/list", response_model=ApiResponse[InventoryListResponse])
async def inventory_list_legacy(
    home_id: UUID = Query(...),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[InventoryListResponse]:
    """旧版列表入口（显式传 home_id），保留兼容。"""
    result = await db.execute(
        select(InventoryItem)
        .where(InventoryItem.home_id == home_id, InventoryItem.status != "deleted")
        .order_by(InventoryItem.updated_at.desc())
    )
    items = [_item_to_schema(item) for item in result.scalars().all()]
    return ApiResponse[InventoryListResponse](data=InventoryListResponse(items=items))


@router.post("/update", response_model=ApiResponse[InventoryUpdateResponse])
async def inventory_update_legacy(
    payload: InventoryUpdateRequest,
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[InventoryUpdateResponse]:
    """旧版更新接口：home_id 必传；不走鉴权链以兼容老调用方。"""
    if payload.home_id is None:
        raise HTTPException(status_code=400, detail="home_id is required for legacy update")
    item = await _upsert_item(db, payload, home_id=payload.home_id)
    await db.commit()
    await db.refresh(item)
    return ApiResponse[InventoryUpdateResponse](
        data=InventoryUpdateResponse(item_id=item.id),
    )


@router.post("/event", response_model=ApiResponse[InventoryEventResponse])
async def inventory_event_legacy(
    payload: InventoryEventRequest,
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[InventoryEventResponse]:
    """旧版库存事件入口：照原样落库（后续可在这里做幂等键 / 版本冲突处理）。"""
    event = InventoryEvent(**payload.model_dump())
    db.add(event)
    await db.commit()
    return ApiResponse[InventoryEventResponse](
        data=InventoryEventResponse(event_id=event.id),
    )


# ============== 内部辅助 ==============


async def _upsert_item(
    db: AsyncSession,
    payload: InventoryUpdateRequest,
    *,
    home_id: UUID,
) -> InventoryItem:
    """新增或更新一条 InventoryItem，并写入对应 InventoryEvent。

    item_id 为空 → 新建；item_id 给了但找不到 → 也按新建处理（避免外部传错时直接报错）。
    """
    if payload.item_id:
        item = await db.get(InventoryItem, payload.item_id)
        if item is None or item.home_id != home_id:
            # 不归属当前家庭，视为新建（保留老路由行为）。
            item = InventoryItem(home_id=home_id, name=payload.name)
            db.add(item)
            await db.flush()
    else:
        item = InventoryItem(home_id=home_id, name=payload.name)
        db.add(item)
        await db.flush()

    # 全字段覆盖式更新；后端不维护"是否被用户改过"语义，前端按需选择 PUT /{item_id}。
    item.device_id = payload.device_id
    item.name = payload.name
    item.category = payload.category
    item.quantity = payload.quantity
    item.unit = payload.unit
    item.zone = payload.zone
    item.slot = payload.slot
    item.location = payload.location
    item.expire_date = payload.expire_date
    item.status = payload.status
    item.source = payload.source
    item.confidence = payload.confidence
    item.extra = payload.extra

    db.add(
        InventoryEvent(
            home_id=home_id,
            device_id=payload.device_id,
            item_id=item.id,
            event_type="inventory.update",
            actor_type="user",
            payload=payload.model_dump(mode="json"),
        )
    )
    return item
