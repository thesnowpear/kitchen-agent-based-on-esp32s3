"""新版 /auth/wechat-login 路由。

设计目标：
- 把"小程序 code2Session → upsert User → 签发 JWT"这条核心流程独立成 service，
  让旧版 /wx/login 和新版 /auth/wechat-login 共享一份实现，避免双写。
- 同时返回 hasBoundDevice，让小程序首屏判断是否引导绑定页。
- 统一外层 ApiResponse[LoginData] 包壳。
"""

from datetime import datetime, timedelta, timezone

import httpx
from fastapi import APIRouter, Depends, HTTPException
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.config import settings
from app.db.session import get_db
from app.models.device import DeviceBinding
from app.models.home import Home, HomeMember
from app.models.user import User, WxSession
from app.schemas.auth import LoginData, WechatLoginRequest
from app.schemas.common import ApiResponse
from app.services.token import issue_access_token
from app.services.wx_auth import wx_auth_service

router = APIRouter()


async def perform_wechat_login(payload: WechatLoginRequest, db: AsyncSession) -> LoginData:
    """微信登录的核心实现，路由层只负责 HTTP 包壳。

    步骤：
    1. 调 wx_auth_service.code2session 拿 openid（未配置 appid/secret 时返回 placeholder demo openid）。
    2. 按 openid upsert users 表；nickname/avatar 只在新建时写入，避免覆盖用户后续修改。
    3. 写入一条 wx_sessions 记录，附带 ttl 过期时间，便于审计。
    4. 签发 HS256 JWT；payload 仅 sub + openid + iat + exp。
    5. 查询当前用户的活跃家庭里是否有任意 active 设备绑定，决定 hasBoundDevice。
    """
    # 1) 微信换 openid，网络错误统一抛 502。
    try:
        wx_session = await wx_auth_service.code2session(payload.code)
    except (httpx.HTTPError, ValueError) as exc:
        raise HTTPException(status_code=502, detail=str(exc)) from exc

    # 2) upsert 用户主表：openid 唯一，命中老用户则保留原昵称/头像。
    result = await db.execute(select(User).where(User.primary_openid == wx_session.openid))
    user = result.scalar_one_or_none()
    if user is None:
        user = User(
            primary_openid=wx_session.openid,
            display_name=payload.nickname,
            avatar_url=payload.avatar_url,
        )
        db.add(user)
        await db.flush()

    # 3) 写入会话记录（不存 session_key 明文）。
    expires_at = datetime.now(timezone.utc) + timedelta(seconds=settings.access_token_ttl_seconds)
    db.add(
        WxSession(
            user_id=user.id,
            openid=wx_session.openid,
            unionid=wx_session.unionid,
            session_key_ciphertext=None,
            expires_at=expires_at,
        )
    )

    # 4) 签发 JWT。注意：这里必须在 commit 之前完成 user.id 写入（flush 已经写入）。
    access_token = issue_access_token(user.id, wx_session.openid)

    # 5) 判断 hasBoundDevice：用户所在的活跃家庭里是否已经有任何 active 绑定。
    has_bound_device = await _user_has_bound_device(db, user_id=user.id)

    await db.commit()

    return LoginData(
        user_id=user.id,
        openid=wx_session.openid,
        unionid=wx_session.unionid,
        access_token=access_token,
        expires_in=settings.access_token_ttl_seconds,
        is_placeholder_session=wx_session.is_placeholder,
        has_bound_device=has_bound_device,
    )


async def _user_has_bound_device(db: AsyncSession, *, user_id) -> bool:
    """判断用户名下任一 owner 家庭是否含 active 绑定。

    仅以"owner 角色"为口径；当前演示场景里其他角色暂不上线。
    没有家庭直接返回 False，让小程序首屏引导绑定。
    """
    home_id_row = await db.execute(
        select(Home.id)
        .join(HomeMember, HomeMember.home_id == Home.id)
        .where(HomeMember.user_id == user_id, HomeMember.role == "owner")
        .order_by(Home.created_at.asc())
        .limit(1)
    )
    home_id = home_id_row.scalar_one_or_none()
    if home_id is None:
        return False

    binding_row = await db.execute(
        select(DeviceBinding.id)
        .where(DeviceBinding.home_id == home_id, DeviceBinding.status == "active")
        .limit(1)
    )
    return binding_row.scalar_one_or_none() is not None


@router.post("/wechat-login", response_model=ApiResponse[LoginData])
async def wechat_login(
    payload: WechatLoginRequest,
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[LoginData]:
    """新版小程序登录入口。"""
    data = await perform_wechat_login(payload, db)
    return ApiResponse[LoginData](data=data)


__all__ = ["router", "perform_wechat_login"]
