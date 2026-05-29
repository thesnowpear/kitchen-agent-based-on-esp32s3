"""v1 路由总挂载入口。

挂载顺序遵循 plans/ui-reference-zazzy-candle.md §2.3：
- 旧路径（单数前缀）作为兼容 alias 保留；
- 新增 RESTful 复数前缀 / auth / settings / ai 路径，统一 ApiResponse[T] 外壳。
"""

from fastapi import APIRouter

from app.api.v1.routes import (
    ai,
    ai_config,
    auth,
    device,
    devices_alias,
    fridge_zone,
    home,
    inventory,
    mqtt,
    notification,
    reminder,
    reminders_alias,
    scan,
    settings_route,
    sync,
    wx,
)

api_router = APIRouter()

# wx 旧路径保留（POST /wx/login），新版小程序统一走 /auth/wechat-login。
api_router.include_router(wx.router, prefix="/wx", tags=["wx"])
api_router.include_router(auth.router, prefix="/auth", tags=["auth"])

# 家庭：仅 /home 单数前缀，承载 create + overview。
api_router.include_router(home.router, prefix="/home", tags=["home"])

# 设备：旧 /device（单数）保留兼容；新增 /devices（复数）做 RESTful。
api_router.include_router(device.router, prefix="/device", tags=["device"])
api_router.include_router(devices_alias.router, prefix="/devices", tags=["devices"])

# 库存：所有新版 + 老兼容 alias 都在同一个 inventory.router 里挂载到 /inventory。
api_router.include_router(inventory.router, prefix="/inventory", tags=["inventory"])

# 拍照识别：scan.router 内部已自带 /inventory/scan 完整路径，因此挂载时不加 prefix。
api_router.include_router(scan.router, tags=["scan"])

# 提醒：旧 /reminder 保留兼容；新增 /reminders 复数 RESTful。
api_router.include_router(reminder.router, prefix="/reminder", tags=["reminder"])
api_router.include_router(reminders_alias.router, prefix="/reminders", tags=["reminders"])

# 设置（用户级隐私 / 偏好）。
api_router.include_router(settings_route.router, prefix="/settings", tags=["settings"])

# 冰箱分区：家庭级配置，云端保存，小程序本地缓存。
api_router.include_router(fridge_zone.router, prefix="/fridge", tags=["fridge"])

# 三端同步：服务器备份、小程序离线队列和固件 MQTT 快照的统一入口。
api_router.include_router(sync.router, prefix="/sync", tags=["sync"])

# AI 对话：设备转发为主，云端 SiliconFlow 降级。
api_router.include_router(ai.router, prefix="/ai", tags=["ai"])

# AI 配置双向同步：/ai/config GET 读 + POST 写（含 MQTT 推送）。
api_router.include_router(ai_config.router, prefix="/ai", tags=["ai-config"])

# 内部 / 设备侧路由（本期不动）。
api_router.include_router(mqtt.router, prefix="/mqtt", tags=["mqtt"])
api_router.include_router(notification.router, prefix="/notification", tags=["notification"])
