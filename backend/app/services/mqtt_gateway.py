"""MQTT 命令发布薄壳（已迁移到真客户端）。

历史上这里是 placeholder（仅计算 topic 返回 queued=True）；现在 mqtt_client.py 已经接了
真实 gmqtt 客户端，本模块只是为 routes/mqtt.py 旧 `/mqtt/command/publish` HTTP 接口
保留兼容签名 `MqttPublishResult(topic, queued)`，内部转调 mqtt_client.publish_command。
"""

from __future__ import annotations

import logging
from dataclasses import dataclass

from app.core.config import settings
from app.schemas.common import JsonDict
from app.services.mqtt_client import mqtt_client

logger = logging.getLogger(__name__)


@dataclass(frozen=True)
class MqttPublishResult:
    """发布结果占位结构，保持与原占位实现的字段一致。"""

    topic: str
    queued: bool


class MqttGateway:
    """对外保留的网关门面，仅做 topic 拼接与 mqtt_client 转调。"""

    def command_topic(self, device_sn: str) -> str:
        """旧路径会调；现在 home_id 由 mqtt_client 内部反查，这里只返回一个调试用 topic。"""
        return f"{settings.mqtt_topic_prefix}/<home_id>/{device_sn}/cmd"

    async def publish_command(
        self,
        device_sn: str,
        command: str,
        payload: JsonDict,
        qos: int,
    ) -> MqttPublishResult:
        """转调 mqtt_client.publish_command；客户端未连接时返 queued=False，不抛 5xx。"""
        try:
            topic = await mqtt_client.publish_command(device_sn, command, payload, qos=qos)
            return MqttPublishResult(topic=topic, queued=True)
        except (RuntimeError, ValueError) as exc:
            logger.warning(
                "mqtt_gateway publish_command degraded: device_sn=%s cmd=%s err=%s",
                device_sn,
                command,
                exc,
            )
            return MqttPublishResult(
                topic=self.command_topic(device_sn),
                queued=False,
            )


mqtt_gateway = MqttGateway()


__all__ = ["MqttGateway", "MqttPublishResult", "mqtt_gateway"]
