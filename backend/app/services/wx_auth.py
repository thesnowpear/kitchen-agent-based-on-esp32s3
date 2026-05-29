"""WeChat code2Session adapter."""

from dataclasses import dataclass
from hashlib import sha256

import httpx

from app.core.config import settings


@dataclass(frozen=True)
class Code2SessionResult:
    openid: str
    unionid: str | None = None
    session_key: str | None = None
    is_placeholder: bool = False


class WxAuthService:
    """微信登录服务。

    未配置 WX_APPID/WX_SECRET 时返回 demo openid，保证接口契约先可联调。
    """

    async def code2session(self, code: str) -> Code2SessionResult:
        if not settings.wx_appid or not settings.wx_secret:
            digest = sha256(code.encode("utf-8")).hexdigest()[:24]
            return Code2SessionResult(openid=f"demo_openid_{digest}", is_placeholder=True)

        params = {
            "appid": settings.wx_appid,
            "secret": settings.wx_secret,
            "js_code": code,
            "grant_type": "authorization_code",
        }
        async with httpx.AsyncClient(timeout=10.0) as client:
            response = await client.get(settings.wx_code2session_url, params=params)
            response.raise_for_status()
            data = response.json()

        if data.get("errcode"):
            raise ValueError(f"wx code2session failed: {data.get('errmsg', data['errcode'])}")

        return Code2SessionResult(
            openid=data["openid"],
            unionid=data.get("unionid"),
            session_key=data.get("session_key"),
            is_placeholder=False,
        )


wx_auth_service = WxAuthService()
