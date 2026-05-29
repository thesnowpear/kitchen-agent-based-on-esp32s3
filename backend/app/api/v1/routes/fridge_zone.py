"""冰箱分区配置路由。"""

from fastapi import APIRouter, Depends
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.deps import get_active_home, get_current_user
from app.db.session import get_db
from app.models.home import Home
from app.models.user import User
from app.schemas.common import ApiResponse
from app.schemas.fridge_zone import FridgeZoneListData, FridgeZoneUpdateRequest
from app.services.fridge_zone_service import get_fridge_zones, upsert_fridge_zones
from app.services.sync_device_bridge import push_cloud_snapshot_to_devices
from app.services.sync_service import get_status, record_server_event

router = APIRouter()


@router.get("/zones", response_model=ApiResponse[FridgeZoneListData])
async def read_fridge_zones(
    home: Home = Depends(get_active_home),
    db: AsyncSession = Depends(get_db),
    _user: User = Depends(get_current_user),
) -> ApiResponse[FridgeZoneListData]:
    """读取当前家庭的冰箱分区配置。"""
    data = await get_fridge_zones(db, home.id)
    return ApiResponse[FridgeZoneListData](data=data)


@router.put("/zones", response_model=ApiResponse[FridgeZoneListData])
async def write_fridge_zones(
    payload: FridgeZoneUpdateRequest,
    home: Home = Depends(get_active_home),
    db: AsyncSession = Depends(get_db),
    _user: User = Depends(get_current_user),
) -> ApiResponse[FridgeZoneListData]:
    """写入当前家庭的冰箱分区配置。"""
    data = await upsert_fridge_zones(db, home.id, payload.zones)
    await record_server_event(
        db,
        home_id=home.id,
        user_id=_user.id,
        domain="fridge_zones",
        op="replace",
        payload={"zones": [zone.model_dump(mode="json", by_alias=True) for zone in data.zones]},
    )
    await db.commit()
    await push_cloud_snapshot_to_devices(
        db,
        home_id=home.id,
        server_revision=(await get_status(db, home.id)).server_revision,
        domains={"inventory", "fridge_zones"},
        request_device_inventory=False,
    )
    return ApiResponse[FridgeZoneListData](data=data)
