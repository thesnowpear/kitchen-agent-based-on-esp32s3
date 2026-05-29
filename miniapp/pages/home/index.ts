/**
 * 首页 = 冰箱总览 + 设备控制中枢。
 *
 * 数据流：
 *   onShow
 *     ├─ 立刻读 globalData.lastOverview / activeDevice / lastLoginError 渲染骨架
 *     ├─ 调 ensureSession() 确保 token 在
 *     └─ 并行 refreshOverview() + refreshZoneCounts()
 *           - overview 拿到设备状态 / 总数 / 临期 list；
 *           - inventory 全量用来按 zone 计算每个分区的 count + warnCount，
 *             因为 backend HomeOverview 不带 zone-by-zone 的分布。
 *
 * 交互：
 *   - 顶部 app-status 点击展开 status-panel（设备 SN / 固件 / 刷新 / 换设备）；
 *   - 未绑定时直接给"一键绑定 DEMO"和"手动绑定"两条路径，不再强制跳 bind 页；
 *   - 临期 / 待办 / 总库存数字行点击会跳到对应 tab；
 *   - AI fab 跳 ai-chat 页。
 */

import type { MiniAppInstance } from "../../app";
import { DEMO_BIND_CODE } from "../../config/env";
import {
  bindDevice,
  getHomeOverview,
  getInventory,
  refreshInventory,
} from "../../services/api";
import type { HomeOverview, InventoryItem } from "../../types/models";
import { RequestError } from "../../utils/request";
import {
  addCustomFridgeZone,
  deleteCustomFridgeZone,
  displayPlace,
  getFridgeZones,
  updateCustomFridgeZone,
  updateStandardFridgeZone,
} from "../../utils/fridgeZones";
import { updateOfflineSnapshot } from "../../utils/localFeatures";

interface ZoneCardView {
  key: string;
  label: string;
  hint: string;
  custom: boolean;
  width?: number;
  height?: number;
  spanClass?: string;
  selected?: boolean;
  count: number;
  warnCount: number;
}

interface ExpiringView {
  id: string;
  name: string;
  quantityText: string;
  locationText: string;
  expireText: string;
  level: "warn" | "danger";
}

/** 计算某条目临期等级：<=1 天 danger，<=3 天 warn，其它 normal。 */
function freshness(item: InventoryItem): "normal" | "warn" | "danger" {
  if (!item.expireDate) return "normal";
  const today = new Date();
  today.setHours(0, 0, 0, 0);
  const target = new Date(item.expireDate + "T00:00:00");
  const diff = Math.round(
    (target.getTime() - today.getTime()) / (24 * 3600 * 1000),
  );
  if (diff <= 1) return "danger";
  if (diff <= 3) return "warn";
  return "normal";
}

Page({
  data: {
    /** UI 顶部 */
    clockText: "",
    greetingKicker: "你好呀",
    greetingTitle: "今天的冰箱",

    /** 设备状态 */
    hasDevice: false,
    deviceName: "未绑定设备",
    deviceSn: "",
    deviceFirmware: "",
    deviceStatus: "unknown" as string,
    deviceShortText: "未绑定",
    deviceStatusClass: "warn" as "online" | "offline" | "warn",
    statusTagClass: "warn" as string,
    lastSyncText: "尚未同步",

    /** 总数 / 临期 / 待办 */
    inventoryCount: 0,
    expiringCount: 0,
    reminderCount: 0,

    /** zone 卡片 + 临期清单 */
    standardZones: [] as ZoneCardView[],
    customZones: [] as ZoneCardView[],
    expiringList: [] as ExpiringView[],

    /** 错误态 */
    errorText: "",
    loginErrorText: "",

    /** loading 标记 */
    loading: false,
    binding: false,
    refreshing: false,

    /** 状态抽屉 */
    statusPanelOpen: false,
    spaceEditing: false,
    selectedZoneKey: "",
    selectedZoneLabel: "",
    selectedZoneHint: "",
    selectedZoneWidth: 1,
    selectedZoneHeight: 1,
    selectedZoneIsCustom: false,
  },

  /** 时钟更新句柄；离开时清掉 */
  clockTimer: 0,

  onLoad() {
    this.tickClock();
    this.clockTimer = setInterval(() => this.tickClock(), 30_000) as unknown as number;
  },

  onUnload() {
    if (this.clockTimer) {
      clearInterval(this.clockTimer);
      this.clockTimer = 0;
    }
    this.setTabBarHidden(false);
  },

  onHide() {
    this.setTabBarHidden(false);
  },

  onShow() {
    const app = getApp<MiniAppInstance["globalData"]>() as unknown as MiniAppInstance;

    // 同步自定义 tabBar 高亮（首页 = 索引 0）
    const tabBar = (this.getTabBar?.() as unknown) as { setData: (d: { selected: number; hidden: boolean }) => void } | undefined;
    if (tabBar) tabBar.setData({ selected: 0, hidden: false });

    // 1) 立刻用 globalData 缓存渲染（避免冷启动空白）
    if (app.globalData.lastOverview) {
      this.applyOverview(app.globalData.lastOverview);
    }
    if (app.globalData.activeDevice && !app.globalData.lastOverview) {
      this.applyDevice(app.globalData.activeDevice);
    }
    if (app.globalData.lastLoginError) {
      this.setData({ loginErrorText: app.globalData.lastLoginError });
    }
    this.applyZoneCounts([]);

    // 2) 异步 refresh：先确保 session，再并行拉数据
    void this.refresh();
  },

  onPullDownRefresh() {
    this.refresh().finally(() => wx.stopPullDownRefresh());
  },

  tickClock() {
    const now = new Date();
    const h = String(now.getHours()).padStart(2, "0");
    const m = String(now.getMinutes()).padStart(2, "0");
    this.setData({ clockText: `${h}:${m}` });

    // 顺便刷新问候语：基于一天中的不同时段。
    const hour = now.getHours();
    let kicker = "你好呀";
    if (hour < 6) kicker = "夜深了";
    else if (hour < 11) kicker = "早上好";
    else if (hour < 14) kicker = "中午好";
    else if (hour < 18) kicker = "下午好";
    else kicker = "晚上好";
    this.setData({ greetingKicker: kicker });
  },

  async refresh() {
    if (this.data.loading) return;
    this.setData({ loading: true, errorText: "" });

    const app = getApp<MiniAppInstance["globalData"]>() as unknown as MiniAppInstance;

    // 没 session 先静默登录；失败让 loginErrorText 暴露出来，停止后续。
    if (!app.globalData.session?.token) {
      const session = await app.ensureSession();
      if (!session?.token) {
        this.setData({
          loading: false,
          loginErrorText: app.globalData.lastLoginError || "未能完成静默登录",
        });
        return;
      }
    }
    this.setData({ loginErrorText: "" });

    try {
      // overview + inventory 并行
      const [overview, inventoryData] = await Promise.all([
        getHomeOverview(),
        getInventory().catch(() => ({ items: [] as InventoryItem[] })),
      ]);

      app.setOverview(overview);
      updateOfflineSnapshot({ overview, inventoryItems: inventoryData.items || [] });
      if (overview.device) {
        app.setActiveDevice(overview.device);
      }
      this.applyOverview(overview);
      this.applyZoneCounts(inventoryData.items || []);
    } catch (err) {
      const message =
        err instanceof RequestError
          ? err.message
          : err instanceof Error
            ? err.message
            : "无法连接到后端";
      this.setData({ errorText: message });
    } finally {
      this.setData({ loading: false });
    }
  },

  applyOverview(overview: HomeOverview) {
    const device = overview.device;
    this.applyDevice(device || null);

    this.setData({
      inventoryCount: overview.inventoryCount,
      expiringCount: overview.expiringCount,
      reminderCount: overview.pendingReminderCount,
      lastSyncText: overview.lastSyncAt
        ? this.humanizeTime(overview.lastSyncAt)
        : "尚未同步",
      expiringList: (overview.expiringList || []).map((it) => this.toExpiringView(it)),
    });
  },

  applyDevice(device: HomeOverview["device"] | null) {
    if (!device) {
      this.setData({
        hasDevice: false,
        deviceName: "未绑定设备",
        deviceSn: "",
        deviceFirmware: "",
        deviceStatus: "unknown",
        deviceShortText: "未绑定",
        deviceStatusClass: "warn",
        statusTagClass: "warn",
      });
      return;
    }
    const status = device.status || "unknown";
    const cls: "online" | "offline" | "warn" =
      status === "online" ? "online" : status === "offline" ? "offline" : "warn";
    const tagCls = status === "online" ? "mint" : status === "offline" ? "danger" : "warn";
    const shortText =
      status === "online" ? "在线" : status === "offline" ? "离线" : "未知";
    this.setData({
      hasDevice: true,
      deviceName: device.name || device.deviceSn || "冰箱贴",
      deviceSn: device.deviceSn || "",
      deviceFirmware: device.firmwareVersion || "",
      deviceStatus: status,
      deviceShortText: shortText,
      deviceStatusClass: cls,
      statusTagClass: tagCls,
    });
  },

  applyZoneCounts(items: InventoryItem[]) {
    const counts: Record<string, { count: number; warnCount: number }> = {};
    const zonesMeta = getFridgeZones();
    for (const meta of zonesMeta) {
      counts[meta.key] = { count: 0, warnCount: 0 };
    }
    for (const it of items) {
      if (!it.zone) continue;
      const bucket = counts[it.zone];
      if (!bucket) continue;
      bucket.count += 1;
      const lvl = freshness(it);
      if (lvl !== "normal") bucket.warnCount += 1;
    }
    const zones = zonesMeta.map((m) => ({
      ...m,
      count: counts[m.key]?.count ?? 0,
      warnCount: counts[m.key]?.warnCount ?? 0,
      width: m.width || 1,
      height: m.height || 1,
      spanClass: `span-w${m.width || 1}-h${m.height || 1}`,
      selected: m.key === this.data.selectedZoneKey,
      editable: true,
    }));
    this.setData({
      standardZones: zones.filter((z) => !z.custom),
      customZones: zones.filter((z) => z.custom),
    });
  },

  toExpiringView(item: InventoryItem): ExpiringView {
    const today = new Date();
    today.setHours(0, 0, 0, 0);
    let level: "warn" | "danger" = "warn";
    let expireText = "未设置到期";
    if (item.expireDate) {
      const target = new Date(item.expireDate + "T00:00:00");
      const diff = Math.round(
        (target.getTime() - today.getTime()) / (24 * 3600 * 1000),
      );
      if (diff <= 0) {
        expireText = "今天到期";
        level = "danger";
      } else if (diff === 1) {
        expireText = "明天到期";
        level = "danger";
      } else {
        expireText = `${diff} 天后到期`;
        level = diff <= 2 ? "danger" : "warn";
      }
    }
    const loc = item.location
      ? item.location
      : item.zone && item.slot
        ? displayPlace(item.zone, item.slot)
        : "未指定位置";
    return {
      id: item.id,
      name: item.name,
      quantityText: `${item.quantity}${item.unit}`,
      locationText: loc,
      expireText,
      level,
    };
  },

  humanizeTime(iso: string): string {
    try {
      const t = new Date(iso).getTime();
      const diffMin = Math.round((Date.now() - t) / 60_000);
      if (diffMin < 1) return "刚刚同步";
      if (diffMin < 60) return `${diffMin} 分钟前`;
      const diffH = Math.round(diffMin / 60);
      if (diffH < 24) return `${diffH} 小时前`;
      return iso.slice(0, 16).replace("T", " ");
    } catch {
      return iso;
    }
  },

  /** ---------- 交互 ---------- */

  setTabBarHidden(hidden: boolean) {
    const tabBar = (this.getTabBar?.() as unknown) as { setData: (d: { hidden: boolean }) => void } | undefined;
    if (tabBar) tabBar.setData({ hidden });
    const tabBarApi = wx as WechatMiniprogram.Wx & {
      hideTabBar?: (option?: { animation?: boolean; fail?: () => void }) => void;
      showTabBar?: (option?: { animation?: boolean; fail?: () => void }) => void;
    };
    const toggle = hidden ? tabBarApi.hideTabBar : tabBarApi.showTabBar;
    if (!toggle) return;
    toggle({ animation: false, fail: () => undefined });
  },

  onTapStatus() {
    if (!this.data.hasDevice) {
      this.onTapBindManual();
      return;
    }
    this.setTabBarHidden(true);
    this.setData({ statusPanelOpen: true });
  },

  onTapStatusClose() {
    this.setTabBarHidden(false);
    this.setData({ statusPanelOpen: false });
  },

  onStatusPanelTap() {
    // 阻止冒泡到 mask
  },

  onTapZone(event: WechatMiniprogram.BaseEvent) {
    const zoneKey = event.currentTarget.dataset.zone as string;
    const zone = [...this.data.standardZones, ...this.data.customZones].find((z) => z.key === zoneKey);
    if (this.data.spaceEditing) {
      if (zone) this.selectZone(zone);
      return;
    }
    const app = getApp<MiniAppInstance["globalData"]>() as unknown as MiniAppInstance;
    (app.globalData as MiniAppInstance["globalData"] & { pendingZone?: string }).pendingZone = zoneKey;
    // inventory 已不在 tabBar，改用 navigateTo
    wx.navigateTo({ url: "/pages/inventory/index" });
  },

  onTapInventoryAll() {
    const app = getApp<MiniAppInstance["globalData"]>() as unknown as MiniAppInstance;
    (app.globalData as MiniAppInstance["globalData"] & { pendingZone?: string }).pendingZone = "all";
    wx.navigateTo({ url: "/pages/inventory/index" });
  },

  onTapExpiring() {
    // 临期数字 / 临期 pill 都跳到提醒页（提醒页不是 tabBar，用 navigateTo）
    wx.navigateTo({ url: "/pages/reminders/index" });
  },

  onTapReminders() {
    wx.navigateTo({ url: "/pages/reminders/index" });
  },

  onTapExpiringItem(event: WechatMiniprogram.BaseEvent) {
    const id = event.currentTarget.dataset.id as string;
    wx.navigateTo({ url: `/pages/inventory-detail/index?id=${id}` });
  },

  onTapScan() {
    wx.switchTab({ url: "/pages/scan/index" });
  },

  onTapAi() {
    wx.switchTab({ url: "/pages/ai-chat/index" });
  },

  onTapSettings() {
    wx.switchTab({ url: "/pages/settings/index" });
  },

  toggleSpaceEditing() {
    const next = !this.data.spaceEditing;
    const firstZone = [...this.data.standardZones, ...this.data.customZones][0];
    this.setData({
      spaceEditing: next,
      selectedZoneKey: next ? this.data.selectedZoneKey : "",
      standardZones: this.data.standardZones.map((z) => ({ ...z, selected: false })),
      customZones: this.data.customZones.map((z) => ({ ...z, selected: false })),
    });
    if (next && firstZone && !this.data.selectedZoneKey) {
      this.selectZone(firstZone);
    }
  },

  onAddZone() {
    const zones = addCustomFridgeZone("");
    const added = zones.filter((z) => z.custom).slice(-1)[0];
    if (added) {
      this.selectZone({ ...added, count: 0, warnCount: 0 });
    }
    void this.refresh();
  },

  onSelectCustomZone(event: WechatMiniprogram.BaseEvent) {
    const key = event.currentTarget.dataset.zone as string;
    const zone = [...this.data.standardZones, ...this.data.customZones].find((z) => z.key === key);
    if (!zone) return;
    if (this.data.spaceEditing) {
      this.selectZone(zone);
      return;
    }
    const app = getApp<MiniAppInstance["globalData"]>() as unknown as MiniAppInstance;
    (app.globalData as MiniAppInstance["globalData"] & { pendingZone?: string }).pendingZone = key;
    wx.navigateTo({ url: "/pages/inventory/index" });
  },

  selectZone(zone: Partial<ZoneCardView>) {
    const key = zone.key || "";
    this.setData({
      selectedZoneKey: key,
      selectedZoneLabel: zone.label || "",
      selectedZoneHint: zone.hint || "自定义空间",
      selectedZoneWidth: zone.width || 1,
      selectedZoneHeight: zone.height || 1,
      selectedZoneIsCustom: !!zone.custom,
      customZones: this.data.customZones.map((z) => ({
        ...z,
        selected: z.key === key,
      })),
      standardZones: this.data.standardZones.map((z) => ({
        ...z,
        selected: z.key === key,
      })),
    });
  },

  onSelectedZoneInput(event: WechatMiniprogram.Input) {
    const field = event.currentTarget.dataset.field as string;
    const value = String(event.detail.value || "");
    if (field === "label") this.setData({ selectedZoneLabel: value });
    if (field === "hint") this.setData({ selectedZoneHint: value });
  },

  onSelectedZoneSize(event: WechatMiniprogram.BaseEvent & { detail: { value?: number } }) {
    const field = event.currentTarget.dataset.field as string;
    const value = Number(event.detail.value || 1);
    if (field === "width") this.setData({ selectedZoneWidth: value });
    if (field === "height") this.setData({ selectedZoneHeight: value });
    this.saveSelectedZonePatch({
      width: field === "width" ? value : this.data.selectedZoneWidth,
      height: field === "height" ? value : this.data.selectedZoneHeight,
    });
  },

  saveSelectedZoneText() {
    this.saveSelectedZonePatch({
      label: this.data.selectedZoneLabel.trim() || "自定义区",
      hint: this.data.selectedZoneHint.trim() || "自定义空间",
    });
  },

  saveSelectedZonePatch(patch: Parameters<typeof updateCustomFridgeZone>[1]) {
    if (!this.data.selectedZoneKey) return;
    const target = [...this.data.standardZones, ...this.data.customZones].find((z) => z.key === this.data.selectedZoneKey);
    if (!target) return;
    const updatedZones = target.custom
      ? updateCustomFridgeZone(this.data.selectedZoneKey, patch)
      : updateStandardFridgeZone(this.data.selectedZoneKey, patch);
    const selectedKey = this.data.selectedZoneKey;
    const prevCounts = [...this.data.standardZones, ...this.data.customZones];
    const counts = new Map(prevCounts.map((z) => [z.key, { count: z.count, warnCount: z.warnCount }]));
    const zones = updatedZones.map((z) => ({
      ...z,
      count: counts.get(z.key)?.count || 0,
      warnCount: counts.get(z.key)?.warnCount || 0,
      width: z.width || 1,
      height: z.height || 1,
      spanClass: `span-w${z.width || 1}-h${z.height || 1}`,
      selected: z.key === selectedKey,
      editable: true,
    }));
    const selected = zones.find((z) => z.key === selectedKey);
    this.setData({
      standardZones: zones.filter((z) => !z.custom),
      customZones: zones.filter((z) => z.custom),
      selectedZoneLabel: selected?.label || this.data.selectedZoneLabel,
      selectedZoneHint: selected?.hint || this.data.selectedZoneHint,
      selectedZoneWidth: selected?.width || this.data.selectedZoneWidth,
      selectedZoneHeight: selected?.height || this.data.selectedZoneHeight,
      selectedZoneIsCustom: !!selected?.custom,
    });
  },

  onDeleteZone(event: WechatMiniprogram.BaseEvent) {
    const key = event.currentTarget.dataset.zone as string;
    this.deleteZoneByKey(key);
  },

  deleteZoneByKey(key: string) {
    const zone = this.data.customZones.find((z) => z.key === key);
    if (!zone?.custom) return;
    wx.showModal({
      title: "删除自定义分区",
      content: `删除"${zone.label}"后，库存条目不会被删除，但可能显示为未配置分区。`,
      confirmText: "删除",
      confirmColor: "#d95745",
      success: (res) => {
        if (!res.confirm) return;
        deleteCustomFridgeZone(key);
        this.setData({
          selectedZoneKey: this.data.selectedZoneKey === key ? "" : this.data.selectedZoneKey,
          selectedZoneIsCustom: this.data.selectedZoneKey === key ? false : this.data.selectedZoneIsCustom,
        });
        void this.refresh();
      },
    });
  },

  onDeleteSelectedZone() {
    if (!this.data.selectedZoneKey) return;
    const target = [...this.data.standardZones, ...this.data.customZones].find((z) => z.key === this.data.selectedZoneKey);
    if (!target) return;
    if (target.custom) {
      this.deleteZoneByKey(this.data.selectedZoneKey);
      return;
    }
    wx.showToast({ title: "默认分区暂不支持删除", icon: "none" });
  },

  onTapOffline() {
    this.setTabBarHidden(false);
    this.setData({ statusPanelOpen: false });
    wx.navigateTo({ url: "/pages/offline/index" });
  },

  onTapBindManual() {
    this.setTabBarHidden(false);
    this.setData({ statusPanelOpen: false });
    wx.navigateTo({ url: "/pages/bind/index" });
  },

  /** 一键绑定 DEMO 演示设备：成功后立刻刷新 overview。 */
  async onTapBindDemo() {
    if (this.data.binding) return;
    const app = getApp<MiniAppInstance["globalData"]>() as unknown as MiniAppInstance;
    if (!app.globalData.session?.token) {
      const session = await app.ensureSession();
      if (!session?.token) {
        wx.showToast({ title: "登录未完成", icon: "none" });
        return;
      }
    }
    this.setData({ binding: true });
    try {
      const device = await bindDevice({ bindCode: DEMO_BIND_CODE });
      app.setActiveDevice(device);
      this.applyDevice(device);
      wx.showToast({ title: "绑定成功", icon: "success" });
      void this.refresh();
    } catch (err) {
      const message =
        err instanceof RequestError
          ? err.message
          : err instanceof Error
            ? err.message
            : "绑定失败";
      wx.showToast({ title: message, icon: "none" });
    } finally {
      this.setData({ binding: false });
    }
  },

  /** 状态抽屉中的"刷新冰箱"：透传 MQTT 命令给设备。 */
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

  /** 重试静默登录。 */
  async onTapRetryLogin() {
    const app = getApp<MiniAppInstance["globalData"]>() as unknown as MiniAppInstance;
    this.setData({ loginErrorText: "" });
    const session = await app.ensureSession(true);
    if (session?.token) {
      void this.refresh();
    } else {
      this.setData({ loginErrorText: app.globalData.lastLoginError || "登录失败" });
    }
  },
});
