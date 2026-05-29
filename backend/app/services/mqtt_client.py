"""异步 MQTT 客户端：subscribe 设备上报、publish 命令下发。

设计要点：
- 用 gmqtt（asyncio 友好）替换 mqtt_gateway.py 占位实现；
- 订阅 fridge/+/+/{state,sensor,cmd_ack,error}，state 落 Device + DeviceStatusEvent，cmd_ack 喂给 Future；
- publish_command 把 device_sn → device_id (=device_sn, 与 ESP32 NVS 约定一致) + home_id 反查后拼 topic；
- 重连交给 gmqtt 内置；on_message DB 操作失败仅记日志，不让客户端挂掉。

主题约定（与 components/mqtt_protocol/fridge_mqtt_protocol.c 一致）：
- 上行：fridge/{home_id}/{device_id}/state | sensor | cmd_ack | error
- 下行：fridge/{home_id}/{device_id}/cmd

设备端 NVS 里把 `device_id` 直接写成 `device_sn`，因此本模块所有 topic 拼接时用 device_sn 当 device_id 段。
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import logging
import time
from datetime import datetime, timezone
from typing import Any
from uuid import UUID

from gmqtt import Client as GmqttClient
from sqlalchemy import select

from app.core.config import settings
from app.db.session import AsyncSessionLocal
from app.models.device import Device, DeviceBinding, DeviceStatusEvent

logger = logging.getLogger(__name__)


# 限频日志：避免设备掉线后疯狂打 warning。
_LAST_LOG_TS: dict[str, float] = {}


def _log_throttled(key: str, level: int, msg: str, *args: Any) -> None:
    """同一 key 30 秒内只允许打一次同级别日志。"""
    now = time.monotonic()
    if now - _LAST_LOG_TS.get(key, 0.0) < 30.0:
        return
    _LAST_LOG_TS[key] = now
    logger.log(level, msg, *args)


class FridgeMqttClient:
    """全局 MQTT 单例，由 lifespan 在 app 启动时 connect。"""

    def __init__(self) -> None:
        self._client: GmqttClient | None = None
        # request_id → Future(ack payload dict)；ai_service.chat_via_device 用它等设备回复。
        self._ack_waiters: dict[str, asyncio.Future[dict]] = {}
        self._connected: bool = False
        self._connect_lock: asyncio.Lock | None = None

    # ---------- 生命周期 ----------

    async def connect(self) -> None:
        """启动 MQTT 客户端并订阅设备上行主题。"""
        if self._client is not None and self._connected:
            return
        if self._connect_lock is None:
            self._connect_lock = asyncio.Lock()
        async with self._connect_lock:
            if self._client is not None and self._connected:
                return
            if self._client is not None:
                with contextlib.suppress(Exception):
                    await self._client.disconnect()
                self._client = None
                self._connected = False

            client = GmqttClient(settings.mqtt_client_id)
            if settings.mqtt_username:
                # gmqtt 在 connect 前调用 set_auth_credentials，password 可为 None。
                client.set_auth_credentials(
                    settings.mqtt_username, settings.mqtt_password or None
                )

            client.on_connect = self._on_connect
            client.on_message = self._on_message
            client.on_disconnect = self._on_disconnect

            try:
                await client.connect(
                    settings.mqtt_broker_host,
                    settings.mqtt_broker_port,
                    keepalive=settings.mqtt_keepalive_seconds,
                )
            except Exception as exc:  # noqa: BLE001 - 让 lifespan 决定要不要重试
                logger.warning(
                    "MQTT connect failed: host=%s port=%s client_id=%s error=%s",
                    settings.mqtt_broker_host,
                    settings.mqtt_broker_port,
                    settings.mqtt_client_id,
                    exc,
                )
                self._client = None
                self._connected = False
                raise

            self._client = client

    async def disconnect(self) -> None:
        """优雅关闭客户端，取消所有 pending future。"""
        if self._client is None:
            return
        with contextlib.suppress(Exception):
            await self._client.disconnect()
        self._client = None
        self._connected = False
        # 取消所有未完成的 ack 等待者，避免协程泄漏。
        for fut in self._ack_waiters.values():
            if not fut.done():
                fut.cancel()
        self._ack_waiters.clear()

    # ---------- gmqtt 回调 ----------

    def _on_connect(self, client: GmqttClient, flags: int, rc: int, properties: Any) -> None:  # noqa: D401
        """连接成功后批量订阅 fridge/+/+/* 上行主题。"""
        self._connected = True
        prefix = settings.mqtt_topic_prefix
        topics = [
            (f"{prefix}/+/+/state", 1),
            (f"{prefix}/+/+/sensor", 1),
            (f"{prefix}/+/+/cmd_ack", 1),
            (f"{prefix}/+/+/error", 1),
            (f"{prefix}/+/+/inventory", 1),
            (f"{prefix}/+/+/sync", 1),
            # 设备 retained 上报当前 AI 配置；backend 用 last-write-wins 与 SystemConfig 协调。
            (f"{prefix}/+/+/ai_config", 1),
        ]
        for topic, qos in topics:
            client.subscribe(topic, qos=qos)
        logger.info("MQTT connected, subscribed to %d topics", len(topics))

    def _on_disconnect(self, client: GmqttClient, packet: Any, exc: Any = None) -> None:  # noqa: D401
        """记一次断线日志（限频），gmqtt 会自动重连。"""
        self._connected = False
        _log_throttled(
            "mqtt_disconnect", logging.WARNING, "MQTT disconnected (will auto-reconnect)"
        )

    async def _on_message(
        self,
        client: GmqttClient,
        topic: str,
        payload: bytes,
        qos: int,
        properties: Any,
    ) -> int:
        """统一消息分发；topic 末段决定走哪条处理路径。"""
        try:
            data = json.loads(payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            _log_throttled(
                f"mqtt_bad_payload:{topic}",
                logging.WARNING,
                "MQTT bad payload on %s: %s",
                topic,
                exc,
            )
            return 0

        parts = topic.split("/")
        if len(parts) < 4:
            return 0
        kind = parts[-1]
        # topic = fridge/{home_id}/{device_sn}/{kind}
        home_id_str = parts[1] if len(parts) >= 4 else ""
        device_sn_from_topic = parts[2] if len(parts) >= 4 else ""

        try:
            if kind == "state":
                await self._handle_state(home_id_str, device_sn_from_topic, data)
            elif kind == "cmd_ack":
                self._handle_cmd_ack(data)
            elif kind == "inventory":
                await self._handle_inventory(home_id_str, device_sn_from_topic, data)
            elif kind == "sync":
                await self._handle_device_sync(home_id_str, device_sn_from_topic, data)
            elif kind == "ai_config":
                await self._handle_ai_config(home_id_str, device_sn_from_topic, data)
            else:
                # sensor / error 仅写一条事件留痕，不更新 device 主表。
                await self._record_event(data, kind)
        except Exception:  # noqa: BLE001
            logger.exception("MQTT on_message handler failed: topic=%s", topic)
        return 0

    # ---------- 业务分发 ----------

    async def _handle_state(
        self,
        home_id_str: str,
        device_sn_from_topic: str,
        data: dict,
    ) -> None:
        """state 上报：更新 Device 状态；上线/重连时补发云端快照。"""
        from uuid import UUID as _UUID

        device_sn = data.get("device_id") or device_sn_from_topic or ""
        if not device_sn:
            return
        next_status = "online" if data.get("online", True) else "offline"
        was_online = False
        has_active_binding = False
        try:
            home_id = _UUID(home_id_str)
        except (ValueError, AttributeError):
            home_id = None
        try:
            async with AsyncSessionLocal() as db:
                result = await db.execute(
                    select(Device).where(Device.device_sn == device_sn)
                )
                device = result.scalar_one_or_none()
                if device is not None:
                    was_online = device.status == "online"
                    device.status = next_status
                    device.last_seen_at = datetime.now(timezone.utc)
                    firmware = data.get("firmware")
                    if isinstance(firmware, str) and firmware:
                        device.firmware_version = firmware
                    if home_id is not None:
                        binding_result = await db.execute(
                            select(DeviceBinding.id)
                            .where(
                                DeviceBinding.device_id == device.id,
                                DeviceBinding.home_id == home_id,
                                DeviceBinding.status == "active",
                            )
                            .limit(1)
                        )
                        has_active_binding = binding_result.scalar_one_or_none() is not None

                db.add(
                    DeviceStatusEvent(
                        device_id=device.id if device else None,
                        device_sn=device_sn,
                        event_type="state",
                        payload=data,
                    )
                )
                await db.commit()
        except Exception:  # noqa: BLE001
            logger.exception("handle_state failed: device_sn=%s", device_sn)
            return

        is_reconnect = bool(
            data.get("reconnect")
            or data.get("reconnected")
            or data.get("boot")
            or data.get("reason") in {"reconnect", "mqtt_reconnect", "wifi_reconnect"}
        )
        should_push_snapshot = next_status == "online" and has_active_binding and (not was_online or is_reconnect)
        if should_push_snapshot and home_id is not None:
            await self._push_snapshot_after_device_online(home_id, device_sn)

    async def _push_snapshot_after_device_online(self, home_id: UUID, device_sn: str) -> None:
        """设备恢复在线后，先拉设备本地库存，再推送非库存配置。

        初次绑定/演示现场经常是设备里已有库存而云端仍是测试数据；上线瞬间如果先
        下发旧云端库存，会把设备的真实 20 条覆盖成 1 条。这里让设备 clean snapshot
        也能作为种子导入，库存导入成功后 _handle_inventory 会再广播最新云端库存。
        """
        from app.services.sync_device_bridge import DEVICE_SYNC_DOMAINS, push_cloud_snapshot_to_devices
        from app.services.sync_service import get_status

        try:
            async with AsyncSessionLocal() as db:
                status = await get_status(db, home_id)
                await push_cloud_snapshot_to_devices(
                    db,
                    home_id=home_id,
                    server_revision=status.server_revision,
                    domains=set(DEVICE_SYNC_DOMAINS) - {"inventory", "fridge_zones"},
                    request_device_inventory=True,
                    accept_clean_device_snapshot=True,
                    )
            logger.info(
                "device inventory requested and config snapshot pushed after online: home=%s device=%s rev=%s",
                home_id,
                device_sn,
                status.server_revision,
            )
        except Exception:  # noqa: BLE001
            logger.exception(
                "cloud snapshot push after device online failed: home=%s device=%s",
                home_id,
                device_sn,
            )

    def _handle_cmd_ack(self, data: dict) -> None:
        """cmd_ack：完成 publish_command 注册的 Future。"""
        request_id = data.get("request_id")
        if not request_id:
            return
        fut = self._ack_waiters.pop(request_id, None)
        if fut is not None and not fut.done():
            fut.set_result(data)

    async def _handle_ai_config(
        self,
        home_id_str: str,
        device_sn: str,
        data: dict,
    ) -> None:
        """处理设备 retained 上报的 AI 配置。

        逻辑（last-write-wins）：
        - 解析 topic 的 home_id（设备 NVS 里写的字符串）；非 UUID 时跳过；
        - 调 ai_config_service.merge_from_device 比对时间戳：
          · 设备 > backend → backend 已写入；
          · backend > 设备 → 这里 publish_command(ai_config_update) 把 backend 版本反推；
          · 相等 / 都为 0 → noop。
        """
        from uuid import UUID as _UUID

        # 延迟 import 避免 mqtt_client ↔ ai_config_service 形成模块初始化时的循环依赖。
        from app.services.ai_config_service import merge_from_device

        try:
            home_id = _UUID(home_id_str)
        except (ValueError, AttributeError):
            _log_throttled(
                f"ai_config_bad_home:{home_id_str}",
                logging.WARNING,
                "ai_config retained msg with non-UUID home_id=%s device_sn=%s",
                home_id_str,
                device_sn,
            )
            return

        try:
            async with AsyncSessionLocal() as db:
                outcome, push_payload = await merge_from_device(db, home_id, data)
        except Exception:  # noqa: BLE001
            logger.exception(
                "ai_config merge_from_device failed: home=%s device=%s",
                home_id_str,
                device_sn,
            )
            return

        logger.info(
            "ai_config sync: home=%s device=%s outcome=%s",
            home_id_str,
            device_sn,
            outcome,
        )

        if outcome == "should_push_to_device" and push_payload is not None and device_sn:
            try:
                await self.publish_command(
                    device_sn=device_sn,
                    command="ai_config_update",
                    payload=push_payload,
                )
            except Exception as exc:  # noqa: BLE001
                logger.warning(
                    "ai_config push_to_device failed: device=%s err=%s",
                    device_sn,
                    exc,
                )

    async def _handle_inventory(
        self,
        home_id_str: str,
        device_sn: str,
        data: dict,
    ) -> None:
        """处理设备上报的完整 UI 库存快照。

        设备收到 inventory_refresh 后会发布 fridge/{home}/{device}/inventory。
        这里把它转换成 sync event，复用同步服务的整快照导入逻辑，再把新的
        serverRevision 回推到设备，清掉设备端 dirty 标志。
        """
        from uuid import UUID as _UUID

        from app.models.home import Home
        from app.schemas.sync import SyncPushEvent
        from app.services.sync_device_bridge import push_cloud_snapshot_to_devices
        from app.services.sync_service import push_events

        try:
            home_id = _UUID(home_id_str)
        except (ValueError, AttributeError):
            _log_throttled(
                f"inventory_bad_home:{home_id_str}",
                logging.WARNING,
                "inventory msg with non-UUID home_id=%s device_sn=%s",
                home_id_str,
                device_sn,
            )
            return

        snapshot_version = data.get("snapshotVersion") or data.get("snapshot_version") or 0
        device_revision = int(data.get("serverRevision") or data.get("server_revision") or 0)
        device_dirty = bool(data.get("dirty"))
        force_import = bool(data.get("forceImport") or data.get("force_import") or data.get("acceptCleanSnapshot"))
        updated_at = data.get("updatedAtMs") or data.get("updated_at_ms") or data.get("timestamp_ms") or int(time.time() * 1000)
        client_event_id = f"device:{device_sn}:inventory:{snapshot_version}:{updated_at}"
        items_count = len(data.get("items") or []) if isinstance(data.get("items"), list) else 0
        zones_count = len(data.get("zones") or []) if isinstance(data.get("zones"), list) else 0
        logger.info(
            "inventory snapshot received: home=%s device=%s items=%s zones=%s snapshot=%s device_rev=%s dirty=%s force=%s",
            home_id_str,
            device_sn,
            items_count,
            zones_count,
            snapshot_version,
            device_revision,
            device_dirty,
            force_import,
        )

        try:
            async with AsyncSessionLocal() as db:
                home = await db.get(Home, home_id)
                if home is None:
                    _log_throttled(
                        f"inventory_unknown_home:{home_id_str}",
                        logging.WARNING,
                        "inventory msg for unknown home=%s device_sn=%s",
                        home_id_str,
                        device_sn,
                    )
                    return
                from app.services.sync_service import get_status

                status = await get_status(db, home_id)
                if not force_import and not device_dirty and device_revision <= status.server_revision:
                    logger.info(
                        "inventory snapshot ignored: home=%s device=%s dirty=false device_rev=%s server_rev=%s",
                        home_id_str,
                        device_sn,
                        device_revision,
                        status.server_revision,
                    )
                    return
                result = await push_events(
                    db,
                    home=home,
                    user=None,
                    events=[
                        SyncPushEvent(
                            client_event_id=client_event_id,
                            domain="inventory",
                            op="snapshot",
                            source="device",
                            device_sn=device_sn,
                            payload=data,
                        )
                    ],
                )
                logger.info(
                    "inventory snapshot imported: home=%s device=%s accepted=%s duplicates=%s server_rev=%s items=%s",
                    home_id_str,
                    device_sn,
                    result.accepted,
                    result.duplicates,
                    result.server_revision,
                    items_count,
                )
                await push_cloud_snapshot_to_devices(
                    db,
                    home_id=home_id,
                    server_revision=result.server_revision,
                    domains={"inventory"},
                    request_device_inventory=False,
                )
        except Exception:  # noqa: BLE001
            logger.exception(
                "inventory snapshot import failed: home=%s device=%s",
                home_id_str,
                device_sn,
            )

    async def _handle_device_sync(
        self,
        home_id_str: str,
        device_sn: str,
        data: dict,
    ) -> None:
        """处理设备上报的非库存本地同步文档。

        设备端只上报购物清单、菜谱缓存、提醒和偏好等普通 JSON 文档；
        API Key 等敏感字段不走该普通事件通道。
        """
        from uuid import UUID as _UUID

        from app.models.home import Home
        from app.schemas.sync import SyncPushEvent
        from app.services.sync_device_bridge import push_cloud_snapshot_to_devices
        from app.services.sync_service import push_events

        try:
            home_id = _UUID(home_id_str)
        except (ValueError, AttributeError):
            _log_throttled(
                f"sync_bad_home:{home_id_str}",
                logging.WARNING,
                "device sync msg with non-UUID home_id=%s device_sn=%s",
                home_id_str,
                device_sn,
            )
            return

        raw_events = data.get("events") if isinstance(data.get("events"), list) else []
        allowed_domains = {"shopping_list", "recipe_cache", "reminder", "settings", "ai_history", "ai_config"}
        events: list[SyncPushEvent] = []
        for raw in raw_events[:20]:
            if not isinstance(raw, dict):
                continue
            domain = str(raw.get("domain") or "")
            payload = raw.get("payload") if isinstance(raw.get("payload"), dict) else {}
            if domain not in allowed_domains or not payload:
                continue
            events.append(
                SyncPushEvent(
                    client_event_id=str(raw.get("clientEventId") or raw.get("client_event_id") or f"device:{device_sn}:{domain}:{time.time_ns()}")[:128],
                    domain=domain,
                    op=str(raw.get("op") or "replace")[:64],
                    source="device",
                    device_sn=device_sn,
                    payload=payload,
                )
            )
        if not events:
            return

        try:
            async with AsyncSessionLocal() as db:
                home = await db.get(Home, home_id)
                if home is None:
                    _log_throttled(
                        f"sync_unknown_home:{home_id_str}",
                        logging.WARNING,
                        "device sync msg for unknown home=%s device_sn=%s",
                        home_id_str,
                        device_sn,
                    )
                    return
                from app.services.ai_config_service import should_accept_device_config

                filtered_events: list[SyncPushEvent] = []
                for event in events:
                    if event.domain == "ai_config" and not await should_accept_device_config(db, home_id, event.payload):
                        continue
                    filtered_events.append(event)
                if not filtered_events:
                    return
                result = await push_events(db, home=home, user=None, events=filtered_events)
                changed_domains = {event.domain for event in filtered_events}
                device_domains = set(changed_domains)
                if "ai_config" in changed_domains:
                    device_domains.update({"asr_config", "tts_config"})
                await push_cloud_snapshot_to_devices(
                    db,
                    home_id=home_id,
                    server_revision=result.server_revision,
                    domains=device_domains,
                    request_device_inventory=False,
                )
        except Exception:  # noqa: BLE001
            logger.exception("device sync import failed: home=%s device=%s", home_id_str, device_sn)

    async def _record_event(self, data: dict, kind: str) -> None:
        """sensor / error 留痕；DB 失败仅日志，不打断客户端。"""
        device_sn = data.get("device_id") or ""
        if not device_sn:
            return
        try:
            async with AsyncSessionLocal() as db:
                result = await db.execute(
                    select(Device.id).where(Device.device_sn == device_sn)
                )
                device_id = result.scalar_one_or_none()
                db.add(
                    DeviceStatusEvent(
                        device_id=device_id,
                        device_sn=device_sn,
                        event_type=kind,
                        payload=data,
                    )
                )
                await db.commit()
        except Exception:  # noqa: BLE001
            logger.exception("record_event failed: device_sn=%s kind=%s", device_sn, kind)

    # ---------- 公开 API ----------

    def is_connected(self) -> bool:
        return self._connected

    async def ensure_connected(self) -> None:
        """确保 MQTT 已连接；启动时失败后，下一次 API 调用会自动补连。"""
        if self._client is not None and self._connected:
            return
        await self.connect()
        if self._client is None or not self._connected:
            raise RuntimeError(
                f"mqtt client is not connected: broker={settings.mqtt_broker_host}:{settings.mqtt_broker_port}"
            )

    def register_ack_waiter(self, request_id: str) -> asyncio.Future[dict]:
        """登记一个 future 等待设备 cmd_ack；调用方负责 unregister 或 await。"""
        loop = asyncio.get_running_loop()
        fut = loop.create_future()
        self._ack_waiters[request_id] = fut
        return fut

    def unregister_ack_waiter(self, request_id: str) -> None:
        """超时或异常时手动清理 future，避免内存泄漏。"""
        self._ack_waiters.pop(request_id, None)

    async def publish_command(
        self,
        device_sn: str,
        command: str,
        payload: dict | None = None,
        qos: int = 1,
    ) -> str:
        """下发命令到 fridge/{home_id}/{device_sn}/cmd。

        - 通过 DeviceBinding 反查 active home_id；如果设备未绑定则抛 ValueError。
        - body 加 schema_version=1 与 request_id（来自 payload 或自动 None），与设备端协议对齐。
        - 返回最终拼出的 topic 字符串，便于上层日志。
        """
        await self.ensure_connected()

        home_id = await self._resolve_home_id(device_sn)
        topic = f"{settings.mqtt_topic_prefix}/{home_id}/{device_sn}/cmd"
        body = {
            "schema_version": 1,
            "request_id": (payload or {}).get("request_id"),
            "command": command,
            "payload": payload or {},
        }
        self._client.publish(topic, json.dumps(body, ensure_ascii=False), qos=qos)
        return topic

    async def _resolve_home_id(self, device_sn: str) -> UUID:
        """根据 device_sn 找到当前 active 绑定的 home_id。"""
        async with AsyncSessionLocal() as db:
            result = await db.execute(
                select(DeviceBinding.home_id)
                .join(Device, Device.id == DeviceBinding.device_id)
                .where(
                    Device.device_sn == device_sn,
                    DeviceBinding.status == "active",
                )
                .limit(1)
            )
            home_id = result.scalar_one_or_none()
        if home_id is None:
            raise ValueError(f"device {device_sn} is not bound to any active home")
        return home_id


# 全局单例：路由 / lifespan 直接 import 用。
mqtt_client = FridgeMqttClient()


# ---------- lifespan 钩子 ----------


async def startup_mqtt_client(_app: Any) -> None:
    """FastAPI lifespan 启动时调，失败仅日志不阻塞整个 app。"""
    try:
        await mqtt_client.connect()
    except Exception as exc:  # noqa: BLE001
        logger.warning("MQTT client startup failed (ignored): %s", exc)


async def shutdown_mqtt_client(_app: Any) -> None:
    """FastAPI lifespan 退出时优雅关闭。"""
    try:
        await mqtt_client.disconnect()
    except Exception:  # noqa: BLE001
        logger.exception("MQTT client shutdown failed (ignored)")


__all__ = [
    "FridgeMqttClient",
    "mqtt_client",
    "startup_mqtt_client",
    "shutdown_mqtt_client",
]
