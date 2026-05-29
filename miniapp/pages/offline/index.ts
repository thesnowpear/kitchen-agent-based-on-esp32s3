/**
 * 离线模式页：展示最近缓存的首页、库存和提醒摘要。
 *
 * 这里不缓存图片、API Key 或 token；只用于断网时维持基础可读状态。
 */

import { getHomeOverview, getInventory, getReminders } from "../../services/api";
import type { InventoryItem, OfflineSnapshot, ReminderItem } from "../../types/models";
import { displayPlace } from "../../utils/fridgeZones";
import { getOfflineSnapshot, updateOfflineSnapshot } from "../../utils/localFeatures";

type OfflineInventoryView = InventoryItem & { placeText: string; expireText: string };
type OfflineReminderView = ReminderItem & { dueText: string };

Page({
  data: {
    hasSnapshot: false,
    savedText: "尚无缓存",
    refreshing: false,
    inventoryCount: 0,
    expiringCount: 0,
    reminderCount: 0,
    deviceText: "未知",
    inventory: [] as OfflineInventoryView[],
    reminders: [] as OfflineReminderView[],
  },

  onShow() {
    this.applySnapshot(getOfflineSnapshot());
  },

  onPullDownRefresh() {
    this.onTapRetry().finally(() => wx.stopPullDownRefresh());
  },

  applySnapshot(snapshot: OfflineSnapshot | null) {
    if (!snapshot) {
      this.setData({ hasSnapshot: false });
      return;
    }
    const overview = snapshot.overview || null;
    const inventory = (snapshot.inventoryItems || []).slice(0, 8).map((item) => ({
      ...item,
      placeText: item.location || displayPlace(item.zone, item.slot),
      expireText: item.expireDate ? item.expireDate : "未设置到期",
    }));
    const reminders = (snapshot.reminderItems || [])
      .filter((it) => it.status === "pending")
      .slice(0, 6)
      .map((item) => ({
        ...item,
        dueText: item.dueAt ? item.dueAt.slice(0, 16).replace("T", " ") : "无截止",
      }));
    this.setData({
      hasSnapshot: true,
      savedText: this.humanize(snapshot.savedAt),
      inventoryCount: overview?.inventoryCount ?? snapshot.inventoryItems.length,
      expiringCount: overview?.expiringCount ?? this.countExpiring(snapshot.inventoryItems),
      reminderCount: overview?.pendingReminderCount ?? reminders.length,
      deviceText: overview?.device
        ? `${overview.device.name || overview.device.deviceSn || "冰箱贴"} · ${overview.device.status || "unknown"}`
        : "无设备缓存",
      inventory,
      reminders,
    });
  },

  async onTapRetry(): Promise<void> {
    if (this.data.refreshing) return;
    this.setData({ refreshing: true });
    try {
      const [overview, inventory, reminders] = await Promise.all([
        getHomeOverview(),
        getInventory(),
        getReminders(),
      ]);
      const snapshot = updateOfflineSnapshot({
        overview,
        inventoryItems: inventory.items || [],
        reminderItems: reminders.items || [],
      });
      this.applySnapshot(snapshot);
      wx.showToast({ title: "已刷新缓存", icon: "success" });
    } catch {
      wx.showToast({ title: "暂时无法联网", icon: "none" });
    } finally {
      this.setData({ refreshing: false });
    }
  },

  onTapInventory() {
    wx.navigateTo({ url: "/pages/inventory/index" });
  },

  onTapReminders() {
    wx.navigateTo({ url: "/pages/reminders/index" });
  },

  humanize(ts: number): string {
    if (!ts) return "尚无缓存";
    const diff = Math.max(0, Math.round((Date.now() - ts) / 60000));
    if (diff < 1) return "刚刚缓存";
    if (diff < 60) return `${diff} 分钟前缓存`;
    const hours = Math.round(diff / 60);
    if (hours < 24) return `${hours} 小时前缓存`;
    const d = new Date(ts);
    return `${d.getMonth() + 1}/${d.getDate()} ${String(d.getHours()).padStart(2, "0")}:${String(d.getMinutes()).padStart(2, "0")}`;
  },

  countExpiring(items: InventoryItem[]): number {
    const today = new Date();
    today.setHours(0, 0, 0, 0);
    return items.filter((item) => {
      if (!item.expireDate) return false;
      const target = new Date(`${item.expireDate}T00:00:00`);
      const diff = Math.round((target.getTime() - today.getTime()) / 86400000);
      return diff <= 3;
    }).length;
  },
});
