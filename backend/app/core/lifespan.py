"""Application lifecycle hooks.

启动 / 关闭钩子集中在这里：
1. 本地开发期可选自动建表（AUTO_CREATE_TABLES）；
2. 本地开发期可选种入 demo 用户 / 家庭 / 设备 / 样本库存，应付比赛"一键演示"；
3. 启动 MQTT 客户端连接 EMQX（失败仅日志、不阻塞 API）；
4. 应用关闭时优雅断开 MQTT。
"""

from __future__ import annotations

import logging
from collections.abc import AsyncIterator
from contextlib import asynccontextmanager
from datetime import date, datetime, timedelta, timezone
from hashlib import sha256
from uuid import UUID, uuid4

from fastapi import FastAPI
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

import app.models  # noqa: F401 -- 让 SQLAlchemy 发现所有 model
from app.core.config import settings
from app.db.base import Base
from app.db.session import AsyncSessionLocal, engine
from app.models.device import Device, DeviceBinding
from app.models.home import Home, HomeMember
from app.models.inventory import InventoryItem
from app.models.user import User
from app.services.mqtt_client import shutdown_mqtt_client, startup_mqtt_client

logger = logging.getLogger(__name__)


# 演示用固定标识：在 backend 启动种子里写死，
# 小程序绑定页"一键绑定演示设备"按钮也用同一个 device_sn。
DEMO_DEVICE_SN = "DEMO-FRIDGE-001"
DEMO_USER_OPENID = "demo_openid_seed_001"


async def _seed_demo_data(db: AsyncSession) -> None:
    """仅在 app_env=local 且当前数据空时插入 demo 数据。

    包含：1 个 demo 用户 + 1 个家庭 + 1 台设备（active 绑定）+ 4 条样本库存（覆盖 4 个 zone）。
    幂等：检测到任何条件已存在则跳过对应步骤，反复重启 backend 不会重复种入。
    """
    if settings.app_env != "local":
        return

    # ---------- 1. demo user ----------
    user_q = await db.execute(select(User).where(User.primary_openid == DEMO_USER_OPENID))
    demo_user = user_q.scalar_one_or_none()
    if demo_user is None:
        demo_user = User(
            primary_openid=DEMO_USER_OPENID,
            display_name="演示用户",
        )
        db.add(demo_user)
        await db.flush()
        logger.info("seed: created demo user %s", demo_user.id)

    # ---------- 2. demo home ----------
    home_q = await db.execute(
        select(Home).where(Home.owner_user_id == demo_user.id).limit(1)
    )
    demo_home = home_q.scalar_one_or_none()
    if demo_home is None:
        demo_home = Home(name="演示厨房", owner_user_id=demo_user.id)
        db.add(demo_home)
        await db.flush()
        db.add(HomeMember(home_id=demo_home.id, user_id=demo_user.id, role="owner"))
        logger.info("seed: created demo home %s", demo_home.id)

    # ---------- 3. demo device + active binding ----------
    device_q = await db.execute(
        select(Device).where(Device.device_sn == DEMO_DEVICE_SN)
    )
    demo_device = device_q.scalar_one_or_none()
    if demo_device is None:
        demo_device = Device(
            device_sn=DEMO_DEVICE_SN,
            name="演示冰箱",
            model="DEMO",
            firmware_version="seed",
            status="offline",
        )
        db.add(demo_device)
        await db.flush()
        logger.info("seed: created demo device %s", demo_device.id)

    binding_q = await db.execute(
        select(DeviceBinding).where(
            DeviceBinding.device_id == demo_device.id,
            DeviceBinding.home_id == demo_home.id,
            DeviceBinding.status == "active",
        )
    )
    if binding_q.scalar_one_or_none() is None:
        db.add(
            DeviceBinding(
                device_id=demo_device.id,
                home_id=demo_home.id,
                bound_by_user_id=demo_user.id,
                status="active",
            )
        )
        logger.info("seed: created active binding for demo device")

    # ---------- 4. demo inventory (4 zones × 1 sample each) ----------
    inv_q = await db.execute(
        select(InventoryItem.id).where(InventoryItem.home_id == demo_home.id).limit(1)
    )
    if inv_q.scalar_one_or_none() is None:
        today = date.today()
        samples = [
            # (name, category, quantity, unit, zone, slot, days_to_expire, location)
            ("速冻水饺", "速冻", 12, "个", "freezer", "B2", 30, "上层冷冻 中·中"),
            ("番茄", "蔬菜", 2, "个", "left", "B2", 3, "左侧冷藏 中·中"),
            ("酸奶", "乳制品", 2, "盒", "right", "A1", 5, "右侧冷藏 内·左"),
            ("牛奶", "饮品", 1, "盒", "door", "C1", 1, "门架 外·左"),
        ]
        for name, category, qty, unit, zone, slot, days, location in samples:
            db.add(
                InventoryItem(
                    home_id=demo_home.id,
                    device_id=demo_device.id,
                    name=name,
                    category=category,
                    quantity=qty,
                    unit=unit,
                    zone=zone,
                    slot=slot,
                    location=location,
                    expire_date=today + timedelta(days=days),
                    status="active",
                    source="seed",
                )
            )
        logger.info("seed: created 4 sample inventory items")

    await db.commit()


@asynccontextmanager
async def lifespan(app: FastAPI) -> AsyncIterator[None]:
    """统一 startup / shutdown 钩子。

    每个步骤都用 try/except 兜底：种子失败不应阻塞 API，MQTT 失败也只是降级到云端 AI。
    """
    # ---- 1. 自动建表（开发期可选） ----
    if settings.auto_create_tables:
        try:
            async with engine.begin() as conn:
                await conn.run_sync(Base.metadata.create_all)
            logger.info("lifespan: auto_create_tables done")
        except Exception:  # noqa: BLE001
            logger.exception("lifespan: auto_create_tables failed (ignored)")

    # ---- 2. demo 种子（仅 local） ----
    try:
        async with AsyncSessionLocal() as db:
            await _seed_demo_data(db)
    except Exception:  # noqa: BLE001
        logger.exception("lifespan: seed demo data failed (ignored)")

    # ---- 3. 启动 MQTT 客户端 ----
    await startup_mqtt_client(app)

    yield

    # ---- 4. 关闭 MQTT 客户端 ----
    await shutdown_mqtt_client(app)
