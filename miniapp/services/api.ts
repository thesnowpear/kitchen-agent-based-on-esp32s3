/**
 * 业务 API 层：所有路径都不带 `/api/v1` 前缀，request 内部统一拼。
 * 字段命名与 backend CamelModel 输出一致，调用方拿到的对象直接可用。
 *
 * 接口分组：
 * 1. auth：wechat-login（无身份）；
 * 2. devices：primary / bind（含 DEMO 一键演示）；
 * 3. home：overview；
 * 4. inventory：列表 / 新增 / 编辑 / 删除 / 刷新 / 拍照识别；
 * 5. reminders：列表 / 确认；
 * 6. settings：读 / 写隐私偏好；
 * 7. ai：chat + config GET/POST；
 * 8. notification：微信订阅授权登记。
 */

import { LONG_REQUEST_TIMEOUT_MS } from "../config/env";
import type {
  AiChatHistoryClearData,
  AiChatHistoryData,
  AiChatResponseData,
  AiConfigData,
  AiConfigUpdateRequest,
  BindDevicePayload,
  DeviceSummary,
  FridgeZoneListData,
  FridgeZoneUpdateRequest,
  HomeOverview,
  InventoryListData,
  InventoryItem,
  InventoryWritePayload,
  LoginData,
  NotificationSubscribePayload,
  NotificationSubscribeResponse,
  RefreshData,
  ReminderItem,
  ReminderListData,
  ScanResult,
  SettingsUpdateRequest,
  SyncPullData,
  SyncDevicePushData,
  SyncDevicePushRequest,
  SyncPushData,
  SyncPushRequest,
  SyncSnapshotData,
  SyncStatusData,
  UserSettingsData,
} from "../types/models";
import { request, uploadFile } from "../utils/request";

// ---------- 1. 认证 ----------

/** 微信登录：传 wx.login() 返回的临时 code；本期 backend 用 demo openid 兜底。 */
export function loginWithWechatCode(
  code: string,
  nickname?: string,
  avatarUrl?: string,
): Promise<LoginData> {
  return request<LoginData>({
    path: "/auth/wechat-login",
    method: "POST",
    auth: false,
    data: { code, nickname, avatarUrl },
  });
}

// ---------- 2. 设备 ----------

/** 取当前 home 的首台 active 绑定设备；未绑定时返 null。 */
export function getPrimaryDevice(): Promise<DeviceSummary | null> {
  return request<DeviceSummary | null>({
    path: "/devices/primary",
  });
}

/** 绑定设备。bindCode 不区分大小写，"DEMO" → 一键绑定演示设备 DEMO-FRIDGE-001。 */
export function bindDevice(payload: BindDevicePayload): Promise<DeviceSummary> {
  return request<DeviceSummary>({
    path: "/devices/bind",
    method: "POST",
    data: payload,
  });
}

// ---------- 3. 首页 ----------

/** 首屏聚合：设备 + 库存计数 + 临期前 5 + 待办提醒数 + 最近同步时间。 */
export function getHomeOverview(): Promise<HomeOverview> {
  return request<HomeOverview>({
    path: "/home/overview",
  });
}

// ---------- 4. 库存 ----------

/** 取当前 home 全部活跃库存。 */
export function getInventory(): Promise<InventoryListData> {
  return request<InventoryListData>({
    path: "/inventory",
  });
}

/** 手动新增库存条目（编辑页提交 / scan 候选确认入库都走这条）。 */
export function createInventoryItem(
  payload: InventoryWritePayload,
): Promise<InventoryItem> {
  return request<InventoryItem, InventoryWritePayload>({
    path: "/inventory",
    method: "POST",
    data: payload,
  });
}

/** 局部更新库存条目（PUT，所有字段可选）。 */
export function updateInventoryItem(
  itemId: string,
  payload: InventoryWritePayload,
): Promise<InventoryItem> {
  return request<InventoryItem, InventoryWritePayload>({
    path: `/inventory/${itemId}`,
    method: "PUT",
    data: payload,
  });
}

/** 标记为删除（backend 软删除：status 改为 deleted）。 */
export function deleteInventoryItem(itemId: string): Promise<void> {
  return request<void>({
    path: `/inventory/${itemId}`,
    method: "DELETE",
  });
}

/** 触发设备端刷新（透传到 MQTT inventory_refresh 命令）。返 queued + nextRefreshAt。 */
export function refreshInventory(): Promise<RefreshData> {
  return request<RefreshData>({
    path: "/inventory/refresh",
    method: "POST",
  });
}

/** 上传图片走视觉识别。filePath 是 wx.chooseMedia 返回的临时路径。 */
export function scanFoodImage(opts: {
  filePath: string;
  hintZone?: string;
}): Promise<ScanResult> {
  return uploadFile<ScanResult>({
    path: "/inventory/scan",
    filePath: opts.filePath,
    fileFieldName: "file",
    formData: opts.hintZone ? { zone: opts.hintZone } : undefined,
    timeoutMs: LONG_REQUEST_TIMEOUT_MS,
  });
}

// ---------- 5. 提醒 ----------

export function getReminders(): Promise<ReminderListData> {
  return request<ReminderListData>({
    path: "/reminders",
  });
}

/** 确认 / 忽略提醒；status 默认 "acked"，可传 "dismissed"。 */
export function confirmReminder(
  reminderId: string,
  status: "acked" | "dismissed" = "acked",
): Promise<ReminderItem> {
  return request<ReminderItem>({
    path: `/reminders/${reminderId}/confirm`,
    method: "POST",
    data: { status },
  });
}

// ---------- 6. 设置 ----------

export function getSettings(): Promise<UserSettingsData> {
  return request<UserSettingsData>({
    path: "/settings",
  });
}

export function updateSettings(
  payload: SettingsUpdateRequest,
): Promise<UserSettingsData> {
  return request<UserSettingsData, SettingsUpdateRequest>({
    path: "/settings",
    method: "PUT",
    data: payload,
  });
}

// ---------- 7. AI ----------

/** 发送一句话给 AI；设备在线时通过 MQTT 转发到冰箱贴，离线时降级 SiliconFlow 云端。 */
export function sendAiChat(
  prompt: string,
  sessionId?: string,
): Promise<AiChatResponseData> {
  return request<AiChatResponseData>({
    path: "/ai/chat",
    method: "POST",
    data: { prompt, sessionId },
    // AI 链路最慢的路径：设备 MQTT 转发等 ack 最多 ai_device_timeout_seconds（默认 30s），
    // 失败后再走云端 60s 超时，所以这里设 90s 兜底。
    timeoutMs: 90_000,
  });
}

/** 读取云端 AI 对话历史；未传 sessionId 时使用后端默认会话。 */
export function getAiChatHistory(
  sessionId?: string,
): Promise<AiChatHistoryData> {
  const suffix = sessionId ? `?sessionId=${encodeURIComponent(sessionId)}` : "";
  return request<AiChatHistoryData>({
    path: `/ai/history${suffix}`,
  });
}

/** 清空云端 AI 对话历史；未传 sessionId 时清默认会话。 */
export function clearAiChatHistory(
  sessionId?: string,
): Promise<AiChatHistoryClearData> {
  const suffix = sessionId ? `?sessionId=${encodeURIComponent(sessionId)}` : "";
  return request<AiChatHistoryClearData>({
    path: `/ai/history${suffix}`,
    method: "DELETE",
  });
}

/** 读 AI 配置（不含明文 apiKey）。 */
export function getAiConfig(): Promise<AiConfigData> {
  return request<AiConfigData>({
    path: "/ai/config",
  });
}

/** 写 AI 配置；apiKey="" 表示清空；任一字段未传表示不修改。 */
export function updateAiConfig(
  payload: AiConfigUpdateRequest,
): Promise<AiConfigData> {
  return request<AiConfigData, AiConfigUpdateRequest>({
    path: "/ai/config",
    method: "POST",
    data: payload,
  });
}

// ---------- 7.5 冰箱分区 ----------

/** 读取家庭级冰箱分区配置；失败时调用方可回退本地缓存。 */
export function getFridgeZonesRemote(): Promise<FridgeZoneListData> {
  return request<FridgeZoneListData>({
    path: "/fridge/zones",
  });
}

/** 写入家庭级冰箱分区配置。 */
export function updateFridgeZonesRemote(
  payload: FridgeZoneUpdateRequest,
): Promise<FridgeZoneListData> {
  return request<FridgeZoneListData, FridgeZoneUpdateRequest>({
    path: "/fridge/zones",
    method: "PUT",
    data: payload,
  });
}

// ---------- 7.6 三端同步 ----------

export function getSyncStatus(): Promise<SyncStatusData> {
  return request<SyncStatusData>({
    path: "/sync/status",
  });
}

export function getSyncSnapshot(): Promise<SyncSnapshotData> {
  return request<SyncSnapshotData>({
    path: "/sync/snapshot",
  });
}

export function pullSyncChanges(sinceRevision: number): Promise<SyncPullData> {
  return request<SyncPullData>({
    path: `/sync/pull?sinceRevision=${encodeURIComponent(String(sinceRevision || 0))}`,
  });
}

export function pushSyncChanges(payload: SyncPushRequest): Promise<SyncPushData> {
  return request<SyncPushData, SyncPushRequest>({
    path: "/sync/push",
    method: "POST",
    data: payload,
  });
}

export function pushSyncSnapshotToDevice(
  payload: SyncDevicePushRequest = {},
): Promise<SyncDevicePushData> {
  return request<SyncDevicePushData, SyncDevicePushRequest>({
    path: "/sync/device-push",
    method: "POST",
    data: payload,
  });
}

// ---------- 8. 通知订阅 ----------

/** 保存微信订阅授权。后端当前路由不包 ApiResponse，request 层可兼容裸响应。 */
export function subscribeNotification(
  payload: NotificationSubscribePayload,
): Promise<NotificationSubscribeResponse> {
  return request<NotificationSubscribeResponse, NotificationSubscribePayload>({
    path: "/notification/subscribe",
    method: "POST",
    data: payload,
  });
}
