"""三端数据备份与同步路由。"""

from fastapi import APIRouter, Depends, Query
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.deps import get_active_home, get_current_user
from app.db.session import get_db
from app.models.home import Home
from app.models.user import User
from app.schemas.common import ApiResponse
from app.schemas.sync import (
    SyncDevicePushData,
    SyncDevicePushRequest,
    SyncPullData,
    SyncPushData,
    SyncPushRequest,
    SyncSnapshotData,
    SyncStatusData,
)
from app.services.sync_device_bridge import push_cloud_snapshot_to_devices
from app.services.sync_service import build_snapshot, get_status, pull_changes, push_events

router = APIRouter()


@router.get("/status", response_model=ApiResponse[SyncStatusData])
async def sync_status(
    home: Home = Depends(get_active_home),
    db: AsyncSession = Depends(get_db),
    _user: User = Depends(get_current_user),
) -> ApiResponse[SyncStatusData]:
    """读取当前家庭同步状态。"""
    return ApiResponse[SyncStatusData](data=await get_status(db, home.id))


@router.get("/snapshot", response_model=ApiResponse[SyncSnapshotData])
async def sync_snapshot(
    home: Home = Depends(get_active_home),
    db: AsyncSession = Depends(get_db),
    user: User = Depends(get_current_user),
) -> ApiResponse[SyncSnapshotData]:
    """读取当前家庭完整云端备份快照。"""
    return ApiResponse[SyncSnapshotData](data=await build_snapshot(db, home=home, user=user))


@router.get("/pull", response_model=ApiResponse[SyncPullData])
async def sync_pull(
    since_revision: int = Query(default=0, ge=0, alias="sinceRevision"),
    home: Home = Depends(get_active_home),
    db: AsyncSession = Depends(get_db),
    user: User = Depends(get_current_user),
) -> ApiResponse[SyncPullData]:
    """按 serverRevision 拉取增量事件，并附带最新快照。"""
    return ApiResponse[SyncPullData](data=await pull_changes(db, home=home, user=user, since_revision=since_revision))


@router.post("/push", response_model=ApiResponse[SyncPushData])
async def sync_push(
    payload: SyncPushRequest,
    home: Home = Depends(get_active_home),
    db: AsyncSession = Depends(get_db),
    user: User = Depends(get_current_user),
) -> ApiResponse[SyncPushData]:
    """批量上报本地变更事件。"""
    data = await push_events(db, home=home, user=user, events=payload.events)
    changed_domains = {event.domain for event in data.events if event.source != "device"}
    device_domains = set()
    if changed_domains & {"inventory", "fridge_zones"}:
        device_domains.update({"inventory", "fridge_zones"})
    if "ai_config" in changed_domains:
        device_domains.update({"ai_config", "asr_config", "tts_config"})
    if changed_domains & {"shopping_list", "recipe_cache", "reminder", "settings"}:
        device_domains.update(changed_domains & {"shopping_list", "recipe_cache", "reminder", "settings"})
    if device_domains:
        await push_cloud_snapshot_to_devices(
            db,
            home_id=home.id,
            server_revision=data.server_revision,
            domains=device_domains,
            request_device_inventory=False,
        )
    return ApiResponse[SyncPushData](data=data)


@router.post("/device-push", response_model=ApiResponse[SyncDevicePushData])
async def sync_device_push(
    payload: SyncDevicePushRequest,
    home: Home = Depends(get_active_home),
    db: AsyncSession = Depends(get_db),
    _user: User = Depends(get_current_user),
) -> ApiResponse[SyncDevicePushData]:
    """把当前云端快照主动下发给绑定设备。

    小程序“立即同步”会调用此接口：先请求设备上报本地库存，再把云端最新库存、
    分区和 AI/ASR/TTS 配置推到设备端 NVS/LittleFS。
    """
    status = await get_status(db, home.id)
    data = await push_cloud_snapshot_to_devices(
        db,
        home_id=home.id,
        server_revision=status.server_revision,
        domains=set(payload.domains) if payload.domains else None,
        request_device_inventory=payload.request_device_inventory,
        accept_clean_device_snapshot=payload.accept_clean_device_snapshot,
        push_cloud_snapshot=payload.push_cloud_snapshot,
    )
    message = "device sync queued" if data.queued_count else "no device command queued"
    return ApiResponse[SyncDevicePushData](data=data, message=message)
