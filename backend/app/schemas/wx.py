"""微信登录相关的请求 / 响应 schema。

外层统一由 ApiResponse[T] 包裹，因此这里的业务模型不再带 ok 字段。
LoginData 是新版登录返回体，比旧版 WxLoginResponse 多了 hasBoundDevice，
方便小程序首屏在登录之后直接判断要不要跳"绑定设备"页。
"""

from uuid import UUID

from pydantic import Field

from app.schemas.common import CamelModel


class WxLoginRequest(CamelModel):
    """小程序换 openid 时 POST 的请求体。"""

    # code 由小程序 wx.login() 拿到，几分钟内有效。
    code: str = Field(min_length=1, max_length=256)
    # 昵称、头像可选；缺省时后端不会强行写入空字符串。
    nickname: str | None = Field(default=None, max_length=80)
    avatar_url: str | None = Field(default=None, max_length=500)


class LoginData(CamelModel):
    """登录成功后的业务数据（外层 ApiResponse[LoginData] 包壳）。

    - hasBoundDevice：当前用户的活跃家庭里是否已有任何 active 绑定设备，
      小程序据此决定是否引导进绑定页。
    """

    user_id: UUID
    openid: str
    unionid: str | None = None
    access_token: str
    expires_in: int
    is_placeholder_session: bool = False
    has_bound_device: bool = False


# 旧版兼容：保留 WxLoginResponse 类名以便老 import 不破，但不再带 ok 字段。
# 真实使用现在已经统一走 ApiResponse[LoginData]。
class WxLoginResponse(CamelModel):
    """兼容旧 import 名称的登录响应壳（即将退役）。"""

    user_id: UUID
    openid: str
    unionid: str | None = None
    access_token: str
    expires_in: int
    is_placeholder_session: bool = False
    has_bound_device: bool = False
