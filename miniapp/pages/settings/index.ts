/**
 * 设置页：4 个分组
 *   1) API 地址：baseUrl / mock 兜底开关 → 本地 storage；
 *   2) 隐私偏好：3 个开关 → PUT /settings（privacy 字段）；
 *   3) AI 配置：apiBaseUrl / model / systemPrompt / timeoutMs / apiKey → POST /ai/config（经后端映射到设备 NVS）；
 *   4) 设备操作：刷新冰箱（POST /inventory/refresh）+ 解绑（本期占位，未真正实现）。
 */

import type { MiniAppInstance } from "../../app";
import { DEFAULT_API_CONFIG } from "../../config/env";
import {
  getAiConfig,
  getSettings,
  refreshInventory,
  subscribeNotification,
  updateAiConfig,
  updateSettings,
} from "../../services/api";
import type {
  AiConfigData,
  AiConfigUpdateRequest,
  ApiConfig,
  PrivacySettings,
  FridgeZoneConfig,
} from "../../types/models";
import { RequestError } from "../../utils/request";
import { getStorage, setStorage } from "../../utils/storage";
import { addCustomFridgeZone, deleteCustomFridgeZone, getFridgeZones } from "../../utils/fridgeZones";

const DEFAULT_PRIVACY: PrivacySettings = {
  allowCloudSync: true,
  allowUsageDiagnostics: false,
  allowReminderPush: true,
};

const DEFAULT_AI_PROFILE_NAME = "默认配置";
const DEFAULT_AI_MODEL = "gpt-4o-mini";
const DEFAULT_AI_TIMEOUT_MS = 30000;
const AI_SYSTEM_PROMPT_MAX_BYTES = 8192;
const DEFAULT_ASR_BASE_URL = "https://api.siliconflow.cn/v1/audio/transcriptions";
const DEFAULT_ASR_MODEL = "TeleAI/TeleSpeechASR";
const DEFAULT_TTS_BASE_URL = "https://api.siliconflow.cn/v1/audio/speech";
const DEFAULT_TTS_MODEL = "fnlp/MOSS-TTSD-v0.5";
const DEFAULT_TTS_VOICE = "fnlp/MOSS-TTSD-v0.5:alex";
const DEFAULT_VOICE_TIMEOUT_MS = 45000;

function utf8ByteLength(text: string): number {
  let bytes = 0;
  for (let i = 0; i < text.length; i += 1) {
    const code = text.charCodeAt(i);
    if (code <= 0x7f) bytes += 1;
    else if (code <= 0x7ff) bytes += 2;
    else if (code >= 0xd800 && code <= 0xdbff) {
      bytes += 4;
      i += 1;
    } else bytes += 3;
  }
  return bytes;
}

function clampAiTimeoutMs(value: string | number): number {
  const parsed = typeof value === "number" ? value : parseInt(value, 10);
  if (!Number.isFinite(parsed)) return DEFAULT_AI_TIMEOUT_MS;
  return Math.min(60000, Math.max(5000, parsed));
}

function clampAsrTimeoutMs(value: string | number): number {
  const parsed = typeof value === "number" ? value : parseInt(value, 10);
  if (!Number.isFinite(parsed)) return DEFAULT_VOICE_TIMEOUT_MS;
  return Math.min(90000, Math.max(10000, parsed));
}

function clampTtsTimeoutMs(value: string | number): number {
  const parsed = typeof value === "number" ? value : parseInt(value, 10);
  if (!Number.isFinite(parsed)) return DEFAULT_VOICE_TIMEOUT_MS;
  return Math.min(90000, Math.max(5000, parsed));
}

Page({
  data: {
    /** API 区 */
    baseUrl: DEFAULT_API_CONFIG.baseUrl,
    mockEnabled: DEFAULT_API_CONFIG.mockEnabled,

    /** 隐私区 */
    privacy: { ...DEFAULT_PRIVACY } as PrivacySettings,
    savingPrivacy: false,
    reminderLeadDays: "3",
    lowStockThreshold: "1",
    savingPreferences: false,
    subscribing: false,

    /** AI 配置区 */
    aiLoading: false,
    aiSaving: false,
    aiProfileName: DEFAULT_AI_PROFILE_NAME,
    aiBaseUrl: "",
    aiModel: DEFAULT_AI_MODEL,
    aiSystemPrompt: "",
    aiSystemPromptBytes: 0,
    aiTimeoutMs: String(DEFAULT_AI_TIMEOUT_MS),
    aiVisionModel: "",
    aiKeyPreview: "",
    aiHasKey: false,
    aiReady: false,
    aiLastError: "",
    aiSource: "",
    aiNewKey: "", // 用户新输入的 apiKey，绑定 input；"" 表示不修改
    asrBaseUrl: DEFAULT_ASR_BASE_URL,
    asrModel: DEFAULT_ASR_MODEL,
    asrTimeoutMs: String(DEFAULT_VOICE_TIMEOUT_MS),
    asrKeyPreview: "",
    asrHasKey: false,
    asrNewKey: "",
    ttsBaseUrl: DEFAULT_TTS_BASE_URL,
    ttsModel: DEFAULT_TTS_MODEL,
    ttsVoice: DEFAULT_TTS_VOICE,
    ttsTimeoutMs: String(DEFAULT_VOICE_TIMEOUT_MS),
    ttsKeyPreview: "",
    ttsHasKey: false,
    ttsNewKey: "",

    /** 设备操作区 */
    refreshing: false,
    zones: [] as FridgeZoneConfig[],
    newZoneName: "",
  },

  async onShow() {
    // 同步自定义 tabBar 高亮（设置 = 索引 3）
    const tabBar = (this.getTabBar?.() as unknown) as { setData: (d: { selected: number }) => void } | undefined;
    if (tabBar) tabBar.setData({ selected: 3 });

    const config = getStorage<ApiConfig>("apiConfig") || DEFAULT_API_CONFIG;
    this.setData({
      baseUrl: config.baseUrl,
      mockEnabled: config.mockEnabled,
    });
    await Promise.all([this.loadPrivacy(), this.loadAiConfig()]);
    this.setData({ zones: getFridgeZones() });
  },

  // ---------- API 地址 ----------

  onBaseUrlInput(event: WechatMiniprogram.Input) {
    this.setData({ baseUrl: String(event.detail.value || "").trim() });
  },

  onMockChange(event: WechatMiniprogram.SwitchChange) {
    this.setData({ mockEnabled: !!event.detail.value });
  },

  saveApiConfig() {
    if (!this.data.baseUrl) {
      wx.showToast({ title: "请输入 API 地址", icon: "none" });
      return;
    }
    const config: ApiConfig = {
      baseUrl: this.data.baseUrl,
      timeoutMs: DEFAULT_API_CONFIG.timeoutMs,
      mockEnabled: this.data.mockEnabled,
    };
    const app = getApp<MiniAppInstance["globalData"]>() as unknown as MiniAppInstance;
    app.setApiConfig(config);
    wx.showToast({ title: "API 已更新", icon: "success" });
  },

  // ---------- 隐私 ----------

  async loadPrivacy() {
    try {
      const data = await getSettings();
      const next: PrivacySettings = {
        ...DEFAULT_PRIVACY,
        ...(data.privacy || {}),
      };
      const prefs = data.preferences || {};
      this.setData({
        privacy: next,
        reminderLeadDays: String(prefs.reminderLeadDays ?? "3"),
        lowStockThreshold: String(prefs.lowStockThreshold ?? "1"),
      });
      setStorage("privacySettings", next);
    } catch {
      const cached =
        getStorage<PrivacySettings>("privacySettings") || DEFAULT_PRIVACY;
      this.setData({ privacy: cached });
    }
  },

  onPrivacyChange(event: WechatMiniprogram.SwitchChange) {
    const key = (event.currentTarget.dataset.key as keyof PrivacySettings) ?? "";
    if (!key) return;
    this.setData({
      privacy: { ...this.data.privacy, [key]: !!event.detail.value },
    });
  },

  async savePrivacy() {
    if (this.data.savingPrivacy) return;
    this.setData({ savingPrivacy: true });
    setStorage("privacySettings", this.data.privacy);
    try {
      await updateSettings({ privacy: this.data.privacy });
      wx.showToast({ title: "隐私已同步", icon: "success" });
    } catch {
      wx.showToast({ title: "已保存到本地", icon: "none" });
    } finally {
      this.setData({ savingPrivacy: false });
    }
  },

  onPreferenceInput(event: WechatMiniprogram.Input) {
    const field = (event.currentTarget.dataset.field as string) || "";
    const value = String(event.detail.value || "").trim();
    if (field === "reminderLeadDays") this.setData({ reminderLeadDays: value });
    if (field === "lowStockThreshold") this.setData({ lowStockThreshold: value });
  },

  async savePreferences() {
    if (this.data.savingPreferences) return;
    this.setData({ savingPreferences: true });
    try {
      await updateSettings({
        preferences: {
          reminderLeadDays: parseInt(this.data.reminderLeadDays, 10) || 3,
          lowStockThreshold: parseInt(this.data.lowStockThreshold, 10) || 1,
        },
      });
      wx.showToast({ title: "提醒偏好已保存", icon: "success" });
    } catch {
      wx.showToast({ title: "保存失败", icon: "none" });
    } finally {
      this.setData({ savingPreferences: false });
    }
  },

  async requestNotificationSubscribe() {
    if (this.data.subscribing) return;
    const app = getApp<MiniAppInstance["globalData"]>() as unknown as MiniAppInstance;
    if (!app.globalData.session?.userId) {
      wx.showToast({ title: "请先完成登录", icon: "none" });
      return;
    }
    this.setData({ subscribing: true });
    try {
      const target = "fridge_reminder_demo";
      await subscribeNotification({
        userId: app.globalData.session.userId,
        homeId: app.globalData.activeHome?.homeId !== "default" ? app.globalData.activeHome?.homeId : undefined,
        channel: "wechat",
        target,
        extra: {
          allowReminderPush: this.data.privacy.allowReminderPush,
          source: "miniapp-settings",
        },
      });
      wx.showToast({ title: "订阅登记已保存", icon: "success" });
    } catch {
      wx.showToast({ title: "订阅登记失败", icon: "none" });
    } finally {
      this.setData({ subscribing: false });
    }
  },

  // ---------- AI 配置 ----------

  async loadAiConfig() {
    if (this.data.aiLoading) return;
    this.setData({ aiLoading: true });
    try {
      const cfg: AiConfigData = await getAiConfig();
      const model = cfg.model || cfg.chatModel || DEFAULT_AI_MODEL;
      const systemPrompt = cfg.systemPrompt || "";
      const ready = cfg.ready ?? Boolean(cfg.apiBaseUrl && model && cfg.hasApiKey);
      this.setData({
        aiProfileName: cfg.profileName || DEFAULT_AI_PROFILE_NAME,
        aiBaseUrl: cfg.apiBaseUrl || "",
        aiModel: model,
        aiSystemPrompt: systemPrompt,
        aiSystemPromptBytes: utf8ByteLength(systemPrompt),
        aiTimeoutMs: String(clampAiTimeoutMs(cfg.timeoutMs || DEFAULT_AI_TIMEOUT_MS)),
        aiVisionModel: cfg.visionModel || "",
        aiKeyPreview: cfg.apiKeyPreview || "",
        aiHasKey: cfg.hasApiKey,
        aiReady: ready,
        aiLastError: cfg.lastError || "",
        aiSource: cfg.source || "",
        asrBaseUrl: cfg.asrApiBaseUrl || DEFAULT_ASR_BASE_URL,
        asrModel: cfg.asrModel || DEFAULT_ASR_MODEL,
        asrTimeoutMs: String(clampAsrTimeoutMs(cfg.asrTimeoutMs || DEFAULT_VOICE_TIMEOUT_MS)),
        asrKeyPreview: cfg.asrApiKeyPreview || "",
        asrHasKey: cfg.asrHasApiKey ?? cfg.hasApiKey,
        ttsBaseUrl: cfg.ttsApiBaseUrl || DEFAULT_TTS_BASE_URL,
        ttsModel: cfg.ttsModel || DEFAULT_TTS_MODEL,
        ttsVoice: cfg.ttsVoice || DEFAULT_TTS_VOICE,
        ttsTimeoutMs: String(clampTtsTimeoutMs(cfg.ttsTimeoutMs || DEFAULT_VOICE_TIMEOUT_MS)),
        ttsKeyPreview: cfg.ttsApiKeyPreview || "",
        ttsHasKey: cfg.ttsHasApiKey ?? cfg.hasApiKey,
      });
    } catch (err) {
      // backend 不可达时静默：用户也不能在 AI 板块做改动
    } finally {
      this.setData({ aiLoading: false });
    }
  },

  onAiInput(event: WechatMiniprogram.Input) {
    const field = (event.currentTarget.dataset.field as string) || "";
    const rawValue = String(event.detail.value || "");
    const value = rawValue.trim();
    const patch: Record<string, string | number> = {};
    if (field === "profileName") patch.aiProfileName = value;
    if (field === "baseUrl") patch.aiBaseUrl = value;
    if (field === "model") patch.aiModel = value;
    if (field === "visionModel") patch.aiVisionModel = value;
    if (field === "newKey") patch.aiNewKey = value;
    if (field === "timeoutMs") patch.aiTimeoutMs = value;
    if (field === "asrBaseUrl") patch.asrBaseUrl = value;
    if (field === "asrModel") patch.asrModel = value;
    if (field === "asrTimeoutMs") patch.asrTimeoutMs = value;
    if (field === "asrNewKey") patch.asrNewKey = value;
    if (field === "ttsBaseUrl") patch.ttsBaseUrl = value;
    if (field === "ttsModel") patch.ttsModel = value;
    if (field === "ttsVoice") patch.ttsVoice = value;
    if (field === "ttsTimeoutMs") patch.ttsTimeoutMs = value;
    if (field === "ttsNewKey") patch.ttsNewKey = value;
    if (field === "systemPrompt") {
      patch.aiSystemPrompt = rawValue;
      patch.aiSystemPromptBytes = utf8ByteLength(rawValue);
    }
    this.setData(patch);
  },

  async saveAiConfig() {
    if (this.data.aiSaving) return;
    const systemPrompt = this.data.aiSystemPrompt || "";
    const systemPromptBytes = utf8ByteLength(systemPrompt);
    if (systemPromptBytes > AI_SYSTEM_PROMPT_MAX_BYTES) {
      wx.showToast({ title: "系统提示词过长", icon: "none" });
      this.setData({ aiSystemPromptBytes: systemPromptBytes });
      return;
    }
    this.setData({ aiSaving: true });
    const payload: AiConfigUpdateRequest = {};
    if (this.data.aiBaseUrl) payload.apiBaseUrl = this.data.aiBaseUrl;
    if (this.data.aiProfileName) payload.profileName = this.data.aiProfileName;
    if (this.data.aiModel) payload.chatModel = this.data.aiModel;
    payload.systemPrompt = systemPrompt;
    payload.timeoutMs = clampAiTimeoutMs(this.data.aiTimeoutMs);
    if (this.data.aiVisionModel) payload.visionModel = this.data.aiVisionModel;
    if (this.data.aiNewKey) payload.apiKey = this.data.aiNewKey;
    if (this.data.asrBaseUrl) payload.asrApiBaseUrl = this.data.asrBaseUrl;
    if (this.data.asrModel) payload.asrModel = this.data.asrModel;
    payload.asrTimeoutMs = clampAsrTimeoutMs(this.data.asrTimeoutMs);
    if (this.data.asrNewKey) payload.asrApiKey = this.data.asrNewKey;
    if (this.data.ttsBaseUrl) payload.ttsApiBaseUrl = this.data.ttsBaseUrl;
    if (this.data.ttsModel) payload.ttsModel = this.data.ttsModel;
    if (this.data.ttsVoice) payload.ttsVoice = this.data.ttsVoice;
    payload.ttsTimeoutMs = clampTtsTimeoutMs(this.data.ttsTimeoutMs);
    if (this.data.ttsNewKey) payload.ttsApiKey = this.data.ttsNewKey;
    try {
      const cfg = await updateAiConfig(payload);
      const model = cfg.model || cfg.chatModel || this.data.aiModel || DEFAULT_AI_MODEL;
      const nextSystemPrompt = cfg.systemPrompt ?? systemPrompt;
      const ready = cfg.ready ?? Boolean(cfg.apiBaseUrl && model && cfg.hasApiKey);
      this.setData({
        aiProfileName: cfg.profileName || this.data.aiProfileName || DEFAULT_AI_PROFILE_NAME,
        aiBaseUrl: cfg.apiBaseUrl || "",
        aiModel: model,
        aiSystemPrompt: nextSystemPrompt,
        aiSystemPromptBytes: utf8ByteLength(nextSystemPrompt),
        aiTimeoutMs: String(clampAiTimeoutMs(cfg.timeoutMs || payload.timeoutMs || DEFAULT_AI_TIMEOUT_MS)),
        aiVisionModel: cfg.visionModel || "",
        aiKeyPreview: cfg.apiKeyPreview || "",
        aiHasKey: cfg.hasApiKey,
        aiReady: ready,
        aiLastError: cfg.lastError || "",
        aiSource: cfg.source || "",
        aiNewKey: "",
        asrBaseUrl: cfg.asrApiBaseUrl || this.data.asrBaseUrl || DEFAULT_ASR_BASE_URL,
        asrModel: cfg.asrModel || this.data.asrModel || DEFAULT_ASR_MODEL,
        asrTimeoutMs: String(clampAsrTimeoutMs(cfg.asrTimeoutMs || payload.asrTimeoutMs || DEFAULT_VOICE_TIMEOUT_MS)),
        asrKeyPreview: cfg.asrApiKeyPreview || "",
        asrHasKey: cfg.asrHasApiKey ?? this.data.asrHasKey,
        asrNewKey: "",
        ttsBaseUrl: cfg.ttsApiBaseUrl || this.data.ttsBaseUrl || DEFAULT_TTS_BASE_URL,
        ttsModel: cfg.ttsModel || this.data.ttsModel || DEFAULT_TTS_MODEL,
        ttsVoice: cfg.ttsVoice || this.data.ttsVoice || DEFAULT_TTS_VOICE,
        ttsTimeoutMs: String(clampTtsTimeoutMs(cfg.ttsTimeoutMs || payload.ttsTimeoutMs || DEFAULT_VOICE_TIMEOUT_MS)),
        ttsKeyPreview: cfg.ttsApiKeyPreview || "",
        ttsHasKey: cfg.ttsHasApiKey ?? this.data.ttsHasKey,
        ttsNewKey: "",
      });
      wx.showToast({ title: "AI 配置已同步", icon: "success" });
    } catch (err) {
      const message =
        err instanceof RequestError ? err.message : "保存失败";
      wx.showToast({ title: message, icon: "none" });
    } finally {
      this.setData({ aiSaving: false });
    }
  },

  async clearAiKey() {
    if (this.data.aiSaving) return;
    this.setData({ aiSaving: true });
    try {
      // backend 约定：apiKey="" 表示清空。
      const cfg = await updateAiConfig({ apiKey: "" });
      const model = cfg.model || cfg.chatModel || this.data.aiModel || DEFAULT_AI_MODEL;
      this.setData({
        aiKeyPreview: cfg.apiKeyPreview || "",
        aiHasKey: cfg.hasApiKey,
        aiReady: cfg.ready ?? Boolean(cfg.apiBaseUrl && model && cfg.hasApiKey),
        aiLastError: cfg.lastError || "",
        aiNewKey: "",
      });
      wx.showToast({ title: "已清除 Key", icon: "success" });
    } catch {
      wx.showToast({ title: "清除失败", icon: "none" });
    } finally {
      this.setData({ aiSaving: false });
    }
  },

  async clearAsrKey() {
    if (this.data.aiSaving) return;
    this.setData({ aiSaving: true });
    try {
      const cfg = await updateAiConfig({ asrApiKey: "" });
      this.setData({
        asrKeyPreview: cfg.asrApiKeyPreview || "",
        asrHasKey: cfg.asrHasApiKey ?? false,
        asrNewKey: "",
      });
      wx.showToast({ title: "已清除 ASR Key", icon: "success" });
    } catch {
      wx.showToast({ title: "清除失败", icon: "none" });
    } finally {
      this.setData({ aiSaving: false });
    }
  },

  async clearTtsKey() {
    if (this.data.aiSaving) return;
    this.setData({ aiSaving: true });
    try {
      const cfg = await updateAiConfig({ ttsApiKey: "" });
      this.setData({
        ttsKeyPreview: cfg.ttsApiKeyPreview || "",
        ttsHasKey: cfg.ttsHasApiKey ?? false,
        ttsNewKey: "",
      });
      wx.showToast({ title: "已清除 TTS Key", icon: "success" });
    } catch {
      wx.showToast({ title: "清除失败", icon: "none" });
    } finally {
      this.setData({ aiSaving: false });
    }
  },

  // ---------- 设备操作 ----------

  async onTapRefreshDevice() {
    if (this.data.refreshing) return;
    this.setData({ refreshing: true });
    try {
      const r = await refreshInventory();
      wx.showToast({
        title: r.queued ? "刷新已发起" : "未连接设备",
        icon: r.queued ? "success" : "none",
      });
    } catch {
      wx.showToast({ title: "刷新失败", icon: "none" });
    } finally {
      this.setData({ refreshing: false });
    }
  },

  onZoneNameInput(event: WechatMiniprogram.Input) {
    this.setData({ newZoneName: String(event.detail.value || "") });
  },

  onAddZone() {
    const zones = addCustomFridgeZone(this.data.newZoneName || "");
    this.setData({ zones, newZoneName: "" });
    wx.showToast({ title: "已添加分区", icon: "success" });
  },

  onDeleteZone(event: WechatMiniprogram.BaseEvent) {
    const key = event.currentTarget.dataset.key as string;
    const target = this.data.zones.find((z) => z.key === key);
    if (!target?.custom) return;
    wx.showModal({
      title: "删除自定义分区",
      content: `删除"${target.label}"后，已归属该分区的库存不会被删除，但会显示为自定义 key。`,
      confirmText: "删除",
      confirmColor: "#d95745",
      success: (res) => {
        if (!res.confirm) return;
        this.setData({ zones: deleteCustomFridgeZone(key) });
      },
    });
  },

  /** 重置本地数据：清掉 token / overview 缓存，留下 apiConfig（用户改过的 baseUrl 不要丢），
   *  然后立刻触发一次静默 wx.login 重新签发 session。 */
  onResetLocal() {
    wx.showModal({
      title: "重置本地数据",
      content: "将清除当前会话与缓存，下一秒会自动重新登录。继续吗？",
      confirmText: "重置",
      confirmColor: "#d95745",
      success: async (res) => {
        if (!res.confirm) return;
        const app = getApp<MiniAppInstance["globalData"]>() as unknown as MiniAppInstance;
        app.setSession(null);
        app.setActiveDevice(null);
        app.setActiveHome(null);
        app.setOverview(null);
        try {
          await app.ensureSession(true);
          wx.showToast({ title: "已重置", icon: "success" });
        } catch {
          wx.showToast({ title: "登录失败", icon: "none" });
        }
        wx.reLaunch({ url: "/pages/home/index" });
      },
    });
  },
});
