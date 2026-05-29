"""冰箱分区配置服务层。"""

from __future__ import annotations

from datetime import datetime, timezone
from typing import Any
from uuid import UUID

from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.models.system_config import SystemConfig
from app.schemas.fridge_zone import FridgeZoneConfig, FridgeZoneListData

FRIDGE_ZONES_KEY = "fridge_zones"

DEFAULT_ZONES = [
    FridgeZoneConfig(key="freezer", label="上层冷冻", hint="速冻 / 雪糕", custom=False, width=2, height=1),
    FridgeZoneConfig(key="left", label="左侧冷藏", hint="蔬菜 / 熟食", custom=False, width=1, height=2),
    FridgeZoneConfig(key="right", label="右侧冷藏", hint="肉类 / 乳制品", custom=False, width=1, height=2),
    FridgeZoneConfig(key="door", label="门架", hint="饮品 / 调料", custom=False, width=1, height=3),
]


def _now() -> datetime:
    return datetime.now(timezone.utc)


async def _load_row(db: AsyncSession, home_id: UUID) -> SystemConfig | None:
    result = await db.execute(
        select(SystemConfig).where(
            SystemConfig.home_id == home_id,
            SystemConfig.config_key == FRIDGE_ZONES_KEY,
        )
    )
    return result.scalar_one_or_none()


def _normalize_zone(zone: FridgeZoneConfig) -> FridgeZoneConfig:
    """收敛宽高和 custom 标记，防止前端缓存异常值污染云端配置。"""
    is_standard = zone.key in {"freezer", "left", "right", "door"}
    return FridgeZoneConfig(
        key=zone.key.strip()[:32],
        label=zone.label.strip()[:32],
        hint=(zone.hint or "").strip()[:64],
        custom=False if is_standard else bool(zone.custom),
        width=min(3, max(1, int(zone.width or 1))),
        height=min(3, max(1, int(zone.height or 1))),
    )


def _parse_zones(value: dict[str, Any] | None) -> list[FridgeZoneConfig]:
    zones = []
    for raw in (value or {}).get("zones") or []:
        try:
            zones.append(_normalize_zone(FridgeZoneConfig.model_validate(raw)))
        except Exception:
            continue
    if not zones:
        return list(DEFAULT_ZONES)
    standard_keys = {zone.key for zone in zones if not zone.custom}
    merged = [zone for zone in DEFAULT_ZONES if zone.key not in standard_keys]
    merged.extend(zones)
    return merged


async def get_fridge_zones(db: AsyncSession, home_id: UUID) -> FridgeZoneListData:
    """读取家庭分区配置；无配置时返回标准默认分区。"""
    row = await _load_row(db, home_id)
    if row is None:
        return FridgeZoneListData(zones=list(DEFAULT_ZONES), source="default")
    return FridgeZoneListData(
        zones=_parse_zones(row.value),
        config_updated_at=row.config_updated_at.isoformat(),
        source=str((row.value or {}).get("source") or "miniapp"),
    )


async def upsert_fridge_zones(
    db: AsyncSession,
    home_id: UUID,
    zones: list[FridgeZoneConfig],
    *,
    source: str = "miniapp",
) -> FridgeZoneListData:
    """写入家庭分区配置。"""
    normalized = [_normalize_zone(zone) for zone in zones if zone.key and zone.label]
    if not normalized:
        normalized = list(DEFAULT_ZONES)
    now = _now()
    payload = {
        "schemaVersion": 1,
        "source": source,
        "zones": [zone.model_dump(mode="json", by_alias=True) for zone in normalized],
    }
    row = await _load_row(db, home_id)
    if row is None:
        row = SystemConfig(
            home_id=home_id,
            config_key=FRIDGE_ZONES_KEY,
            value=payload,
            config_updated_at=now,
        )
        db.add(row)
    else:
        row.value = payload
        row.config_updated_at = now
    await db.flush()
    return FridgeZoneListData(
        zones=normalized,
        config_updated_at=now.isoformat(),
        source=source,
    )


__all__ = [
    "DEFAULT_ZONES",
    "FRIDGE_ZONES_KEY",
    "get_fridge_zones",
    "upsert_fridge_zones",
]
