"""可重复执行的同步表迁移脚本。

项目当前没有 Alembic，本脚本只创建缺失的同步表，不修改或删除已有业务数据。
运行方式：
    python backend/scripts/migrate_sync.py
"""

from __future__ import annotations

import asyncio
import sys
from pathlib import Path

from sqlalchemy import text

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from app.db.session import engine


DDL = [
    "CREATE EXTENSION IF NOT EXISTS pgcrypto",
    """
    CREATE TABLE IF NOT EXISTS sync_states (
        id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
        home_id UUID NOT NULL REFERENCES homes(id),
        current_revision BIGINT NOT NULL DEFAULT 0,
        last_source VARCHAR(32),
        last_client_event_id VARCHAR(128),
        created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
        updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
        CONSTRAINT uq_sync_states_home UNIQUE (home_id)
    )
    """,
    "CREATE INDEX IF NOT EXISTS ix_sync_states_home_id ON sync_states(home_id)",
    """
    CREATE TABLE IF NOT EXISTS sync_events (
        id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
        home_id UUID NOT NULL REFERENCES homes(id),
        user_id UUID REFERENCES users(id),
        device_id UUID REFERENCES devices(id),
        device_sn VARCHAR(80),
        client_event_id VARCHAR(128) NOT NULL,
        domain VARCHAR(64) NOT NULL,
        op VARCHAR(64) NOT NULL,
        source VARCHAR(32) NOT NULL DEFAULT 'miniapp',
        server_revision BIGINT NOT NULL,
        client_revision BIGINT,
        schema_version INTEGER NOT NULL DEFAULT 1,
        payload JSONB NOT NULL DEFAULT '{}'::jsonb,
        created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
        updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
        CONSTRAINT uq_sync_events_home_client_event UNIQUE (home_id, client_event_id)
    )
    """,
    "CREATE INDEX IF NOT EXISTS ix_sync_events_home_id ON sync_events(home_id)",
    "CREATE INDEX IF NOT EXISTS ix_sync_events_device_sn ON sync_events(device_sn)",
    "CREATE INDEX IF NOT EXISTS ix_sync_events_client_event_id ON sync_events(client_event_id)",
    "CREATE INDEX IF NOT EXISTS ix_sync_events_domain ON sync_events(domain)",
    "CREATE INDEX IF NOT EXISTS ix_sync_events_op ON sync_events(op)",
    "CREATE INDEX IF NOT EXISTS ix_sync_events_source ON sync_events(source)",
    "CREATE INDEX IF NOT EXISTS ix_sync_events_server_revision ON sync_events(server_revision)",
]


async def main() -> None:
    """按顺序执行幂等 DDL。"""
    async with engine.begin() as conn:
        for stmt in DDL:
            await conn.execute(text(stmt))
    await engine.dispose()


if __name__ == "__main__":
    asyncio.run(main())
