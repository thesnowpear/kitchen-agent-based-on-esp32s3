"""FastAPI dependency helpers: authentication and active home resolution."""

from uuid import UUID

from fastapi import Depends, Header, HTTPException, status
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.db.session import get_db
from app.models.home import Home, HomeMember
from app.models.user import User
from app.services.token import JWTError, decode_access_token


# 鉴权统一在依赖层完成，路由代码不再关心 Authorization 头解析。
# 失败时直接抛 401/404，与 ApiResponse 壳层无关（FastAPI 会自动序列化 HTTPException）。


async def get_current_user(
    authorization: str | None = Header(default=None, alias="Authorization"),
    db: AsyncSession = Depends(get_db),
) -> User:
    """从 Authorization: Bearer <token> 解析 user。"""
    if not authorization or not authorization.lower().startswith("bearer "):
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="missing bearer token")
    token = authorization.split(" ", 1)[1].strip()
    try:
        payload = decode_access_token(token)
    except JWTError as exc:
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail=f"invalid token: {exc}") from exc

    sub = payload.get("sub")
    if not sub:
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="token missing sub")

    try:
        user_id = UUID(sub)
    except ValueError as exc:
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="invalid user id in token") from exc

    user = await db.get(User, user_id)
    if user is None:
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="user not found")
    return user


async def get_active_home(
    user: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
) -> Home:
    """返回当前用户的"活跃家庭"。

    多家庭支持本期不做：取 owner 角色的第一条 home_members 记录对应的家庭。
    如果用户还没有任何家庭（例如首次登录的 demo 用户），自动创建一个名为"我的冰箱"
    的家庭并把当前用户登记为 owner，确保小程序首屏一键可用。
    """
    result = await db.execute(
        select(Home)
        .join(HomeMember, HomeMember.home_id == Home.id)
        .where(HomeMember.user_id == user.id, HomeMember.role == "owner")
        .order_by(Home.created_at.asc())
        .limit(1)
    )
    home = result.scalar_one_or_none()
    if home is not None:
        return home

    # 自动创建默认家庭，避免比赛演示阶段卡在"先创建家庭"这一步。
    home = Home(name="我的冰箱", owner_user_id=user.id)
    db.add(home)
    await db.flush()
    db.add(HomeMember(home_id=home.id, user_id=user.id, role="owner"))
    await db.commit()
    await db.refresh(home)
    return home


__all__ = ["get_current_user", "get_active_home"]
