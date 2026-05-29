/**
 * 更多页 —— 对齐 ui-reference 的 .more-grid 2×2：
 *   ┌──────────┬──────────┐
 *   │ 食材清单 │ 购物清单 │
 *   ├──────────┼──────────┤
 *   │ 临期归位 │ 离线模式 │
 *   └──────────┴──────────┘
 *
 * 当前入口跳转到对应已存在的页面：
 *  - 食材清单 → inventory（用 navigateTo，因为 inventory 不再是 tabBar 页面）
 *  - 购物清单 → shopping（本地持久化清单）
 *  - 临期归位 → reminders
 *  - 离线模式 → offline 页，展示最近一次本地缓存。
 */

interface MoreEntry {
  key: string;
  title: string;
  hint: string;
  badge?: string;
  url?: string;        // 有则 navigateTo；无则 toast
}

Page({
  data: {
    entries: [
      { key: "inventory", title: "食材清单", hint: "按分区查看 / 搜索 / 入库", url: "/pages/inventory/index" },
      { key: "shopping",  title: "购物清单", hint: "AI 自动添加 + 手动勾选",  url: "/pages/shopping/index" },
      { key: "reminders", title: "临期归位", hint: "今日到期 / 待办提醒",     url: "/pages/reminders/index" },
      { key: "offline",   title: "离线模式", hint: "断网时也能看到最新状态",  url: "/pages/offline/index" },
    ] as MoreEntry[],
  },

  onShow() {
    // 同步自定义 tabBar 高亮（更多 = 索引 4）
    const tabBar = (this.getTabBar?.() as unknown) as { setData: (d: { selected: number }) => void } | undefined;
    if (tabBar) tabBar.setData({ selected: 4 });
  },

  onTapEntry(e: WechatMiniprogram.BaseEvent) {
    const key = e.currentTarget.dataset.key as string;
    const entry = this.data.entries.find((it) => it.key === key);
    if (!entry) return;
    if (entry.url) {
      wx.navigateTo({ url: entry.url });
    } else {
      wx.showToast({ title: "稍后版本支持", icon: "none" });
    }
  },
});
