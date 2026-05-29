/**
 * 提醒页：列出当前 home 待处理提醒；点击确认（POST /reminders/{id}/confirm，status=acked）。
 *
 * - 数据来源：getReminders() 返回 ReminderListData.items；
 * - 仅展示 status === "pending" 的；其它（acked / dismissed）默认隐藏；
 * - 点击"忽略"传 status=dismissed；
 * - 删除后用 setData 局部更新，不重新整页拉。
 */

import { confirmReminder, getReminders } from "../../services/api";
import { enqueueSyncOp, syncNow } from "../../services/sync";
import type { ReminderItem } from "../../types/models";
import { RequestError } from "../../utils/request";
import { updateOfflineSnapshot } from "../../utils/localFeatures";

type ReminderViewItem = ReminderItem & {
  dueText: string;
  levelText: string;
  tagClass: string;
  confirming?: boolean;
};

function toViewItem(item: ReminderItem): ReminderViewItem {
  let levelText = "提示";
  let tagClass = "mint";
  if (item.reminderType === "expire_soon") {
    levelText = "临期";
    tagClass = "danger";
  } else if (item.reminderType === "low_stock") {
    levelText = "低库存";
    tagClass = "warn";
  } else if (item.reminderType === "device_offline") {
    levelText = "设备离线";
    tagClass = "warn";
  } else if (item.reminderType === "scan_pending") {
    levelText = "待处理";
    tagClass = "warn";
  }
  return {
    ...item,
    dueText: item.dueAt ? item.dueAt.slice(0, 16).replace("T", " ") : "无截止",
    levelText,
    tagClass,
  };
}

Page({
  data: {
    items: [] as ReminderViewItem[],
    loading: false,
    errorText: "",
  },

  onShow() {
    this.load();
  },

  onPullDownRefresh() {
    this.load().finally(() => wx.stopPullDownRefresh());
  },

  async load() {
    if (this.data.loading) return;
    this.setData({ loading: true, errorText: "" });
    try {
      const data = await getReminders();
      updateOfflineSnapshot({ reminderItems: data.items || [] });
      this.setData({
        items: (data.items || [])
          .filter((it) => it.status === "pending")
          .map(toViewItem),
      });
    } catch (err) {
      const message =
        err instanceof RequestError
          ? err.message
          : err instanceof Error
            ? err.message
            : "加载失败";
      this.setData({ errorText: message, items: [] });
    } finally {
      this.setData({ loading: false });
    }
  },

  async onConfirm(event: WechatMiniprogram.BaseEvent) {
    await this.actOn(event, "acked");
  },

  async onDismiss(event: WechatMiniprogram.BaseEvent) {
    await this.actOn(event, "dismissed");
  },

  async actOn(event: WechatMiniprogram.BaseEvent, status: "acked" | "dismissed") {
    const id = event.currentTarget.dataset.id as string;
    this.markConfirming(id, true);
    try {
      const updated = await confirmReminder(id, status);
      enqueueSyncOp("reminder", "set_status", {
        reminder: updated,
        reminderId: id,
        status,
      });
      void syncNow().catch(() => {
        // 提醒操作已提交；同步失败时保留队列。
      });
      this.setData({ items: this.data.items.filter((x) => x.id !== id) });
      wx.showToast({
        title: status === "acked" ? "已确认" : "已忽略",
        icon: "success",
      });
    } catch (err) {
      enqueueSyncOp("reminder", "set_status_pending", {
        reminderId: id,
        status,
      });
      this.setData({ items: this.data.items.filter((x) => x.id !== id) });
      wx.showToast({ title: "已加入待同步", icon: "none" });
    }
  },

  markConfirming(id: string, on: boolean) {
    this.setData({
      items: this.data.items.map((it) =>
        it.id === id ? { ...it, confirming: on } : it,
      ),
    });
  },
});
