"""旧版 /wx/login 路由（保留作为兼容 alias）。

实现已经迁移到 routes.auth.perform_wechat_login；这里只做一层薄包壳。
小程序新版本统一改走 POST /api/v1/auth/wechat-login。
"""

from fastapi import APIRouter, Depends
from sqlalchemy.ext.asyncio import AsyncSession

from app.api.v1.routes.auth import perform_wechat_login
from app.db.session import get_db
from app.schemas.auth import LoginData, WechatLoginRequest
from app.schemas.common import ApiResponse

router = APIRouter()


@router.post("/login", response_model=ApiResponse[LoginData])
async def wx_login(
    payload: WechatLoginRequest,
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[LoginData]:
    """旧版微信登录入口；行为等同 /auth/wechat-login。"""
    # 直接复用 auth.perform_wechat_login，确保两个路径行为一致、避免逻辑漂移。
    data = await perform_wechat_login(payload, db)
    return ApiResponse[LoginData](data=data)
