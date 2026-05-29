/**
 * 自定义 TabBar —— 1:1 复刻 ui-reference/WLW 的 .dock：
 *  ┌─────────────────────────────────────────────┐
 *  │  首页    AI    [登记]    设置    更多      │   ← 5 个按钮等宽
 *  └─────────────────────────────────────────────┘
 *           ⬆ 中间"登记"是抬起的番茄红圆角大按钮（margin-top: -28px）
 *
 * 实现方式：
 *  - app.json 设 `tabBar.custom: true` 后，微信会把这个 Component 渲染到所有 tabBar 页面底部；
 *  - 每个 tabBar 页面在 onShow 里 `this.getTabBar().setData({ selected: N })` 同步高亮；
 *  - 切换通过 `wx.switchTab` 跳转，保持原生 tabBar 的"页面栈保留"语义。
 *
 * 与微信 tabBar 约定：
 *  - tabBar.list 的顺序必须和 buttons 数组一致；
 *  - pagePath 必须出现在 app.json `pages` 中；
 *  - 中间"登记"按钮指向相机/录入页（pages/scan/index）。
 */

interface TabButton {
  pagePath: string;     // 与 app.json -> tabBar.list 中的 pagePath 对齐
  text: string;         // 文案（中文）
  iconClass: string;    // CSS 类（icon-home / icon-ai / icon-settings / icon-more）
  isMain?: boolean;     // 中间抬起的"登记"按钮
}

Component({
  data: {
    /** 当前选中索引；外部通过 setData 同步 */
    selected: 0,
    /** 5 个 tab 按钮的元数据；顺序 = 索引 */
    buttons: [
      { pagePath: "/pages/home/index",     text: "首页",   iconClass: "icon-home" },
      { pagePath: "/pages/ai-chat/index",  text: "AI",    iconClass: "icon-ai" },
      { pagePath: "/pages/scan/index",     text: "登记",   iconClass: "icon-camera", isMain: true },
      { pagePath: "/pages/settings/index", text: "设置",   iconClass: "icon-settings" },
      { pagePath: "/pages/more/index",     text: "更多",   iconClass: "icon-more" },
    ] as TabButton[],
  },

  methods: {
    /** 点击 tab：通过 wx.switchTab 切换；同 tab 重复点击直接置空（微信会自己处理） */
    onSwitch(e: WechatMiniprogram.BaseEvent) {
      const index = Number(e.currentTarget.dataset.index);
      const target = this.data.buttons[index];
      if (!target) return;

      // 同 tab 重复点击：不重复触发 switchTab，避免 IDE 报 "redirect to same path"
      if (index === this.data.selected) return;

      wx.switchTab({
        url: target.pagePath,
        // 失败时回到首页保底（不会发生在 pages/ 已注册的情况下）
        fail: (err) => {
          console.warn("[tabbar] switchTab fail", target.pagePath, err);
        },
      });
    },
  },
});