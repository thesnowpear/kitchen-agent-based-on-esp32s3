"""旧版设备路由（device，单数前缀），保留作为兼容 alias。

新版 RESTful 路径见 routes/devices_alias.py（复数前缀 /devices）。
这里所有路径都保留旧行为，不强行要求 Authorization。
"""

from datetime import datetime, timezone
from uuid import UUID

from fastapi import APIRouter, Depends, HTTPException, Query
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.db.session import get_db
from app.models.device import Device, DeviceBinding
from app.schemas.common import ApiResponse
from app.schemas.device import (
    DeviceBindRequest,
    DeviceListResponse,
    DeviceStatusResponse,
    DeviceSummary,
    DeviceUnbindRequest,
)

router = APIRouter()


# 将一台 Device ORM 实例渲染为对外暴露的 DeviceSummary，集中一处避免重复字段映射。
def _to_summary(device: Device) -> DeviceSummary:
    return DeviceSummary(
        id=device.id,
        device_sn=device.device_sn,
        name=device.name,
        model=device.model,
        firmware_version=device.firmware_version,
        status=device.status,
        last_seen_at=device.last_seen_at,
    )


@router.post("/bind", response_model=ApiResponse[DeviceSummary])
async def device_bind(
    payload: DeviceBindRequest,
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[DeviceSummary]:
    """旧版绑定：显式传 user_id / home_id / device_sn；若设备未注册则先 upsert。"""
    # 设备主表 upsert：旧 demo 设备可能尚未在 devices 表里，按 device_sn 找不到就建。
    result = await db.execute(select(Device).where(Device.device_sn == payload.device_sn))
    device = result.scalar_one_or_none()
    if device is None:
        device = Device(
            device_sn=payload.device_sn,
            name=payload.name,
            model=payload.model,
            status="offline",
        )
        db.add(device)
        await db.flush()

    # 绑定关系也要 upsert：同一 device + home 已有 active 绑定时跳过插入，幂等。
    binding_result = await db.execute(
        select(DeviceBinding).where(
            DeviceBinding.device_id == device.id,
            DeviceBinding.home_id == payload.home_id,
            DeviceBinding.status == "active",
        )
    )
    if binding_result.scalar_one_or_none() is None:
        db.add(
            DeviceBinding(
                device_id=device.id,
                home_id=payload.home_id,
                bound_by_user_id=payload.user_id,
            )
        )

    await db.commit()
    return ApiResponse[DeviceSummary](data=_to_summary(device))


@router.post("/unbind", response_model=ApiResponse[None])
async def device_unbind(
    payload: DeviceUnbindRequest,
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[None]:
    """解绑：保留历史关系，把 status 置 inactive 并打 unbound_at 时间戳。"""
    query = select(DeviceBinding).where(
        DeviceBinding.home_id == payload.home_id,
        DeviceBinding.status == "active",
    )
    # device_id / device_sn 二选一：直接给 device_id 时跳过 devices 表查询。
    if payload.device_id:
        query = query.where(DeviceBinding.device_id == payload.device_id)
    elif payload.device_sn:
        device = (
            await db.execute(select(Device).where(Device.device_sn == payload.device_sn))
        ).scalar_one_or_none()
        if device is None:
            raise HTTPException(status_code=404, detail="device not found")
        query = query.where(DeviceBinding.device_id == device.id)
    else:
        raise HTTPException(status_code=400, detail="device_id or device_sn is required")

    binding = (await db.execute(query)).scalar_one_or_none()
    if binding is None:
        raise HTTPException(status_code=404, detail="active binding not found")

    binding.status = "inactive"
    binding.unbound_at = datetime.now(timezone.utc)
    await db.commit()
    return ApiResponse[None](message="device unbound")


@router.get("/list", response_model=ApiResponse[DeviceListResponse])
async def device_list(
    home_id: UUID = Query(...),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[DeviceListResponse]:
    """按家庭列出已绑定设备，按 device.created_at desc 排。"""
    result = await db.execute(
        select(Device)
        .join(DeviceBinding, DeviceBinding.device_id == Device.id)
        .where(DeviceBinding.home_id == home_id, DeviceBinding.status == "active")
        .order_by(Device.created_at.desc())
    )
    items = [_to_summary(item) for item in result.scalars().all()]
    return ApiResponse[DeviceListResponse](data=DeviceListResponse(items=items))


@router.get("/status", response_model=ApiResponse[DeviceStatusResponse])
async def device_status(
    device_sn: str = Query(...),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[DeviceStatusResponse]:
    """按 SN 查单台设备最近状态。"""
    device = (
        await db.execute(select(Device).where(Device.device_sn == device_sn))
    ).scalar_one_or_none()
    if device is None:
        raise HTTPException(status_code=404, detail="device not found")
    data = DeviceStatusResponse(
        device_sn=device.device_sn,
        status=device.status,
        last_seen_at=device.last_seen_at,
        firmware_version=device.firmware_version,
        extra=device.extra or {},
    )
    return ApiResponse[DeviceStatusResponse](data=data)
