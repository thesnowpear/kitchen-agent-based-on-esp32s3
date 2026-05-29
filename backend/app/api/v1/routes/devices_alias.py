"""新版 /devices 路由（复数前缀，RESTful + 当前用户活跃家庭）。

设计要点：
- 全部走 Depends(get_current_user) + Depends(get_active_home)，
  小程序不需要再显式传 user_id / home_id；活跃家庭由 token 推断（无家庭时自动创建）。
- /devices/primary 用于首屏：拿当前家庭排第一的 active 设备；无设备时优雅返回 data=None
  而不是 404，避免小程序一上来就抛错。
- /devices/bind 收 bindCode；"DEMO"（不分大小写）走演示设备一键绑定，
  否则把 bindCode 当 device_sn upsert。
"""

from fastapi import APIRouter, Depends
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.deps import get_active_home, get_current_user
from app.db.session import get_db
from app.models.device import Device, DeviceBinding
from app.models.home import Home
from app.models.user import User
from app.schemas.common import ApiResponse
from app.schemas.device import BindRequest, DeviceSummary

router = APIRouter()


# 演示模式专用：bindCode 写 "DEMO" 时自动落地的设备 SN。
# 与本仓库 doc/项目架构... 里的设备命名保持一致，方便后续 MQTT 联调时直接复用。
_DEMO_BIND_CODE = "DEMO"
_DEMO_DEVICE_SN = "DEMO-FRIDGE-001"
_DEMO_DEVICE_NAME = "演示冰箱"
_DEMO_DEVICE_MODEL = "DEMO"


def _to_summary(device: Device) -> DeviceSummary:
    """ORM → DTO 渲染（与 device.py 内部使用的是同一字段集）。"""
    return DeviceSummary(
        id=device.id,
        device_sn=device.device_sn,
        name=device.name,
        model=device.model,
        firmware_version=device.firmware_version,
        status=device.status,
        last_seen_at=device.last_seen_at,
    )


@router.get("/primary", response_model=ApiResponse[DeviceSummary | None])
async def devices_primary(
    home: Home = Depends(get_active_home),
    db: AsyncSession = Depends(get_db),
    # _user 仅用于触发 Depends 链，确保未鉴权请求被 401 拦在 deps 层。
    _user: User = Depends(get_current_user),
) -> ApiResponse[DeviceSummary | None]:
    """当前家庭排第一的活跃绑定设备；无设备时 data=None，不抛 404。"""
    # 按 DeviceBinding.created_at asc 排序：先绑的设备视为"主"设备。
    result = await db.execute(
        select(Device)
        .join(DeviceBinding, DeviceBinding.device_id == Device.id)
        .where(DeviceBinding.home_id == home.id, DeviceBinding.status == "active")
        .order_by(DeviceBinding.created_at.asc())
        .limit(1)
    )
    device = result.scalar_one_or_none()
    if device is None:
        # 让小程序首屏拿到 data=None 优雅渲染"未绑定设备"卡片。
        return ApiResponse[DeviceSummary | None](data=None, message="no active device")
    return ApiResponse[DeviceSummary | None](data=_to_summary(device))


@router.post("/bind", response_model=ApiResponse[DeviceSummary])
async def devices_bind(
    payload: BindRequest,
    home: Home = Depends(get_active_home),
    db: AsyncSession = Depends(get_db),
    user: User = Depends(get_current_user),
) -> ApiResponse[DeviceSummary]:
    """绑定设备到当前活跃家庭。

    规则：
    - bindCode 不区分大小写；upper() == "DEMO" 走一键演示设备路径。
    - 否则把 bindCode 当 device_sn 处理：找不到设备时 upsert 一台 status=offline 的设备。
    - DeviceBinding 同 (device_id, home_id, status='active') 已存在则幂等跳过插入。
    """
    bind_code_upper = payload.bind_code.strip().upper()

    # 推导实际要绑的 device_sn / name / model：DEMO 模式下用固定演示设备信息。
    if bind_code_upper == _DEMO_BIND_CODE:
        device_sn = _DEMO_DEVICE_SN
        default_name = _DEMO_DEVICE_NAME
        default_model = _DEMO_DEVICE_MODEL
    else:
        # 普通绑定：直接把用户输入当 SN 用；保留原大小写以便排查。
        device_sn = payload.bind_code.strip()
        default_name = None
        default_model = None

    # 1) upsert 设备主表。
    result = await db.execute(select(Device).where(Device.device_sn == device_sn))
    device = result.scalar_one_or_none()
    if device is None:
        device = Device(
            device_sn=device_sn,
            name=default_name,
            model=default_model,
            status="offline",
        )
        db.add(device)
        await db.flush()

    # 2) upsert 绑定关系：避免重复插入造成 UNIQUE 冲突。
    binding_row = await db.execute(
        select(DeviceBinding).where(
            DeviceBinding.device_id == device.id,
            DeviceBinding.home_id == home.id,
            DeviceBinding.status == "active",
        )
    )
    if binding_row.scalar_one_or_none() is None:
        db.add(
            DeviceBinding(
                device_id=device.id,
                home_id=home.id,
                bound_by_user_id=user.id,
            )
        )

    await db.commit()
    return ApiResponse[DeviceSummary](data=_to_summary(device))
