"""Access token helpers (HS256 JWT, development-grade)."""

from datetime import datetime, timedelta, timezone
from uuid import UUID

from jose import JWTError, jwt

from app.core.config import settings


# 当前 token 仍是开发期级别：HS256 + 单一 secret + 无刷新机制。
# 生产化前需要：换非对称密钥、加 refresh token、把 secret 放进 KMS/Vault。


def issue_access_token(user_id: UUID, openid: str) -> str:
    """签发 HS256 JWT。

    payload 仅放最少字段，避免把可识别信息（昵称、头像）写进 token；
    令牌中也不放 wx_session.session_key 之类的敏感凭证。
    """
    now = datetime.now(timezone.utc)
    payload = {
        "sub": str(user_id),
        "openid": openid,
        "iat": int(now.timestamp()),
        "exp": int((now + timedelta(seconds=settings.access_token_ttl_seconds)).timestamp()),
    }
    return jwt.encode(payload, settings.jwt_secret, algorithm="HS256")


def decode_access_token(token: str) -> dict:
    """解码并验证 access token；失败统一抛 JWTError 由调用方转 HTTP 401。"""
    return jwt.decode(token, settings.jwt_secret, algorithms=["HS256"])


# 保留旧 API 名称供既有 routes 暂时引用；task #5 会把所有调用点切到 issue_access_token。
def issue_dev_access_token(user_id: UUID, openid: str) -> str:
    """已弃用：保留是为了让 task #5 完成路由迁移之前老路由仍能编译运行。"""
    return issue_access_token(user_id, openid)


__all__ = ["issue_access_token", "decode_access_token", "issue_dev_access_token", "JWTError"]
