"""同步快照到设备的 MQTT 桥接层。

同步服务本身只负责落库和生成 serverRevision；本模块负责把云端当前快照转换为
固件已经支持的 MQTT 命令，避免小程序“同步成功”但设备端没有任何变化。
"""

from __future__ import annotations

import logging
from datetime import date, datetime, timezone
from typing import Any
from uuid import UUID

from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.models.device import Device, DeviceBinding
from app.models.inventory import InventoryItem
from app.models.reminder import Reminder
from app.models.system_config import SystemConfig
from app.schemas.fridge_zone import FridgeZoneConfig
from app.schemas.sync import SyncDevicePushData
from app.services.ai_config_service import build_device_payload, get_ai_config_full
from app.services.fridge_zone_service import get_fridge_zones
from app.services.mqtt_client import mqtt_client

logger = logging.getLogger(__name__)

DEVICE_SYNC_DOMAINS = {
    "inventory",
    "fridge_zones",
    "ai_config",
    "asr_config",
    "tts_config",
    "shopping_list",
    "recipe_cache",
    "reminder",
    "settings",
}

_SLOT_TO_CELL = {
    "A1": 0,
    "A2": 1,
    "A3": 2,
    "B1": 3,
    "B2": 4,
    "B3": 5,
    "C1": 6,
    "C2": 7,
    "C3": 8,
}


def _now_ms() -> int:
    return int(datetime.now(timezone.utc).timestamp() * 1000)


def _days_left(expire_date: date | None) -> int | None:
    if expire_date is None:
        return None
    return (expire_date - datetime.now(timezone.utc).date()).days


async def _active_devices(db: AsyncSession, home_id: UUID) -> list[Device]:
    result = await db.execute(
        select(Device)
        .join(DeviceBinding, DeviceBinding.device_id == Device.id)
        .where(DeviceBinding.home_id == home_id, DeviceBinding.status == "active")
        .order_by(DeviceBinding.created_at.asc())
    )
    return list(result.scalars().all())


def _device_zone_id(zone_key: str | None, zones: list[FridgeZoneConfig]) -> int:
    if not zone_key:
        return 1
    standard = {"freezer": 0, "left": 1, "right": 2, "door": 3}
    if zone_key in standard:
        return standard[zone_key]
    for idx, zone in enumerate(zones[:6]):
        if zone.key == zone_key:
            return idx
    return 4


def _device_zones(zones: list[FridgeZoneConfig]) -> list[dict[str, Any]]:
    device_zones: list[dict[str, Any]] = []
    for idx, zone in enumerate(zones[:6]):
        device_zones.append(
            {
                "id": idx,
                "key": zone.key,
                "name": zone.label,
                "note": zone.hint,
                "custom": zone.custom,
                "width": zone.width,
                "height": zone.height,
            }
        )
    return device_zones


async def build_device_inventory_document(
    db: AsyncSession,
    *,
    home_id: UUID,
    server_revision: int,
) -> dict[str, Any]:
    """把云端库存表转换为固件 UI 库存整快照。

    固件屏幕模型目前使用 numeric zone + 0..8 cell，因此这里保留云端 zone key，
    同时生成设备可直接应用的 zone/cell 字段。
    """
    zones_data = await get_fridge_zones(db, home_id)
    zones = list(zones_data.zones)
    items = (
        await db.execute(
            select(InventoryItem)
            .where(InventoryItem.home_id == home_id, InventoryItem.status != "deleted")
            .order_by(InventoryItem.updated_at.desc())
        )
    ).scalars().all()

    device_items: list[dict[str, Any]] = []
    data_updated_at_ms = 0
    for item in items:
        if item.updated_at is not None:
            data_updated_at_ms = max(data_updated_at_ms, int(item.updated_at.timestamp() * 1000))
        zone_id = _device_zone_id(item.zone, zones)
        cell = _SLOT_TO_CELL.get(str(item.slot or "").upper(), 4)
        quantity = float(item.quantity) if item.quantity is not None else 1.0
        if quantity.is_integer():
            quantity_text = f"{int(quantity)}{item.unit or '份'}"
        else:
            quantity_text = f"{quantity:g}{item.unit or '份'}"
        days_left = _days_left(item.expire_date)
        device_items.append(
            {
                "id": str(item.id),
                "name": item.name,
                "category": item.category,
                "quantity": quantity_text,
                "quantityValue": quantity,
                "unit": item.unit,
                "expire_date": item.expire_date.isoformat() if item.expire_date else "",
                "days_left": days_left,
                "location": item.location or "",
                "zone": zone_id,
                "zoneKey": item.zone,
                "slot": item.slot,
                "cell": cell,
                "source": item.source,
            }
        )

    return {
        "schema_version": 1,
        "snapshotVersion": server_revision,
        "serverRevision": server_revision,
        "updatedAtMs": data_updated_at_ms or _now_ms(),
        "source": "cloud_snapshot",
        "zones": _device_zones(zones),
        "items": device_items,
    }


def _asr_payload(full_config: dict[str, Any]) -> dict[str, Any]:
    return {
        "asrApiBaseUrl": full_config.get("asrApiBaseUrl"),
        "asrModel": full_config.get("asrModel"),
        "asrApiKey": full_config.get("asrApiKey", ""),
        "asrTimeoutMs": full_config.get("asrTimeoutMs"),
        "configUpdatedAt": full_config.get("configUpdatedAt", 0),
    }


def _tts_payload(full_config: dict[str, Any]) -> dict[str, Any]:
    return {
        "ttsApiBaseUrl": full_config.get("ttsApiBaseUrl"),
        "ttsModel": full_config.get("ttsModel"),
        "ttsVoice": full_config.get("ttsVoice"),
        "ttsApiKey": full_config.get("ttsApiKey", ""),
        "ttsTimeoutMs": full_config.get("ttsTimeoutMs"),
        "configUpdatedAt": full_config.get("configUpdatedAt", 0),
    }


async def _load_config_doc(db: AsyncSession, home_id: UUID, key: str) -> dict[str, Any]:
    row = (
        await db.execute(
            select(SystemConfig).where(SystemConfig.home_id == home_id, SystemConfig.config_key == key)
        )
    ).scalar_one_or_none()
    value = dict(row.value or {}) if row is not None else {}
    value.setdefault("items", [])
    value["source"] = value.get("source") or "cloud_snapshot"
    return value


async def _build_reminder_document(db: AsyncSession, home_id: UUID, server_revision: int) -> dict[str, Any]:
    reminders = (
        await db.execute(select(Reminder).where(Reminder.home_id == home_id).order_by(Reminder.updated_at.desc()))
    ).scalars().all()
    return {
        "schema_version": 1,
        "serverRevision": server_revision,
        "updatedAtMs": _now_ms(),
        "source": "cloud_snapshot",
        "reminders": [
            {
                "id": str(item.id),
                "reminderType": item.reminder_type,
                "title": item.title,
                "content": item.content,
                "status": item.status,
                "dueAt": item.due_at.isoformat() if item.due_at else None,
                "ackedAt": item.acked_at.isoformat() if item.acked_at else None,
            }
            for item in reminders
        ],
    }


async def push_cloud_snapshot_to_devices(
    db: AsyncSession,
    *,
    home_id: UUID,
    server_revision: int,
    domains: set[str] | None = None,
    request_device_inventory: bool = False,
    accept_clean_device_snapshot: bool = False,
    push_cloud_snapshot: bool = True,
) -> SyncDevicePushData:
    """按 domain 把云端当前快照下发给该家庭所有 active 设备。"""
    selected = set(domains or DEVICE_SYNC_DOMAINS) & DEVICE_SYNC_DOMAINS
    devices = await _active_devices(db, home_id)
    errors: list[str] = []
    command_count = 0
    queued_count = 0

    inventory_doc: dict[str, Any] | None = None
    full_ai_config: dict[str, Any] | None = None
    shopping_doc: dict[str, Any] | None = None
    recipe_doc: dict[str, Any] | None = None
    reminder_doc: dict[str, Any] | None = None
    settings_doc: dict[str, Any] | None = None

    for device in devices:
        if request_device_inventory:
            command_count += 1
            try:
                await mqtt_client.publish_command(
                    device.device_sn,
                    "inventory_refresh",
                    {
                        "homeId": str(home_id),
                        "request_id": f"sync-refresh:{server_revision}:{device.device_sn}",
                        "acceptCleanSnapshot": accept_clean_device_snapshot,
                    },
                )
                queued_count += 1
            except Exception as exc:  # noqa: BLE001
                errors.append(f"{device.device_sn}: inventory_refresh failed: {exc}")
            command_count += 1
            try:
                await mqtt_client.publish_command(
                    device.device_sn,
                    "sync_documents_refresh",
                    {
                        "homeId": str(home_id),
                        "request_id": f"sync-docs:{server_revision}:{device.device_sn}",
                        "acceptCleanSnapshot": accept_clean_device_snapshot,
                    },
                )
                queued_count += 1
            except Exception as exc:  # noqa: BLE001
                errors.append(f"{device.device_sn}: sync_documents_refresh failed: {exc}")

        if not push_cloud_snapshot:
            continue

        if server_revision > 0 and selected & {"inventory", "fridge_zones"}:
            if inventory_doc is None:
                inventory_doc = await build_device_inventory_document(
                    db,
                    home_id=home_id,
                    server_revision=server_revision,
                )
            command_count += 1
            try:
                await mqtt_client.publish_command(
                    device.device_sn,
                    "inventory_replace",
                    {
                        "request_id": f"sync-inventory:{server_revision}:{device.device_sn}",
                        "serverRevision": server_revision,
                        "inventory": inventory_doc,
                    },
                )
                queued_count += 1
            except Exception as exc:  # noqa: BLE001
                errors.append(f"{device.device_sn}: inventory_replace failed: {exc}")

        if "ai_config" in selected:
            if full_ai_config is None:
                full_ai_config = await get_ai_config_full(db, home_id)
            command_count += 1
            try:
                await mqtt_client.publish_command(
                    device.device_sn,
                    "ai_config_update",
                    {
                        "request_id": f"sync-ai:{server_revision}:{device.device_sn}",
                        **build_device_payload(full_ai_config),
                    },
                )
                queued_count += 1
            except Exception as exc:  # noqa: BLE001
                errors.append(f"{device.device_sn}: ai_config_update failed: {exc}")

        if "asr_config" in selected:
            if full_ai_config is None:
                full_ai_config = await get_ai_config_full(db, home_id)
            command_count += 1
            try:
                await mqtt_client.publish_command(
                    device.device_sn,
                    "asr_config_update",
                    {
                        "request_id": f"sync-asr:{server_revision}:{device.device_sn}",
                        **_asr_payload(full_ai_config),
                    },
                )
                queued_count += 1
            except Exception as exc:  # noqa: BLE001
                errors.append(f"{device.device_sn}: asr_config_update failed: {exc}")

        if "tts_config" in selected:
            if full_ai_config is None:
                full_ai_config = await get_ai_config_full(db, home_id)
            command_count += 1
            try:
                await mqtt_client.publish_command(
                    device.device_sn,
                    "tts_config_update",
                    {
                        "request_id": f"sync-tts:{server_revision}:{device.device_sn}",
                        **_tts_payload(full_ai_config),
                    },
                )
                queued_count += 1
            except Exception as exc:  # noqa: BLE001
                errors.append(f"{device.device_sn}: tts_config_update failed: {exc}")

        if "shopping_list" in selected:
            if shopping_doc is None:
                shopping_doc = await _load_config_doc(db, home_id, "shopping_list")
                shopping_doc["serverRevision"] = server_revision
                shopping_doc["updatedAtMs"] = _now_ms()
            command_count += 1
            try:
                await mqtt_client.publish_command(
                    device.device_sn,
                    "shopping_list_update",
                    {"request_id": f"sync-shopping:{server_revision}:{device.device_sn}", **shopping_doc},
                )
                queued_count += 1
            except Exception as exc:  # noqa: BLE001
                errors.append(f"{device.device_sn}: shopping_list_update failed: {exc}")

        if "recipe_cache" in selected:
            if recipe_doc is None:
                recipe_doc = await _load_config_doc(db, home_id, "recipe_cache")
                recipe_doc["serverRevision"] = server_revision
                recipe_doc["updatedAtMs"] = _now_ms()
            command_count += 1
            try:
                await mqtt_client.publish_command(
                    device.device_sn,
                    "recipe_cache_update",
                    {"request_id": f"sync-recipe:{server_revision}:{device.device_sn}", **recipe_doc},
                )
                queued_count += 1
            except Exception as exc:  # noqa: BLE001
                errors.append(f"{device.device_sn}: recipe_cache_update failed: {exc}")

        if "reminder" in selected:
            if reminder_doc is None:
                reminder_doc = await _build_reminder_document(db, home_id, server_revision)
            command_count += 1
            try:
                await mqtt_client.publish_command(
                    device.device_sn,
                    "reminder_update",
                    {"request_id": f"sync-reminder:{server_revision}:{device.device_sn}", **reminder_doc},
                )
                queued_count += 1
            except Exception as exc:  # noqa: BLE001
                errors.append(f"{device.device_sn}: reminder_update failed: {exc}")

        if "settings" in selected:
            if settings_doc is None:
                settings_doc = await _load_config_doc(db, home_id, "settings")
                settings_doc["serverRevision"] = server_revision
                settings_doc["updatedAtMs"] = _now_ms()
            command_count += 1
            try:
                await mqtt_client.publish_command(
                    device.device_sn,
                    "preferences_update",
                    {"request_id": f"sync-settings:{server_revision}:{device.device_sn}", **settings_doc},
                )
                queued_count += 1
            except Exception as exc:  # noqa: BLE001
                errors.append(f"{device.device_sn}: preferences_update failed: {exc}")

    if errors:
        logger.warning("cloud snapshot push completed with errors: home=%s errors=%s", home_id, errors)

    return SyncDevicePushData(
        device_count=len(devices),
        command_count=command_count,
        queued_count=queued_count,
        domains=sorted(selected),
        errors=errors,
    )


__all__ = [
    "DEVICE_SYNC_DOMAINS",
    "build_device_inventory_document",
    "push_cloud_snapshot_to_devices",
]
