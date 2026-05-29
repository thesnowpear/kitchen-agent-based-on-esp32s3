/**
 * 库存页：顶部 zone tab（freezer/left/right/door + 全部），下方按九宫格 slot 展示该 zone 食材。
 *
 * - 数据：getInventory() 一次取当前 home 全量；前端按 zone 过滤；
 * - "+" 按钮跳 /pages/scan（拍照入库）；
 * - 单格点击：已占 → 跳 /pages/inventory-detail/{id}，空 → 跳 detail 新增（带 zone+slot 预填）；
 * - 顶部"刷新冰箱"按钮：调 /inventory/refresh 异步触发 MQTT 命令；
 * - 切 tab 时若 globalData.pendingZone 存在，自动定位到该 zone（首页跳过来用）。
 */

import type { MiniAppInstance } from "../../app";
import { getInventory, refreshInventory } from "../../services/api";
import type { InventoryItem, StandardZone } from "../../types/models";
import { STANDARD_SLOTS, STANDARD_ZONES } from "../../types/models";
import { RequestError } from "../../utils/request";
import { displayPlace, getFridgeZones } from "../../utils/fridgeZones";
import { updateOfflineSnapshot } from "../../utils/localFeatures";

interface ZoneTab {
  key: string;
  label: string;
}

interface SlotCellView {
  slot: string;
  occupied: boolean;
  item?: InventoryItem;
  level?: "normal" | "warn" | "danger";
  expireText?: string;
}

type InventoryListViewItem = InventoryItem & { locationText: string };

function zoneTabs(): ZoneTab[] {
  return [
    { key: "all", label: "全部" },
    ...getFridgeZones().map((z) => ({ key: z.key, label: z.label })),
  ];
}

function freshnessLevel(item: InventoryItem): "normal" | "warn" | "danger" {
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

function expireText(item: InventoryItem): string {
  if (!item.expireDate) return "";
  const today = new Date();
  today.setHours(0, 0, 0, 0);
  const target = new Date(item.expireDate + "T00:00:00");
  const diff = Math.round(
    (target.getTime() - today.getTime()) / (24 * 3600 * 1000),
  );
  if (diff <= 0) return "今天";
  if (diff === 1) return "明天";
  return `${diff}天`;
}

Page({
  data: {
    loading: false,
    refreshing: false,
    items: [] as InventoryItem[],
    zones: zoneTabs(),
    activeZone: "all",
    cells: [] as SlotCellView[],
    listView: [] as InventoryListViewItem[],
    showAsGrid: true,
    errorText: "",
  },

  onShow() {
    const app = getApp<MiniAppInstance["globalData"]>() as unknown as MiniAppInstance;
    this.setData({ zones: zoneTabs() });
    // 首页跳转过来时通过 globalData.pendingZone 传值
    const pendingZone = (app.globalData as MiniAppInstance["globalData"] & { pendingZone?: string })
      .pendingZone;
    if (pendingZone) {
      this.setData({ activeZone: pendingZone, showAsGrid: pendingZone !== "all" });
      (app.globalData as MiniAppInstance["globalData"] & { pendingZone?: string }).pendingZone = undefined;
    }
    this.load();
  },

  onPullDownRefresh() {
    this.load().finally(() => wx.stopPullDownRefresh());
  },

  async load() {
    if (this.data.loading) return;
    this.setData({ loading: true, errorText: "" });
    try {
      const data = await getInventory();
      this.setData({ items: data.items });
      updateOfflineSnapshot({ inventoryItems: data.items || [] });
      this.applyZoneFilter();
    } catch (err) {
      const message =
        err instanceof RequestError
          ? err.message
          : err instanceof Error
            ? err.message
            : "加载失败";
      this.setData({ errorText: message, items: [], cells: [], listView: [] });
    } finally {
      this.setData({ loading: false });
    }
  },

  onTapTab(event: WechatMiniprogram.BaseEvent) {
    const zone = event.currentTarget.dataset.zone as string;
    this.setData({ activeZone: zone, showAsGrid: zone !== "all" });
    this.applyZoneFilter();
  },

  applyZoneFilter() {
    const zone = this.data.activeZone;
    if (zone === "all") {
      this.setData({ listView: this.toListView(this.data.items), cells: [] });
      return;
    }
    // 当前 zone 视图：按 STANDARD_SLOTS 排九宫格
    const inZone = this.data.items.filter((it) => it.zone === zone);
    const occupied = new Map<string, InventoryItem>();
    for (const it of inZone) {
      if (it.slot) occupied.set(it.slot, it);
    }
    const cells: SlotCellView[] = STANDARD_SLOTS.map((slot) => {
      const item = occupied.get(slot);
      return item
        ? {
            slot,
            occupied: true,
            item,
            level: freshnessLevel(item),
            expireText: expireText(item),
          }
        : { slot, occupied: false };
    });
    // zone 里 slot 未指定的，单独放进 listView 当"待归位"。
    const unplaced = this.toListView(inZone.filter((it) => !it.slot));
    this.setData({ cells, listView: unplaced });
  },

  toListView(items: InventoryItem[]): InventoryListViewItem[] {
    return items.map((item) => ({
      ...item,
      locationText: item.location || displayPlace(item.zone, item.slot),
    }));
  },

  onTapCell(event: WechatMiniprogram.BaseEvent) {
    const slot = event.currentTarget.dataset.slot as string;
    const cell = this.data.cells.find((c) => c.slot === slot);
    if (cell?.occupied && cell.item) {
      wx.navigateTo({ url: `/pages/inventory-detail/index?id=${cell.item.id}` });
    } else {
      const zone = this.data.activeZone;
      wx.navigateTo({
        url: `/pages/inventory-detail/index?mode=create&zone=${zone}&slot=${slot}`,
      });
    }
  },

  onTapItem(event: WechatMiniprogram.BaseEvent) {
    const id = event.currentTarget.dataset.id as string;
    wx.navigateTo({ url: `/pages/inventory-detail/index?id=${id}` });
  },

  onTapScan() {
    const zone = this.data.activeZone !== "all" ? this.data.activeZone : "";
    const app = getApp<MiniAppInstance["globalData"]>() as unknown as MiniAppInstance;
    app.globalData.pendingScanZone = zone || undefined;
    // scan 是 tabBar 页面，必须用 switchTab；zone 通过 globalData 暂存。
    wx.switchTab({ url: "/pages/scan/index" });
  },

  async onTapRefreshDevice() {
    if (this.data.refreshing) return;
    this.setData({ refreshing: true });
    try {
      const data = await refreshInventory();
      wx.showToast({
        title: data.queued ? "已发起刷新" : "未连接设备",
        icon: data.queued ? "success" : "none",
      });
    } catch {
      wx.showToast({ title: "刷新失败", icon: "none" });
    } finally {
      this.setData({ refreshing: false });
    }
  },

  // 简化的类型守卫，避免 wx.event 强转 noise
  asStandardZone(zone: string): StandardZone | null {
    return STANDARD_ZONES.includes(zone as StandardZone)
      ? (zone as StandardZone)
      : null;
  },

});
