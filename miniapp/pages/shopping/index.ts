/**
 * 购物清单页：本地持久化 CRUD。
 *
 * 第一版不新增后端表，保证离线时也能增删勾选；菜谱页可把缺少食材写入同一份本地清单。
 */

import { getInventory } from "../../services/api";
import { enqueueSyncOp, syncNow } from "../../services/sync";
import type { InventoryItem, ShoppingItem } from "../../types/models";
import { addShoppingItems, getShoppingItems, saveShoppingItems } from "../../utils/localFeatures";

type SourceFilter = "all" | ShoppingItem["source"];
type WxActionSheet = {
  showActionSheet(options: {
    itemList: string[];
    success?: (res: { tapIndex: number }) => void;
    fail?: () => void;
  }): void;
};

Page({
  data: {
    items: [] as ShoppingItem[],
    visibleItems: [] as ShoppingItem[],
    filter: "all" as SourceFilter,
    filters: [
      { key: "all", label: "全部" },
      { key: "manual", label: "手动" },
      { key: "recipe", label: "菜谱" },
      { key: "expire", label: "临期" },
      { key: "ai", label: "AI" },
    ],
    addOpen: false,
    editingId: "",
    inputName: "",
    inputQuantity: "",
    inputSourceText: "",
    seeding: false,
  },

  onShow() {
    this.load();
  },

  load() {
    const items = getShoppingItems();
    this.setData({ items });
    this.applyFilter(items, this.data.filter);
  },

  applyFilter(items: ShoppingItem[], filter: SourceFilter) {
    this.setData({
      visibleItems: filter === "all" ? items : items.filter((it) => it.source === filter),
    });
  },

  onTapFilter(event: WechatMiniprogram.BaseEvent) {
    const filter = event.currentTarget.dataset.key as SourceFilter;
    this.setData({ filter });
    this.applyFilter(this.data.items, filter);
  },

  onToggle(e: WechatMiniprogram.BaseEvent) {
    const id = e.currentTarget.dataset.id as string;
    const items = this.data.items.map((it) =>
      it.id === id ? { ...it, checked: !it.checked } : it,
    );
    this.persist(items);
  },

  onLongPressItem(e: WechatMiniprogram.BaseEvent) {
    const id = e.currentTarget.dataset.id as string;
    const target = this.data.items.find((it) => it.id === id);
    if (!target) return;
    (wx as unknown as WxActionSheet).showActionSheet({
      itemList: ["编辑", "删除"],
      success: (res) => {
        if (res.tapIndex === 0) this.openEdit(target);
        if (res.tapIndex === 1) this.deleteItem(id);
      },
    });
  },

  openAdd() {
    this.setData({
      addOpen: true,
      editingId: "",
      inputName: "",
      inputQuantity: "",
      inputSourceText: "手动添加",
    });
  },

  openEdit(item: ShoppingItem) {
    this.setData({
      addOpen: true,
      editingId: item.id,
      inputName: item.name,
      inputQuantity: item.quantityText,
      inputSourceText: item.sourceText,
    });
  },

  closeAdd() {
    this.setData({ addOpen: false, editingId: "" });
  },

  onInput(event: WechatMiniprogram.Input) {
    const field = event.currentTarget.dataset.field as string;
    const value = String(event.detail.value || "");
    if (field === "name") this.setData({ inputName: value });
    if (field === "quantity") this.setData({ inputQuantity: value });
    if (field === "sourceText") this.setData({ inputSourceText: value });
  },

  saveManualItem() {
    const name = this.data.inputName.trim();
    if (!name) {
      wx.showToast({ title: "请填写名称", icon: "none" });
      return;
    }
    if (this.data.editingId) {
      const items = this.data.items.map((it) =>
        it.id === this.data.editingId
          ? {
              ...it,
              name,
              quantityText: this.data.inputQuantity.trim() || "1 份",
              sourceText: this.data.inputSourceText.trim() || it.sourceText,
            }
          : it,
      );
      this.persist(items);
    } else {
      const items = addShoppingItems([
        {
          name,
          quantityText: this.data.inputQuantity.trim() || "1 份",
          source: "manual",
          sourceText: this.data.inputSourceText.trim() || "手动添加",
        },
      ]);
      this.setData({ items });
      this.applyFilter(items, this.data.filter);
      this.queueShoppingSync("add", items);
    }
    this.closeAdd();
  },

  deleteItem(id: string) {
    wx.showModal({
      title: "删除购物项",
      content: "确定删除这一项吗？",
      confirmText: "删除",
      confirmColor: "#d95745",
      success: (res) => {
        if (res.confirm) this.persist(this.data.items.filter((it) => it.id !== id));
      },
    });
  },

  clearChecked() {
    const checkedCount = this.data.items.filter((it) => it.checked).length;
    if (!checkedCount) {
      wx.showToast({ title: "没有已购项", icon: "none" });
      return;
    }
    wx.showModal({
      title: "清除已购",
      content: `将清除 ${checkedCount} 个已勾选条目。`,
      confirmText: "清除",
      confirmColor: "#d95745",
      success: (res) => {
        if (res.confirm) this.persist(this.data.items.filter((it) => !it.checked));
      },
    });
  },

  async seedFromExpiring() {
    if (this.data.seeding) return;
    this.setData({ seeding: true });
    try {
      const data = await getInventory();
      const expiring = (data.items || []).filter((item) => isExpiring(item));
      if (!expiring.length) {
        wx.showToast({ title: "暂无临期食材", icon: "none" });
        return;
      }
      const items = addShoppingItems(
        expiring.map((item) => ({
          name: item.name,
          quantityText: `${item.quantity}${item.unit}`,
          source: "expire",
          sourceText: "临期补货",
        })),
      );
      this.setData({ items, filter: "all" });
      this.applyFilter(items, "all");
      this.queueShoppingSync("seed_expiring", items);
      wx.showToast({ title: `已加入 ${expiring.length} 项`, icon: "success" });
    } catch {
      wx.showToast({ title: "读取库存失败", icon: "none" });
    } finally {
      this.setData({ seeding: false });
    }
  },

  onExport() {
    const text = this.data.items
      .filter((it) => !it.checked)
      .map((it) => `${it.name} ${it.quantityText}`)
      .join("\n");
    if (!text) {
      wx.showToast({ title: "没有未购项", icon: "none" });
      return;
    }
    wx.setClipboardData({
      data: text,
      success: () => wx.showToast({ title: "已复制到剪贴板", icon: "success" }),
    });
  },

  persist(items: ShoppingItem[]) {
    saveShoppingItems(items);
    this.setData({ items });
    this.applyFilter(items, this.data.filter);
    this.queueShoppingSync("replace", items);
  },

  queueShoppingSync(op: string, items: ShoppingItem[]) {
    enqueueSyncOp("shopping_list", op, {
      items,
      updatedAt: new Date().toISOString(),
    });
    void syncNow().catch(() => {
      // 购物清单是本地优先能力；同步失败保留队列。
    });
  },

  noop() {
    // 用于阻止编辑弹层点击冒泡。
  },
});

function isExpiring(item: InventoryItem): boolean {
  if (!item.expireDate) return false;
  const today = new Date();
  today.setHours(0, 0, 0, 0);
  const target = new Date(`${item.expireDate}T00:00:00`);
  const diff = Math.round((target.getTime() - today.getTime()) / 86400000);
  return diff <= 3;
}
