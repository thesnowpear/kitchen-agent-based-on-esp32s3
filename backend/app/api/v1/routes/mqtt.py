"""MQTT bridge routes."""

from datetime import datetime, timezone

from fastapi import APIRouter, Depends
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.db.session import get_db
from app.models.device import Device, DeviceStatusEvent
from app.schemas.mqtt import (
    MqttCommandPublishRequest,
    MqttCommandPublishResponse,
    MqttEventIngestRequest,
    MqttEventIngestResponse,
)
from app.services.mqtt_gateway import mqtt_gateway

router = APIRouter()


@router.post("/event/ingest", response_model=MqttEventIngestResponse)
async def mqtt_event_ingest(
    payload: MqttEventIngestRequest,
    db: AsyncSession = Depends(get_db),
) -> MqttEventIngestResponse:
    """接收 EMQX Webhook 或内部 MQTT 消费者转发的设备事件。"""
    device = (await db.execute(select(Device).where(Device.device_sn == payload.device_sn))).scalar_one_or_none()
    if device:
        device.status = "online"
        device.last_seen_at = datetime.now(timezone.utc)
        if "firmware_version" in payload.payload:
            device.firmware_version = str(payload.payload["firmware_version"])

    event = DeviceStatusEvent(
        device_id=device.id if device else None,
        device_sn=payload.device_sn,
        event_type=payload.event_type,
        payload=payload.model_dump(mode="json"),
    )
    db.add(event)
    await db.commit()
    return MqttEventIngestResponse(event_id=event.id)


@router.post("/command/publish", response_model=MqttCommandPublishResponse)
async def mqtt_command_publish(payload: MqttCommandPublishRequest) -> MqttCommandPublishResponse:
    """发布设备命令的占位接口，后续替换为真实 MQTT 客户端。"""
    result = await mqtt_gateway.publish_command(
        device_sn=payload.device_sn,
        command=payload.command,
        payload=payload.payload,
        qos=payload.qos,
    )
    return MqttCommandPublishResponse(topic=result.topic, queued=result.queued, request_id=payload.request_id)
