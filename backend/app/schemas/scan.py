"""手机拍照入库 - 视觉识别接口的 Pydantic schema。

设计要点：
- 与 plans/ui-reference-zazzy-candle.md §2.5 的 `/api/v1/inventory/scan` 字段保持一致；
- 对外仍走 CamelModel（snake_case → camelCase 自动 alias），方便小程序直接消费；
- 仅承载"识别 + 推荐位置"的预览结果，不直接写入库存，真正落库由用户在小程序点"确认入库"再走 `/inventory` POST。
"""

from app.schemas.common import CamelModel


# 单个识别候选：模型给出的食材项 + 服务端补充的"建议位置"。
# - quantity/unit 都来自模型猜测，因此 quantity 用 float（可以是 0.5 瓶这种小数）。
# - suggested_zone/slot/reason 由 vision_service 内的位置推荐器写入，模型本身不感知冰箱布局。
# - confidence 是 0~100 整数，前端用来决定是否高亮"需要二次确认"。
class ScanCandidate(CamelModel):
    """识别结果中的单个食材候选条目。"""

    name: str
    quantity: float
    unit: str
    category: str | None = None
    # 推荐区域：freezer / left / right / door / custom_*；为 None 表示无法推荐（例如所有区域已满）。
    suggested_zone: str | None = None
    # 推荐 slot：A1~C3；为 None 表示该 zone 已满或未给出 zone。
    suggested_slot: str | None = None
    # 推荐原因，便于前端展示给用户（例如「同类食材优先合并」/「按类别推荐空位」）。
    suggested_reason: str | None = None
    # 模型给出的置信度，0~100 整数。低置信度建议前端提示用户二次确认。
    confidence: int
    # 模型补充说明（保鲜建议、储存温度等），可选。
    note: str | None = None


# 整个 /inventory/scan 接口的 data 部分：
# - candidates：识别出的食材列表，模型识别失败或图中无食材时为空 []；
# - raw_text：模型原始 content，便于线上排错（截断 200 字符），不暴露 token / key；
# - model_used：实际命中的模型名（来自 settings.vision_model），方便 A/B 不同视觉模型时定位。
class ScanResult(CamelModel):
    """`/api/v1/inventory/scan` 接口的数据载荷。"""

    candidates: list[ScanCandidate]
    raw_text: str | None = None
    model_used: str


__all__ = ["ScanCandidate", "ScanResult"]
