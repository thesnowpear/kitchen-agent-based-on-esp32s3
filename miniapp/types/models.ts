/**
 * 小程序类型层 —— 与 backend `app/schemas/*.py` 经过 CamelModel 自动 alias 后
 * 的 camelCase 输出**严格对齐**，避免再做客户端字段重命名。
 *
 * 命名约定：
 * - 所有响应体字段保持 camelCase；
 * - 时间戳全部用 ISO 字符串（backend 返回 datetime 时序列化为 ISO8601）；
 * - 可选字段（`?`）映射 backend 的 `| None`；
 * - 业务模型不包含 `ok` / `message` / `requestId`，外层 `ApiResponse<T>` 已包壳。
 */

/** wx.request 通用配置；本地持久化到 storage，重启小程序不丢。 */
export interface ApiConfig {
  /** backend 接口根，默认指向当前联调服务器。 */
  baseUrl: string;
  /** 单次请求超时（毫秒）。AI/Scan 类请求会在 request 层临时放大。 */
  timeoutMs: number;
  /** 启用本地 mock fallback：默认关；仅当 backend 不可达 + 用户在设置里手动打开时生效。 */
  mockEnabled: boolean;
}

/** 已登录会话；token 由 backend HS256 JWT 签发。 */
export interface AuthSession {
  /** Bearer 部分，去掉 "Bearer " 前缀。 */
  token: string;
  openid?: string;
  userId?: string;
  /** 会话过期时间（ISO 字符串）。本期不强制校验，由 backend 失效时 401 触发重登录。 */
  expiresAt?: string;
  /** 本期 demo openid 场景：占位会话。 */
  isPlaceholderSession?: boolean;
}

/** 统一响应外壳（backend `ApiResponse[T]`）。 */
export interface ApiResponse<T> {
  ok: boolean;
  data?: T;
  message?: string;
  requestId?: string;
}

/** 当前家庭对象（小程序全局状态用）。 */
export interface HomeInfo {
  homeId: string;
  name: string;
}

/** 登录响应体（backend `LoginData`）。 */
export interface LoginData {
  userId: string;
  openid: string;
  unionid?: string;
  accessToken: string;
  expiresIn: number;
  isPlaceholderSession: boolean;
  hasBoundDevice: boolean;
}

/** 设备状态枚举：在线 / 离线 / 未知。后端字符串透传，未明示也按 unknown 处理。 */
export type DeviceOnlineState = "online" | "offline" | "unknown";

/** 设备摘要（backend `DeviceSummary`）。 */
export interface DeviceSummary {
  /** 设备 UUID（backend 主键）。 */
  id: string;
  /** 设备 SN（与 ESP32 NVS device_id 一致；也是 MQTT topic 段）。 */
  deviceSn: string;
  name?: string;
  model?: string;
  firmwareVersion?: string;
  /** "online" / "offline" / "unknown" — 由 backend 根据 last_seen_at + 状态事件推断。 */
  status: DeviceOnlineState | string;
  /** ISO 字符串。 */
  lastSeenAt?: string;
}

/** 库存条目（backend `InventoryItemSchema`）。 */
export interface InventoryItem {
  id: string;
  name: string;
  category?: string;
  quantity: number;
  unit: string;
  /** "freezer" / "left" / "right" / "door" / "custom_*"；null 表示未分配。 */
  zone?: string;
  /** "A1"~"C3"；null 表示未分配。 */
  slot?: string;
  /** 可读位置文本，由 backend 服务层根据 zone+slot 自动渲染。 */
  location?: string;
  /** ISO 日期（YYYY-MM-DD）。 */
  expireDate?: string;
  /** "active" / "deleted" 等。 */
  status: string;
  /** "manual" / "scan" / "device" 等录入来源。 */
  source: string;
  /** 0~100 整数，识别置信度；手动添加为 null。 */
  confidence?: number;
  /** ISO 字符串：后端记录创建时间。 */
  createdAt?: string;
  updatedAt?: string;
  /** 扩展字段：用于保存 ui-reference 中的放入时间、备注等非核心库存字段。 */
  extra?: Record<string, unknown>;
}

/** 首页一次性聚合数据（backend `HomeOverview`）。 */
export interface HomeOverview {
  device?: DeviceSummary;
  inventoryCount: number;
  expiringCount: number;
  pendingReminderCount: number;
  /** 临期前 5 条，按到期日 asc。 */
  expiringList: InventoryItem[];
  lastSyncAt?: string;
}

/** 设备绑定请求（新版 `BindRequest`）。bindCode 不区分大小写，"DEMO" → 演示设备。 */
export interface BindDevicePayload {
  bindCode: string;
}

/** 提醒类型；与 backend ReminderSchema.reminder_type 透传一致。 */
export type ReminderType =
  | "expire_soon"
  | "low_stock"
  | "device_offline"
  | "scan_pending"
  | string;

/** 提醒状态：pending / acked / dismissed。 */
export type ReminderStatus = "pending" | "acked" | "dismissed" | string;

/** 提醒条目（backend `ReminderSchema`）。 */
export interface ReminderItem {
  id: string;
  reminderType: ReminderType;
  title: string;
  content?: string;
  status: ReminderStatus;
  dueAt?: string;
  ackedAt?: string;
}

/** 单条识别候选（backend `ScanCandidate`）。 */
export interface ScanCandidate {
  name: string;
  quantity: number;
  unit: string;
  category?: string;
  /** 服务端推荐 zone；null = 全部 zone 已满。 */
  suggestedZone?: string;
  /** 服务端推荐 slot；null = 推荐 zone 已满或无 zone。 */
  suggestedSlot?: string;
  /** 推荐原因（"同类食材优先合并" 等）。 */
  suggestedReason?: string;
  /** 0~100，模型置信度。 */
  confidence: number;
  note?: string;
}

/** /inventory/scan 响应（backend `ScanResult`）。 */
export interface ScanResult {
  candidates: ScanCandidate[];
  /** 模型原始输出截断 200 字，便于排错；不含 key/token。 */
  rawText?: string;
  modelUsed: string;
}

/** AI 对话单条消息（小程序内部维护，非 backend 结构）。 */
export interface AiChatMessage {
  id: string;
  role: "user" | "assistant";
  content: string;
  /** "device" 表示来自冰箱贴本地 AI；"cloud_fallback" 表示云端降级。 */
  source?: "device" | "cloud_fallback";
  /** source=cloud_fallback 时的降级原因（device_timeout / no_active_device / mqtt_disconnected / cloud_error: ...）。 */
  fallbackReason?: string;
  modelUsed?: string;
  deviceSn?: string;
  /** ISO 字符串，前端展示时间。 */
  sentAt: string;
}

/** /ai/chat 响应（backend `AiChatResponseData`）。 */
export interface AiChatResponseData {
  source: "device" | "cloud_fallback" | string;
  reply: string;
  fallbackReason?: string;
  modelUsed?: string;
  deviceSn?: string;
}

/** /ai/config GET 响应（backend `AiConfigData`，不含明文 apiKey）。 */
export interface AiConfigData {
  /** 设备端 Web Serial/USB 协议字段；后端旧响应可能只给 chatModel。 */
  profileId?: number;
  apiBaseUrl?: string;
  hasApiKey: boolean;
  apiKeyPreview?: string;
  /** 设备端字段名；与固件 NVS `model` 对齐。 */
  model?: string;
  /** 后端字段名；语义等同于设备端 model。 */
  chatModel?: string;
  /** 仅后端图片识别使用，不会推送到设备 NVS。 */
  visionModel?: string;
  systemPrompt?: string;
  timeoutMs: number;
  profileName: string;
  asrApiBaseUrl?: string;
  asrModel?: string;
  asrTimeoutMs?: number;
  asrHasApiKey?: boolean;
  asrApiKeyPreview?: string;
  ttsApiBaseUrl?: string;
  ttsModel?: string;
  ttsVoice?: string;
  ttsTimeoutMs?: number;
  ttsHasApiKey?: boolean;
  ttsApiKeyPreview?: string;
  configUpdatedAt?: string;
  ready?: boolean;
  lastError?: string;
  /** "miniapp" / "device" / "env_fallback" — 让前端展示当前配置来源。 */
  source: string;
}

/** /ai/config POST 请求（backend `AiConfigUpdateRequest`）。 */
export interface AiConfigUpdateRequest {
  apiBaseUrl?: string;
  /** "" 表示清空 apiKey；undefined 表示不修改。 */
  apiKey?: string;
  /** 预留给设备直连协议；当前后端仍使用 chatModel。 */
  model?: string;
  /** 设备端 model 在后端 schema 中叫 chatModel。 */
  chatModel?: string;
  /** 仅后端图片识别使用，不会推送到设备 NVS。 */
  visionModel?: string;
  systemPrompt?: string;
  timeoutMs?: number;
  profileName?: string;
  asrApiBaseUrl?: string;
  asrModel?: string;
  asrApiKey?: string;
  asrTimeoutMs?: number;
  ttsApiBaseUrl?: string;
  ttsModel?: string;
  ttsVoice?: string;
  ttsApiKey?: string;
  ttsTimeoutMs?: number;
}

/** 隐私设置（backend `PrivacySettings` + UserSettingsData.privacy 透传）。 */
export interface PrivacySettings {
  allowCloudSync: boolean;
  allowUsageDiagnostics: boolean;
  allowReminderPush: boolean;
  [key: string]: boolean;
}

/** /settings 响应（backend `UserSettingsData`）。 */
export interface UserSettingsData {
  privacy: PrivacySettings;
  preferences: Record<string, unknown>;
  updatedAt?: string;
}

/** /settings PUT 请求（backend `SettingsUpdateRequest`）。 */
export interface SettingsUpdateRequest {
  privacy?: Partial<PrivacySettings>;
  preferences?: Record<string, unknown>;
}

/** 库存新增 / 编辑请求（backend `InventoryUpdateRequest` / `InventoryPatchRequest` 简化版）。 */
export interface InventoryWritePayload {
  name?: string;
  category?: string;
  quantity?: number;
  unit?: string;
  zone?: string;
  slot?: string;
  location?: string;
  expireDate?: string;
  status?: string;
  source?: string;
  confidence?: number;
  extra?: Record<string, unknown>;
}

/** 库存列表响应（backend `InventoryListData`）。 */
export interface InventoryListData {
  items: InventoryItem[];
}

/** 提醒列表响应（backend `ReminderListData`）。 */
export interface ReminderListData {
  items: ReminderItem[];
}

/** /inventory/refresh 响应（backend `RefreshData`）。 */
export interface RefreshData {
  queued: boolean;
  nextRefreshAt?: string;
}

/** 离线页使用的本地快照。只缓存摘要，不保存图片或敏感 token。 */
export interface OfflineSnapshot {
  overview?: HomeOverview | null;
  inventoryItems: InventoryItem[];
  reminderItems: ReminderItem[];
  savedAt: number;
}

/** 本地购物清单条目。第一版不走后端，便于断网和 Demo 场景继续可用。 */
export interface ShoppingItem {
  id: string;
  name: string;
  quantityText: string;
  source: "ai" | "manual" | "expire" | "recipe";
  sourceText: string;
  checked: boolean;
  createdAt: number;
}

/** AI 菜谱里单个食材，missing=true 时可一键加入购物清单。 */
export interface RecipeIngredient {
  name: string;
  quantityText: string;
  missing?: boolean;
}

/** AI 菜谱推荐。rawText 保留模型原文，避免 JSON 解析失败时丢失信息。 */
export interface RecipeRecommendation {
  id: string;
  title: string;
  desc: string;
  durationText: string;
  tags: string[];
  ingredients: RecipeIngredient[];
  steps: string[];
  rawText?: string;
}

/** 冰箱分区配置：标准分区 + 用户自定义 custom_* 分区。 */
export interface FridgeZoneConfig {
  key: string;
  label: string;
  hint: string;
  custom: boolean;
  /** 首页编辑态使用：自定义空间在下方网格中占几列。 */
  width?: number;
  /** 首页编辑态使用：自定义空间在下方网格中占几行。 */
  height?: number;
}

/** 微信订阅/通知登记请求。 */
export interface NotificationSubscribePayload {
  userId: string;
  homeId?: string;
  channel: "wechat" | string;
  target: string;
  extra?: Record<string, unknown>;
}

/** 微信订阅/通知登记响应。 */
export interface NotificationSubscribeResponse {
  ok: boolean;
  subscriptionId: string;
  status: string;
}

/** 小程序内部派生：临期等级（不来自后端，前端按 expireDate 差天数推算）。 */
export type FreshnessLevel = 0 | 1 | 2;

/** 标准冰箱 zone（与 ui-reference 一致）。 */
export const STANDARD_ZONES = ["freezer", "left", "right", "door"] as const;
export type StandardZone = (typeof STANDARD_ZONES)[number];

/** 标准九宫格 slot 顺序，从左上到右下。 */
export const STANDARD_SLOTS = [
  "A1",
  "A2",
  "A3",
  "B1",
  "B2",
  "B3",
  "C1",
  "C2",
  "C3",
] as const;
export type StandardSlot = (typeof STANDARD_SLOTS)[number];
