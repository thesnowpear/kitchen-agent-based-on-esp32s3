"""三端数据备份与同步服务层。

第一版采用后写覆盖策略：每个被接受的事件都会获得新的 server_revision，并立即
投影到当前业务表或 SystemConfig 文档。固件库存回流使用整份快照，后端负责把
设备 UI JSON 转成云端 InventoryItem。
"""

from __future__ import annotations

from datetime import date, datetime, timezone
from decimal import Decimal, InvalidOperation
from typing import Any
from uuid import UUID, uuid4

from sqlalchemy import func, select
from sqlalchemy.ext.asyncio import AsyncSession

from app.models.ai_chat import AiChatMessage
from app.models.device import Device
from app.models.home import Home
from app.models.inventory import InventoryItem
from app.models.reminder import Reminder
from app.models.system_config import SystemConfig
from app.models.sync import SyncEvent, SyncState
from app.models.user import User
from app.schemas.ai_config import AiConfigUpdateRequest
from app.schemas.common import JsonDict
from app.schemas.fridge_zone import FridgeZoneConfig
from app.schemas.inventory import InventoryItemSchema, InventoryUpdateRequest
from app.schemas.reminder import ReminderSchema
from app.schemas.sync import (
    AiHistorySnapshot,
    RecipeCacheData,
    ShoppingListData,
    SyncEventSchema,
    SyncPullData,
    SyncPushData,
    SyncPushEvent,
    SyncSnapshotData,
    SyncStatusData,
)
from app.services.ai_chat_history_service import DEFAULT_SESSION_ID, list_history
from app.services.ai_config_service import get_ai_config, should_accept_device_config, upsert_ai_config
from app.services.fridge_zone_service import DEFAULT_ZONES, FRIDGE_ZONES_KEY, get_fridge_zones, upsert_fridge_zones

SHOPPING_LIST_KEY = "shopping_list"
RECIPE_CACHE_KEY = "recipe_cache"
SETTINGS_KEY = "settings"


def _now() -> datetime:
    return datetime.now(timezone.utc)


async def _get_state(db: AsyncSession, home_id: UUID, *, create: bool = True) -> SyncState:
    """读取或创建家庭同步游标。"""
    result = await db.execute(select(SyncState).where(SyncState.home_id == home_id))
    state = result.scalar_one_or_none()
    if state is None and create:
        state = SyncState(home_id=home_id, current_revision=0)
        db.add(state)
        await db.flush()
    if state is None:
        raise RuntimeError("sync state not found")
    return state


async def _next_revision(db: AsyncSession, home_id: UUID, source: str, client_event_id: str) -> int:
    """生成家庭内单调递增 revision。"""
    state = await _get_state(db, home_id)
    state.current_revision += 1
    state.last_source = source
    state.last_client_event_id = client_event_id
    await db.flush()
    return int(state.current_revision)


def _event_to_schema(event: SyncEvent) -> SyncEventSchema:
    return SyncEventSchema(
        id=event.id,
        client_event_id=event.client_event_id,
        domain=event.domain,
        op=event.op,
        source=event.source,
        server_revision=int(event.server_revision),
        client_revision=int(event.client_revision) if event.client_revision is not None else None,
        device_sn=event.device_sn,
        payload=event.payload or {},
        created_at=event.created_at,
    )


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


def _reminder_to_schema(item: Reminder) -> ReminderSchema:
    return ReminderSchema(
        id=item.id,
        reminder_type=item.reminder_type,
        title=item.title,
        content=item.content,
        status=item.status,
        due_at=item.due_at,
        acked_at=item.acked_at,
    )


async def _resolve_device(db: AsyncSession, device_sn: str | None) -> Device | None:
    if not device_sn:
        return None
    return (await db.execute(select(Device).where(Device.device_sn == device_sn))).scalar_one_or_none()


def _slot_from_cell(cell: Any) -> str | None:
    slots = ["A1", "A2", "A3", "B1", "B2", "B3", "C1", "C2", "C3"]
    try:
        idx = int(cell)
    except (TypeError, ValueError):
        return None
    if idx < 0 or idx >= len(slots):
        return None
    return slots[idx]


def _parse_date(value: Any) -> date | None:
    """兼容小程序 ISO 日期和设备空值；无法解析时不覆盖原值。"""
    if value is None or value == "":
        return None
    if isinstance(value, date) and not isinstance(value, datetime):
        return value
    if isinstance(value, datetime):
        return value.date()
    if isinstance(value, str):
        try:
            return date.fromisoformat(value[:10])
        except ValueError:
            return None
    return None


def _parse_quantity(value: Any, fallback: Any) -> Decimal:
    """库存数量写入 Numeric 列前统一收敛，避免字符串直接落库。"""
    raw = fallback if value is None else value
    try:
        return Decimal(str(raw if raw is not None else 1))
    except (InvalidOperation, ValueError, TypeError):
        return Decimal("1")


def _inventory_payload_from_event(payload: dict[str, Any]) -> dict[str, Any]:
    """兼容小程序队列里 item/payload 包裹的库存事件。

    在线 REST 写入成功后，小程序会把服务端返回 item 和原始 payload 一起入队；
    离线时则只有 payload。同步服务统一摊平成 InventoryItem 可识别的字段。
    """
    raw_payload = payload.get("payload")
    raw_item = payload.get("item")
    merged: dict[str, Any] = {}
    if isinstance(raw_payload, dict):
        merged.update(raw_payload)
    if isinstance(raw_item, dict):
        merged.update(raw_item)
    for key, value in payload.items():
        if key not in {"payload", "item"}:
            merged.setdefault(key, value)
    return merged or payload


def _zone_key_from_device_zone(zone: Any, zones: list[dict[str, Any]]) -> str | None:
    try:
        zone_id = int(zone)
    except (TypeError, ValueError):
        return zone if isinstance(zone, str) else None
    if 0 <= zone_id < len(zones):
        raw = zones[zone_id]
        if isinstance(raw.get("key"), str):
            return raw["key"]
        standard = ["freezer", "left", "right", "door"]
        if zone_id < len(standard):
            return standard[zone_id]
        return f"custom_{zone_id - 3}"
    return None


async def _upsert_inventory_item(
    db: AsyncSession,
    *,
    home_id: UUID,
    payload: dict[str, Any],
    source: str,
    device: Device | None,
) -> None:
    """把小程序/同步事件中的单条库存写入 InventoryItem。"""
    item_id = payload.get("id") or payload.get("itemId") or payload.get("item_id")
    item: InventoryItem | None = None
    if item_id:
        try:
            item = await db.get(InventoryItem, UUID(str(item_id)))
        except (ValueError, TypeError):
            item = None
        if item is not None and item.home_id != home_id:
            item = None

    if item is None:
        item = InventoryItem(home_id=home_id, name=str(payload.get("name") or "未命名食材"))
        db.add(item)
        await db.flush()

    if device is not None:
        item.device_id = device.id
    item.name = str(payload.get("name") or item.name or "未命名食材")[:120]
    item.category = payload.get("category")
    item.quantity = _parse_quantity(payload.get("quantity"), item.quantity)
    item.unit = str(payload.get("unit") or item.unit or "份")[:24]
    item.zone = payload.get("zone")
    item.slot = payload.get("slot")
    item.location = payload.get("location")
    parsed_expire_date = _parse_date(payload.get("expireDate") or payload.get("expire_date"))
    if parsed_expire_date is not None or "expireDate" in payload or "expire_date" in payload:
        item.expire_date = parsed_expire_date
    item.status = str(payload.get("status") or item.status or "active")[:32]
    item.source = source[:32]
    item.confidence = payload.get("confidence")
    item.extra = payload.get("extra") if isinstance(payload.get("extra"), dict) else item.extra or {}


async def _replace_inventory_from_device_snapshot(
    db: AsyncSession,
    *,
    home_id: UUID,
    snapshot: dict[str, Any],
    source: str,
    device: Device | None,
) -> None:
    """把设备整份 ui_inventory 快照投影到云端库存表。"""
    zones_raw = snapshot.get("zones") if isinstance(snapshot.get("zones"), list) else []
    zones = [z for z in zones_raw if isinstance(z, dict)]
    items_raw = snapshot.get("items") if isinstance(snapshot.get("items"), list) else []
    seen_keys: set[tuple[str | None, str | None, str]] = set()

    for raw in items_raw:
        if not isinstance(raw, dict):
            continue
        zone_key = _zone_key_from_device_zone(raw.get("zone"), zones)
        slot = raw.get("slot") or _slot_from_cell(raw.get("cell"))
        name = str(raw.get("name") or "未命名食材")[:120]
        location = raw.get("location")
        quantity_text = raw.get("quantity")
        item_payload = {
            "name": name,
            "quantity": 1,
            "unit": str(quantity_text or "份")[:24],
            "zone": zone_key,
            "slot": slot,
            "location": location,
            "expireDate": None,
            "status": "active",
            "source": source,
            "extra": {
                "deviceQuantityText": quantity_text,
                "deviceExpireText": raw.get("expire_date") or raw.get("expireDate"),
                "deviceDaysLeft": raw.get("days_left") or raw.get("daysLeft"),
                "deviceCell": raw.get("cell"),
            },
        }
        seen_keys.add((zone_key, slot, name))
        existing = (
            await db.execute(
                select(InventoryItem).where(
                    InventoryItem.home_id == home_id,
                    InventoryItem.zone == zone_key,
                    InventoryItem.slot == slot,
                    InventoryItem.name == name,
                )
            )
        ).scalar_one_or_none()
        if existing is not None:
            item_payload["id"] = str(existing.id)
        await _upsert_inventory_item(db, home_id=home_id, payload=item_payload, source=source, device=device)

    current = (
        await db.execute(
            select(InventoryItem).where(
                InventoryItem.home_id == home_id,
                InventoryItem.status != "deleted",
            )
        )
    ).scalars().all()
    for item in current:
        key = (item.zone, item.slot, item.name)
        # 设备上报的是整份 ui_inventory 快照；导入后家庭库存应以设备快照为准，
        # 否则小程序里早先手工测试的无 device_id 条目会残留成第 21 条。
        if key not in seen_keys:
            item.status = "deleted"


async def _upsert_config_doc(
    db: AsyncSession,
    *,
    home_id: UUID,
    key: str,
    value: dict[str, Any],
    source: str,
) -> None:
    now = _now()
    row = (
        await db.execute(
            select(SystemConfig).where(SystemConfig.home_id == home_id, SystemConfig.config_key == key)
        )
    ).scalar_one_or_none()
    next_value = dict(value)
    next_value["source"] = source
    if row is None:
        db.add(SystemConfig(home_id=home_id, config_key=key, value=next_value, config_updated_at=now))
    else:
        row.value = next_value
        row.config_updated_at = now


async def _apply_event(
    db: AsyncSession,
    *,
    home: Home,
    user: User | None,
    event: SyncPushEvent,
) -> Device | None:
    """把同步事件投影到业务表。"""
    device = await _resolve_device(db, event.device_sn)
    payload = event.payload or {}

    if event.domain == "inventory":
        inventory_payload = _inventory_payload_from_event(payload)
        if event.op in {"snapshot", "replace"}:
            await _replace_inventory_from_device_snapshot(db, home_id=home.id, snapshot=payload, source=event.source, device=device)
        elif event.op == "delete":
            item_id = inventory_payload.get("id") or inventory_payload.get("itemId")
            if item_id:
                try:
                    item = await db.get(InventoryItem, UUID(str(item_id)))
                except (ValueError, TypeError):
                    item = None
                if item is not None and item.home_id == home.id:
                    item.status = "deleted"
        else:
            await _upsert_inventory_item(db, home_id=home.id, payload=inventory_payload, source=event.source, device=device)
    elif event.domain == "fridge_zones":
        zones_raw = payload.get("zones") if isinstance(payload.get("zones"), list) else []
        zones = [FridgeZoneConfig.model_validate(z) for z in zones_raw if isinstance(z, dict)]
        await upsert_fridge_zones(db, home.id, zones or list(DEFAULT_ZONES), source=event.source)
    elif event.domain == "ai_config":
        if event.source == "device" and not await should_accept_device_config(db, home.id, payload):
            return device
        config_ts_raw = payload.get("configUpdatedAt") or payload.get("config_updated_at")
        try:
            config_ts_ms = int(config_ts_raw) if config_ts_raw is not None else 0
        except (TypeError, ValueError):
            config_ts_ms = 0
        config_dt = datetime.fromtimestamp(config_ts_ms / 1000.0, tz=timezone.utc) if config_ts_ms > 0 else None
        await upsert_ai_config(
            db,
            home.id,
            AiConfigUpdateRequest.model_validate(payload),
            source=event.source,
            config_updated_at=config_dt,
            commit=False,
        )
    elif event.domain in {"shopping_list", "recipe_cache", "settings"}:
        key = SHOPPING_LIST_KEY if event.domain == "shopping_list" else RECIPE_CACHE_KEY if event.domain == "recipe_cache" else SETTINGS_KEY
        await _upsert_config_doc(db, home_id=home.id, key=key, value=payload, source=event.source)
    elif event.domain == "reminder":
        if event.op in {"replace", "snapshot"}:
            await _upsert_config_doc(db, home_id=home.id, key="reminder_queue", value=payload, source=event.source)
            return device
        reminder_id = payload.get("id") or payload.get("reminderId")
        if reminder_id:
            try:
                reminder = await db.get(Reminder, UUID(str(reminder_id)))
            except (ValueError, TypeError):
                reminder = None
            if reminder is not None and reminder.home_id == home.id:
                reminder.status = str(payload.get("status") or reminder.status)
                reminder.acked_at = _now()
    elif event.domain == "ai_history":
        messages = payload.get("messages") if isinstance(payload.get("messages"), list) else []
        session_id = str(payload.get("sessionId") or DEFAULT_SESSION_ID)[:128]
        for msg in messages[-20:]:
            if not isinstance(msg, dict):
                continue
            role = str(msg.get("role") or "")[:16]
            content = str(msg.get("content") or "")
            if role not in {"user", "assistant"} or not content:
                continue
            db.add(
                AiChatMessage(
                    home_id=home.id,
                    user_id=user.id if user else None,
                    session_id=session_id,
                    role=role,
                    content=content[:4096],
                    source=event.source,
                    device_sn=event.device_sn,
                    extra={"synced": True},
                )
            )
    return device


async def push_events(
    db: AsyncSession,
    *,
    home: Home,
    user: User | None,
    events: list[SyncPushEvent],
) -> SyncPushData:
    """批量接收客户端事件，按 client_event_id 幂等。"""
    accepted: list[SyncEvent] = []
    duplicates = 0

    for incoming in events:
        existing = (
            await db.execute(
                select(SyncEvent).where(
                    SyncEvent.home_id == home.id,
                    SyncEvent.client_event_id == incoming.client_event_id,
                )
            )
        ).scalar_one_or_none()
        if existing is not None:
            duplicates += 1
            accepted.append(existing)
            continue

        device = await _apply_event(db, home=home, user=user, event=incoming)
        revision = await _next_revision(db, home.id, incoming.source, incoming.client_event_id)
        row = SyncEvent(
            home_id=home.id,
            user_id=user.id if user else None,
            device_id=device.id if device else None,
            device_sn=incoming.device_sn,
            client_event_id=incoming.client_event_id,
            domain=incoming.domain,
            op=incoming.op,
            source=incoming.source,
            server_revision=revision,
            client_revision=incoming.client_revision,
            payload=incoming.payload,
        )
        db.add(row)
        await db.flush()
        accepted.append(row)

    await db.commit()
    state = await _get_state(db, home.id)
    return SyncPushData(
        server_revision=int(state.current_revision),
        accepted=len(events) - duplicates,
        duplicates=duplicates,
        events=[_event_to_schema(event) for event in accepted],
    )


async def record_server_event(
    db: AsyncSession,
    *,
    home_id: UUID,
    domain: str,
    op: str,
    payload: dict[str, Any],
    user_id: UUID | None = None,
    device_id: UUID | None = None,
    device_sn: str | None = None,
    client_event_id: str | None = None,
) -> SyncEvent:
    """服务端 REST 写接口记录同步事件。

    这些接口已经完成了业务表写入，因此这里只分配 revision 并写事件日志，
    让小程序和设备后续能通过 /sync/pull 看到同一批变更。
    """
    event_id = client_event_id or f"server:{domain}:{op}:{uuid4()}"
    existing = (
        await db.execute(
            select(SyncEvent).where(SyncEvent.home_id == home_id, SyncEvent.client_event_id == event_id)
        )
    ).scalar_one_or_none()
    if existing is not None:
        return existing
    revision = await _next_revision(db, home_id, "server", event_id)
    row = SyncEvent(
        home_id=home_id,
        user_id=user_id,
        device_id=device_id,
        device_sn=device_sn,
        client_event_id=event_id,
        domain=domain,
        op=op,
        source="server",
        server_revision=revision,
        payload=payload,
    )
    db.add(row)
    await db.flush()
    return row


async def get_status(db: AsyncSession, home_id: UUID) -> SyncStatusData:
    from app.core.config import settings
    from app.services.mqtt_client import mqtt_client

    state = await _get_state(db, home_id)
    domain_rows = await db.execute(
        select(SyncEvent.domain, func.max(SyncEvent.server_revision))
        .where(SyncEvent.home_id == home_id)
        .group_by(SyncEvent.domain)
    )
    domains = {domain: int(revision or 0) for domain, revision in domain_rows.all()}
    return SyncStatusData(
        server_revision=int(state.current_revision),
        last_synced_at=state.updated_at,
        pending_events=0,
        domains=domains,
        mqtt_connected=mqtt_client.is_connected(),
        mqtt_broker=f"{settings.mqtt_broker_host}:{settings.mqtt_broker_port}",
    )


async def _load_config_doc(db: AsyncSession, home_id: UUID, key: str) -> tuple[dict[str, Any], datetime | None]:
    row = (
        await db.execute(
            select(SystemConfig).where(SystemConfig.home_id == home_id, SystemConfig.config_key == key)
        )
    ).scalar_one_or_none()
    return (row.value if row else {}, row.config_updated_at if row else None)


async def build_snapshot(
    db: AsyncSession,
    *,
    home: Home,
    user: User | None,
) -> SyncSnapshotData:
    """构建当前家庭完整备份快照。"""
    state = await _get_state(db, home.id)
    inventory = (
        await db.execute(
            select(InventoryItem)
            .where(InventoryItem.home_id == home.id, InventoryItem.status != "deleted")
            .order_by(InventoryItem.updated_at.desc())
        )
    ).scalars().all()
    reminders = (
        await db.execute(select(Reminder).where(Reminder.home_id == home.id).order_by(Reminder.updated_at.desc()))
    ).scalars().all()
    zones = await get_fridge_zones(db, home.id)
    ai_config = await get_ai_config(db, home.id)
    shopping, shopping_updated = await _load_config_doc(db, home.id, SHOPPING_LIST_KEY)
    recipes, recipes_updated = await _load_config_doc(db, home.id, RECIPE_CACHE_KEY)
    settings_doc, settings_updated = await _load_config_doc(db, home.id, SETTINGS_KEY)
    session_id, history = await list_history(db, home_id=home.id, session_id=DEFAULT_SESSION_ID)
    history_payload = [msg.model_dump(mode="json", by_alias=True) for msg in history]
    if settings_updated:
        settings_doc = dict(settings_doc)
        settings_doc.setdefault("updatedAt", settings_updated.isoformat())
    return SyncSnapshotData(
        server_revision=int(state.current_revision),
        generated_at=_now(),
        inventory=[_item_to_schema(item) for item in inventory],
        fridge_zones=zones.zones,
        ai_config=ai_config,
        reminders=[_reminder_to_schema(item) for item in reminders],
        shopping_list=ShoppingListData(items=shopping.get("items") or [], updated_at=shopping_updated),
        recipe_cache=RecipeCacheData(items=recipes.get("items") or [], updated_at=recipes_updated),
        settings=settings_doc,
        ai_history=AiHistorySnapshot(session_id=session_id, messages=history_payload),
    )


async def pull_changes(
    db: AsyncSession,
    *,
    home: Home,
    user: User | None,
    since_revision: int,
) -> SyncPullData:
    state = await _get_state(db, home.id)
    rows = (
        await db.execute(
            select(SyncEvent)
            .where(SyncEvent.home_id == home.id, SyncEvent.server_revision > since_revision)
            .order_by(SyncEvent.server_revision.asc())
            .limit(200)
        )
    ).scalars().all()
    snapshot = await build_snapshot(db, home=home, user=user)
    return SyncPullData(
        server_revision=int(state.current_revision),
        events=[_event_to_schema(row) for row in rows],
        snapshot=snapshot,
    )


__all__ = [
    "build_snapshot",
    "get_status",
    "pull_changes",
    "push_events",
    "record_server_event",
]
