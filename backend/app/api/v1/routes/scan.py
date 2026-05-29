"""手机拍照入库 - `/inventory/scan` 路由。

说明：
- 本路由**不挂**到 `api/v1/router.py`。挂载顺序由主对话另行处理，避免与其它任务冲突；
- 错误统一走 `ApiResponse(ok=False, message=...)`，不抛 5xx，便于小程序 utils/request.ts 直接展示；
- 大小校验先于读完 body，避免下游 Pillow OOM；
- 接口本身只做"识别 + 位置推荐"预览，**不直接写 InventoryItem**，用户点确认后再走 `/inventory` POST。
"""

from __future__ import annotations

import logging

from fastapi import APIRouter, Depends, File, Form, UploadFile
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.config import settings
from app.core.deps import get_active_home, get_current_user
from app.db.session import get_db
from app.models.home import Home
from app.models.user import User
from app.schemas.common import ApiResponse
from app.schemas.scan import ScanCandidate, ScanResult
from app.services.vision_service import analyze_food_image

router = APIRouter()
logger = logging.getLogger(__name__)


def _empty_result(message: str, model_used: str | None = None) -> ApiResponse[ScanResult]:
    """统一构造一个空 ScanResult 的失败响应，避免前端解 data 时 None 检查。"""
    return ApiResponse[ScanResult](
        ok=False,
        message=message,
        data=ScanResult(
            candidates=[],
            raw_text=None,
            model_used=model_used or settings.vision_model,
        ),
    )


@router.post("/inventory/scan", response_model=ApiResponse[ScanResult])
async def inventory_scan(
    file: UploadFile = File(...),
    zone: str | None = Form(default=None),
    user: User = Depends(get_current_user),
    home: Home = Depends(get_active_home),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[ScanResult]:
    """接收手机端上传的食材照片，返回识别候选 + 推荐位置。

    Form 字段：
    - file: 上传的图片（必填，jpg/png/webp 等 Pillow 能读的都可以）；
    - zone: 用户当前所在的"区域 tab"提示，仅做提示用，最终位置仍由服务端推荐器决定。
    """
    # 1. key 未配置直接快速失败，告诉用户去配置环境变量。
    if not settings.siliconflow_api_key:
        return _empty_result(
            "vision service not configured, set SILICONFLOW_API_KEY"
        )

    # 2. 大小校验。UploadFile.size 在 starlette ≥0.27 上由 multipart 解析时填好，
    #    但仍然保留读后再校验的兜底，防止前端篡改 Content-Length。
    declared_size = getattr(file, "size", None)
    if declared_size is not None and declared_size > settings.inventory_scan_max_bytes:
        return _empty_result("image too large")

    # 3. 读 bytes。一次性读到内存即可，已经限了 5 MB 不会爆。
    image_bytes = await file.read()
    if not image_bytes:
        return _empty_result("empty image")
    if len(image_bytes) > settings.inventory_scan_max_bytes:
        return _empty_result("image too large")

    # 4. 调 vision_service。任何异常（图片解析失败、模型 HTTP 错误、JSON 解析失败等）
    #    都收敛为 ApiResponse(ok=False)，错误信息透出给前端但不带 key / 长堆栈。
    try:
        result = await analyze_food_image(
            image_bytes=image_bytes,
            hint_zone=zone,
            db=db,
            home_id=home.id,
        )
    except RuntimeError as exc:
        logger.warning(
            "inventory_scan failed: user=%s home=%s err=%s",
            user.id,
            home.id,
            exc,
        )
        # 失败时仍带回 model_used，让前端能区分"模型未命中"与"网络挂了"。
        return _empty_result(f"vision failed: {exc}")
    except Exception as exc:  # noqa: BLE001 -- 兜底，避免 500 直泄到小程序。
        logger.exception(
            "inventory_scan unexpected error: user=%s home=%s", user.id, home.id
        )
        return _empty_result(f"internal error: {exc.__class__.__name__}")

    # 5. 即便模型识别到 0 个食材，依然返 ok=True，data.candidates=[]，
    #    让前端展示"未识别到食材，请重拍/手动添加"。
    if not result.candidates:
        return ApiResponse[ScanResult](
            ok=True,
            message="no food detected",
            data=result,
        )

    return ApiResponse[ScanResult](data=result)


__all__ = ["router", "ScanCandidate"]
