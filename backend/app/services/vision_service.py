"""视觉识别服务：手机拍照 → SiliconFlow 多模态模型 → 食材候选 + 位置推荐。

调用入口 `analyze_food_image`：
1. 用 Pillow 压缩到长边 ≤ 1024 / JPEG q=85，降低请求体积与模型 latency；
2. base64 编码进 OpenAI 兼容的 chat/completions 多模态消息体；
3. 解析模型返回的 JSON 数组（容错 markdown 包裹）；
4. 服务端补充每个候选项的 `suggested_zone / slot / reason`；
5. 写一条 `InventoryEvent(event_type="inventory.scan")` 留痕，不存原图。

注意：
- 本服务**不会**写 InventoryItem。识别只是预览，落库走 `/inventory` POST + 用户确认；
- 任何与外网交互的字符串都不应包含 API key；日志只记 HTTP 状态、长度、耗时；
- 与 firmware 侧 ai_client 一样，必须严格 UTF-8 输入输出，避免 400 invalid unicode。
"""

from __future__ import annotations

import base64
import io
import json
import logging
import time
from collections import Counter
from typing import Any
from uuid import UUID

import httpx
from PIL import Image
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.config import settings
from app.models.inventory import InventoryEvent, InventoryItem
from app.schemas.scan import ScanCandidate, ScanResult

logger = logging.getLogger(__name__)

# 标准九宫格槽位：A1~C3，从左上到右下的可读顺序。
# 推荐空位时按此顺序找第一个未占用的 slot，行为可预测，便于前端高亮。
_SLOT_ORDER: tuple[str, ...] = ("A1", "A2", "A3", "B1", "B2", "B3", "C1", "C2", "C3")

# 标准 zone（不含 custom_*）。custom_* 区域由用户自建，识别器不主动推荐过去。
_STANDARD_ZONES: tuple[str, ...] = ("freezer", "left", "right", "door")

# 类别 → 默认 zone 的映射。模型返回的 category 不一定严格匹配，因此用包含关键字的方式做兜底。
_CATEGORY_ZONE_MAP: dict[str, str] = {
    "蔬菜": "left",
    "水果": "left",
    "熟食": "left",
    "肉类": "right",
    "海鲜": "right",
    "乳制品": "right",
    "速冻": "freezer",
    "饮品": "door",
    "调料": "door",
}

# 模型 prompt：要求严格输出 JSON 数组、不带 markdown，便于直接 json.loads。
# 这里写死中文，与小程序业务语言一致；如果以后做国际化再做 i18n。
_VISION_PROMPT = (
    "你是冰箱食材识别助手。请仔细看这张冰箱内或食材的照片，识别出图中所有可见的食材或包装食品。\n"
    "请严格按以下 JSON 数组格式输出（不要 markdown 代码块，不要任何额外文字）：\n"
    "[\n"
    "  {\"name\": \"食材中文名\", \"quantity\": 数字, \"unit\": \"个/盒/袋/瓶/g/ml/份 等\",\n"
    "   \"category\": \"蔬菜/水果/肉类/海鲜/乳制品/饮品/调料/速冻/熟食/其他\",\n"
    "   \"confidence\": 0-100 的整数, \"note\": \"保鲜或储存建议（可选）\"}\n"
    "]\n"
    "若图中无任何食材，输出 []。"
)


def _compress_image(image_bytes: bytes) -> bytes:
    """长边 ≤ 1024 / JPEG q=85 的压缩。原图小于 1024 长边时不上采样，只走重新编码统一格式。"""
    with Image.open(io.BytesIO(image_bytes)) as img:
        # 一些手机拍出的 PNG / WEBP 带 alpha 通道，JPEG 不支持，必须先转 RGB。
        if img.mode not in ("RGB", "L"):
            img = img.convert("RGB")
        long_edge = max(img.size)
        if long_edge > 1024:
            ratio = 1024.0 / long_edge
            new_size = (int(img.size[0] * ratio), int(img.size[1] * ratio))
            img = img.resize(new_size, Image.LANCZOS)
        buf = io.BytesIO()
        img.save(buf, format="JPEG", quality=85, optimize=True)
        return buf.getvalue()


def _strip_markdown_fences(text: str) -> str:
    """去掉模型可能多带的 ```json / ``` 包裹，留中间真实 JSON。"""
    t = text.strip()
    if t.startswith("```"):
        # 去掉首行 ``` 或 ```json
        first_newline = t.find("\n")
        if first_newline != -1:
            t = t[first_newline + 1 :]
        # 去掉末尾 ```
        if t.endswith("```"):
            t = t[:-3]
    return t.strip()


def _parse_candidates(raw_text: str) -> list[dict[str, Any]]:
    """把模型输出解析为 dict 列表。解析失败返回空列表，由调用方决定如何处理。"""
    cleaned = _strip_markdown_fences(raw_text)
    if not cleaned:
        return []
    try:
        parsed = json.loads(cleaned)
    except json.JSONDecodeError:
        logger.warning("vision model returned non-JSON content, length=%d", len(cleaned))
        return []
    if not isinstance(parsed, list):
        logger.warning("vision model returned non-list JSON: type=%s", type(parsed).__name__)
        return []
    return [item for item in parsed if isinstance(item, dict)]


def _resolve_zone_from_category(category: str | None) -> str:
    """类别 → 默认 zone。模型类别用包含匹配做兜底，所有未命中归 right。"""
    if not category:
        return "right"
    for key, zone in _CATEGORY_ZONE_MAP.items():
        if key in category:
            return zone
    return "right"


def _first_empty_slot(occupied_in_zone: set[str]) -> str | None:
    """按 A1~C3 顺序找第一个未占用 slot。全占返回 None。"""
    for slot in _SLOT_ORDER:
        if slot not in occupied_in_zone:
            return slot
    return None


def _build_zone_occupancy(items: list[InventoryItem]) -> dict[str, set[str]]:
    """从活跃 InventoryItem 列表汇总每个 zone 已占用的 slot 集合。"""
    occ: dict[str, set[str]] = {z: set() for z in _STANDARD_ZONES}
    for it in items:
        if it.zone and it.slot:
            occ.setdefault(it.zone, set()).add(it.slot)
    return occ


def _build_name_to_location(items: list[InventoryItem]) -> dict[str, tuple[str, str]]:
    """同名食材 → (zone, slot) 索引，便于"合并到同一格"的推荐规则。

    若同名食材存在多条记录，取最近一条更新的；这里简单按列表顺序覆盖，
    上游已用 updated_at desc 排序的话会自然保留最新位置。
    """
    mapping: dict[str, tuple[str, str]] = {}
    for it in items:
        if it.zone and it.slot:
            # 仅当还没记录过该名字时写入，避免被更老的记录覆盖（依赖调用方传入的顺序）。
            mapping.setdefault(it.name, (it.zone, it.slot))
    return mapping


def _suggest_location_for_candidate(
    name: str,
    category: str | None,
    occupancy: dict[str, set[str]],
    name_index: dict[str, tuple[str, str]],
) -> tuple[str | None, str | None, str | None]:
    """位置推荐规则，按优先级顺序：
    1. 已有同名 → 同 zone/slot；
    2. 类别默认 zone 有空位 → 取该 zone 第一个空 slot；
    3. 默认 zone 已满 → 4 个标准 zone 里物品最少且未满的那个的第一个空 slot；
    4. 全部 zone 都满 → 不给位置，让用户手动选。

    注意：occupancy 会被本函数修改（占用刚分配的 slot），保证同一次 scan 多个候选不会推到同一格。
    """
    # 规则 1：同名合并。允许覆盖到已占满的 zone（用户语义就是"放一起"）。
    if name in name_index:
        zone, slot = name_index[name]
        return zone, slot, "同类食材优先合并"

    # 规则 2：默认 zone 优先。
    default_zone = _resolve_zone_from_category(category)
    slot = _first_empty_slot(occupancy.get(default_zone, set()))
    if slot is not None:
        occupancy.setdefault(default_zone, set()).add(slot)
        return default_zone, slot, "按类别推荐空位"

    # 规则 3：选剩余物品最少且未满的 zone。
    candidates_with_room: list[tuple[str, int]] = []
    for z in _STANDARD_ZONES:
        used = occupancy.get(z, set())
        if len(used) < len(_SLOT_ORDER):
            candidates_with_room.append((z, len(used)))
    if candidates_with_room:
        # 按已用 slot 数升序，取最空的；同分时保持 _STANDARD_ZONES 原顺序。
        candidates_with_room.sort(key=lambda x: x[1])
        chosen_zone = candidates_with_room[0][0]
        slot = _first_empty_slot(occupancy.get(chosen_zone, set()))
        if slot is not None:
            occupancy.setdefault(chosen_zone, set()).add(slot)
            return chosen_zone, slot, "原区域已满，建议放最空区域"

    # 规则 4：全部满。
    return None, None, "所有标准区域已满，请手动指定位置"


def _normalize_candidate(item: dict[str, Any]) -> ScanCandidate | None:
    """把模型返回的 dict 规整为 ScanCandidate；字段缺失/类型错时尽量兜底，完全无法识别时返 None。"""
    name = item.get("name")
    if not isinstance(name, str) or not name.strip():
        return None
    name = name.strip()

    # quantity 模型可能返回数字或字符串
    raw_qty = item.get("quantity", 1)
    try:
        quantity = float(raw_qty)
    except (TypeError, ValueError):
        quantity = 1.0
    if quantity <= 0:
        quantity = 1.0

    unit = item.get("unit") if isinstance(item.get("unit"), str) else "份"
    if not unit:
        unit = "份"

    category = item.get("category") if isinstance(item.get("category"), str) else None

    raw_conf = item.get("confidence", 60)
    try:
        confidence = int(raw_conf)
    except (TypeError, ValueError):
        confidence = 60
    confidence = max(0, min(100, confidence))

    note = item.get("note") if isinstance(item.get("note"), str) else None

    return ScanCandidate(
        name=name,
        quantity=quantity,
        unit=unit,
        category=category,
        confidence=confidence,
        note=note,
    )


async def _call_vision_model(
    image_bytes: bytes,
    ai_config: dict[str, Any] | None = None,
) -> tuple[str, str]:
    """调用 SiliconFlow OpenAI 兼容多模态接口。

    ai_config 由 analyze_food_image 调 ai_config_service.get_ai_config_full(db, home_id) 取得，
    含 visionModel / apiBaseUrl / apiKey；为 None 时回落 settings 默认（向后兼容老调用方）。

    返回 (raw_text, model_used)。出错时抛 RuntimeError，路由层会转成 ApiResponse(ok=False)。
    """
    if ai_config is None:
        ai_config = {
            "apiBaseUrl": settings.siliconflow_base_url,
            "apiKey": settings.siliconflow_api_key or "",
            "visionModel": settings.vision_model,
        }
    api_key = ai_config.get("apiKey") or settings.siliconflow_api_key
    base_url = ai_config.get("apiBaseUrl") or settings.siliconflow_base_url
    model_name = ai_config.get("visionModel") or settings.vision_model

    if not api_key:
        # 调用层 normally 已经先判过，这里再守一次。
        raise RuntimeError("vision service not configured, set SILICONFLOW_API_KEY")

    b64 = base64.b64encode(image_bytes).decode("ascii")
    data_url = f"data:image/jpeg;base64,{b64}"

    payload = {
        "model": model_name,
        "messages": [
            {
                "role": "user",
                "content": [
                    {"type": "text", "text": _VISION_PROMPT},
                    {"type": "image_url", "image_url": {"url": data_url}},
                ],
            }
        ],
        "temperature": 0.2,
        "max_tokens": 800,
    }
    url = f"{base_url.rstrip('/')}/chat/completions"
    headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }

    start = time.monotonic()
    async with httpx.AsyncClient(timeout=60.0) as client:
        resp = await client.post(url, json=payload, headers=headers)
    latency_ms = int((time.monotonic() - start) * 1000)

    # 严禁打印 key / 完整 body，只记必要的可观测指标。
    logger.info(
        "vision call status=%d latency_ms=%d resp_len=%d model=%s",
        resp.status_code,
        latency_ms,
        len(resp.content or b""),
        model_name,
    )

    if resp.status_code != 200:
        # 仍尝试把状态码 + 短截断 body 抛给上游记录，但不会回写到 ApiResponse 给用户。
        snippet = (resp.text or "")[:200]
        raise RuntimeError(f"vision http {resp.status_code}: {snippet}")

    try:
        body = resp.json()
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"vision response not json: {exc}") from exc

    choices = body.get("choices") or []
    if not choices:
        raise RuntimeError("vision response missing choices")
    msg = choices[0].get("message") or {}
    content = msg.get("content")
    if not isinstance(content, str):
        # 某些模型可能返回 list 形式 content，简单 join 一下。
        if isinstance(content, list):
            content = "".join(
                p.get("text", "") if isinstance(p, dict) else str(p) for p in content
            )
        else:
            content = ""
    return content, model_name


async def _load_active_items(db: AsyncSession, home_id: UUID) -> list[InventoryItem]:
    """读取当前 home 未删除的 InventoryItem，按 updated_at desc 排序，用于位置推荐。"""
    stmt = (
        select(InventoryItem)
        .where(InventoryItem.home_id == home_id, InventoryItem.status != "deleted")
        .order_by(InventoryItem.updated_at.desc())
    )
    result = await db.execute(stmt)
    return list(result.scalars().all())


def _zone_with_most_room(occupancy: dict[str, set[str]]) -> str | None:
    """辅助：用于 hint_zone 不在标准 zone 时的兜底（当前未使用，留作扩展）。"""
    counter = Counter({z: len(occupancy.get(z, set())) for z in _STANDARD_ZONES})
    if not counter:
        return None
    return counter.most_common()[-1][0]


async def analyze_food_image(
    image_bytes: bytes,
    hint_zone: str | None = None,
    db: AsyncSession | None = None,
    home_id: UUID | None = None,
) -> ScanResult:
    """对外主入口：压缩 → 模型识别 → 位置推荐 → 写事件。

    hint_zone 暂未参与推荐（用户在前端选择区域 tab 时透传过来，留作未来加权用）；
    db / home_id 任一为空时，跳过"读现有库存"与"写 InventoryEvent"两步，便于单测。
    """
    # 1. 压缩。压缩异常不致命，但通常意味着上传的不是图片，向上抛即可。
    try:
        compressed = _compress_image(image_bytes)
    except Exception as exc:  # noqa: BLE001 -- Pillow 抛的异常类型较多，统一兜底转 RuntimeError。
        raise RuntimeError(f"invalid image: {exc}") from exc

    # 2. 取当前 active AI 配置（含明文 api_key、visionModel）。
    #    延迟 import 避免 vision_service ↔ ai_config_service 在模块加载阶段循环依赖。
    if db is not None and home_id is not None:
        from app.services.ai_config_service import get_ai_config_full

        ai_config = await get_ai_config_full(db, home_id)
    else:
        ai_config = None  # _call_vision_model 内部会回落 settings 默认

    # 3. 调模型。
    raw_text, model_used = await _call_vision_model(compressed, ai_config=ai_config)

    # 3. 解析候选。
    raw_items = _parse_candidates(raw_text)
    candidates: list[ScanCandidate] = []
    for raw_item in raw_items:
        c = _normalize_candidate(raw_item)
        if c is not None:
            candidates.append(c)

    # 4. 位置推荐。仅在 db + home_id 都给定时读库存做推荐；否则候选位置全空，前端自行处理。
    if db is not None and home_id is not None and candidates:
        items = await _load_active_items(db, home_id)
        occupancy = _build_zone_occupancy(items)
        name_index = _build_name_to_location(items)
        enriched: list[ScanCandidate] = []
        for c in candidates:
            zone, slot, reason = _suggest_location_for_candidate(
                c.name, c.category, occupancy, name_index
            )
            enriched.append(
                c.model_copy(
                    update={
                        "suggested_zone": zone,
                        "suggested_slot": slot,
                        "suggested_reason": reason,
                    }
                )
            )
        candidates = enriched

    # 5. 写事件留痕。原图不存，只存摘要 + 截断 raw_text。
    truncated_raw = raw_text[:200] if isinstance(raw_text, str) else None
    if db is not None and home_id is not None:
        event = InventoryEvent(
            home_id=home_id,
            event_type="inventory.scan",
            actor_type="user",
            payload={
                "model_used": model_used,
                "candidate_count": len(candidates),
                "candidates": [c.model_dump(by_alias=False) for c in candidates],
                "raw_text_snippet": truncated_raw,
                "hint_zone": hint_zone,
            },
        )
        db.add(event)
        await db.commit()

    return ScanResult(
        candidates=candidates,
        raw_text=truncated_raw,
        model_used=model_used,
    )


__all__ = ["analyze_food_image"]
