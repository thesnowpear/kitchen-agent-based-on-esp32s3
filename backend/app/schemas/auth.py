"""新版 /auth 路由的请求 / 响应 schema。

POST /auth/wechat-login 与旧版 /wx/login 共享 LoginData 业务数据，
但请求体改名为 WechatLoginRequest，字段含义保持一致。
"""

from pydantic import Field

from app.schemas.common import CamelModel
from app.schemas.wx import LoginData  # noqa: F401  对外导出复用


class WechatLoginRequest(CamelModel):
    """小程序 wx.login 后调用 /auth/wechat-login 的请求体。"""

    # code：小程序 wx.login() 拿到的临时 code（一次性、5 分钟有效）。
    code: str = Field(min_length=1, max_length=256)
    # 可选：小程序拿到的用户昵称 / 头像；首次登录时写入 users 表。
    nickname: str | None = Field(default=None, max_length=80)
    avatar_url: str | None = Field(default=None, max_length=500)


__all__ = ["WechatLoginRequest", "LoginData"]
