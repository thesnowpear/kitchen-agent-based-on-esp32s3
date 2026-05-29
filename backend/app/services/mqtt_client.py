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

    # ---------- 生命周期 ----------

    async def connect(self) -> None:
        """启动 MQTT 客户端并订阅设备上行主题。"""
        if self._client is not None:
            return

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
            logger.warning("MQTT connect failed: %s", exc)
            self._client = None
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
                await self._handle_state(data)
            elif kind == "cmd_ack":
                self._handle_cmd_ack(data)
            elif kind == "ai_config":
                await self._handle_ai_config(home_id_str, device_sn_from_topic, data)
            else:
                # sensor / error 仅写一条事件留痕，不更新 device 主表。
                await self._record_event(data, kind)
        except Exception:  # noqa: BLE001
            logger.exception("MQTT on_message handler failed: topic=%s", topic)
        return 0

    # ---------- 业务分发 ----------

    async def _handle_state(self, data: dict) -> None:
        """state 上报：更新 Device.status / last_seen_at / firmware，并写一条事件。"""
        device_sn = data.get("device_id") or ""
        if not device_sn:
            return
        try:
            async with AsyncSessionLocal() as db:
                result = await db.execute(
                    select(Device).where(Device.device_sn == device_sn)
                )
                device = result.scalar_one_or_none()
                if device is not None:
                    device.status = "online" if data.get("online", True) else "offline"
                    device.last_seen_at = datetime.now(timezone.utc)
                    firmware = data.get("firmware")
                    if isinstance(firmware, str) and firmware:
                        device.firmware_version = firmware

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
        if self._client is None or not self._connected:
            raise RuntimeError("mqtt client is not connected")

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
