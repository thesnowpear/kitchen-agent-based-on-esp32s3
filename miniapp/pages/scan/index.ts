/**
 * 拍照入库页：
 *   1) 选图（wx.chooseMedia，前置摄像头 / 相册都可）
 *   2) 上传 /inventory/scan，等候 SiliconFlow 视觉模型返回候选
 *   3) 展示候选列表：name / quantity / unit / category / 推荐位置 / 置信度
 *   4) 用户点"入库" → POST /inventory（预填模型给出的字段 + 服务端建议 zone/slot）
 *
 * - 不预览原图（plan §3.1 决定：仅识别，不预览）；
 * - hintZone 由首页或 inventory 页跳过来时带，用于位置推荐器加权（虽然 backend 当前没用，预留）；
 * - 上传是长接口（60s），界面用 loading 大圆 + 提示文字。
 */

import { createInventoryItem, scanFoodImage } from "../../services/api";
import type { MiniAppInstance } from "../../app";
import type {
  InventoryWritePayload,
  ScanCandidate,
  ScanResult,
} from "../../types/models";
import { RequestError } from "../../utils/request";

interface CandidateView extends ScanCandidate {
  viewId: string;
  busy?: boolean;
  saved?: boolean;
  confidenceClass: "danger" | "warn" | "mint";
}

function classifyConfidence(c: number): "danger" | "warn" | "mint" {
  if (c < 50) return "danger";
  if (c < 80) return "warn";
  return "mint";
}

Page({
  data: {
    hintZone: "",
    uploading: false,
    progressText: "",
    rawText: "",
    modelUsed: "",
    candidates: [] as CandidateView[],
    errorText: "",
    confirmingAll: false,
  },

  onLoad(query: Record<string, string | undefined>) {
    if (query.zone) {
      this.setData({ hintZone: query.zone });
    }
  },

  onShow() {
    const app = getApp<MiniAppInstance["globalData"]>() as unknown as MiniAppInstance;
    if (app.globalData.pendingScanZone) {
      this.setData({ hintZone: app.globalData.pendingScanZone });
      app.globalData.pendingScanZone = undefined;
    }

    // 同步自定义 tabBar 高亮（登记/相机 = 索引 2，中间抬起按钮）
    const tabBar = (this.getTabBar?.() as unknown) as { setData: (d: { selected: number }) => void } | undefined;
    if (tabBar) tabBar.setData({ selected: 2 });
  },

  onChooseImage() {
    if (this.data.uploading) return;
    wx.chooseMedia({
      count: 1,
      mediaType: ["image"],
      sourceType: ["album", "camera"],
      camera: "back",
      sizeType: ["compressed"],
      success: (res) => {
        const file = res.tempFiles?.[0];
        if (file?.tempFilePath) {
          this.upload(file.tempFilePath);
        }
      },
      fail: () => wx.showToast({ title: "已取消", icon: "none" }),
    });
  },

  async upload(filePath: string) {
    this.setData({
      uploading: true,
      progressText: "正在压缩并识别食材，预计 5-30 秒…",
      errorText: "",
      candidates: [],
    });
    try {
      const result: ScanResult = await scanFoodImage({
        filePath,
        hintZone: this.data.hintZone || undefined,
      });
      const candidates: CandidateView[] = result.candidates.map((c, index) => ({
        ...c,
        viewId: `${c.name}_${c.suggestedZone || "none"}_${c.suggestedSlot || "none"}_${index}`,
        confidenceClass: classifyConfidence(c.confidence),
      }));
      this.setData({
        candidates,
        rawText: result.rawText || "",
        modelUsed: result.modelUsed,
      });
      if (candidates.length === 0) {
        wx.showToast({ title: "未识别到食材", icon: "none" });
      } else {
        wx.showToast({
          title: `识别到 ${candidates.length} 项`,
          icon: "success",
        });
      }
    } catch (err) {
      const message =
        err instanceof RequestError
          ? err.message
          : err instanceof Error
            ? err.message
            : "上传失败";
      this.setData({ errorText: message });
      wx.showToast({ title: "识别失败", icon: "none" });
    } finally {
      this.setData({ uploading: false, progressText: "" });
    }
  },

  async onConfirmCandidate(event: WechatMiniprogram.BaseEvent) {
    const idx = Number(event.currentTarget.dataset.index);
    const list = this.data.candidates;
    const target = list[idx];
    if (!target || target.saved) return;
    this.patchCandidate(idx, { busy: true });

    const payload: InventoryWritePayload = {
      name: target.name,
      category: target.category || undefined,
      quantity: target.quantity || 1,
      unit: target.unit || "份",
      zone: target.suggestedZone || undefined,
      slot: target.suggestedSlot || undefined,
      source: "scan",
      confidence: target.confidence,
    };
    try {
      await createInventoryItem(payload);
      this.patchCandidate(idx, { busy: false, saved: true });
      wx.showToast({ title: "已入库", icon: "success" });
    } catch (err) {
      this.patchCandidate(idx, { busy: false });
      wx.showToast({ title: "入库失败", icon: "none" });
    }
  },

  async onConfirmAll() {
    if (this.data.confirmingAll) return;
    const pending = this.data.candidates
      .map((candidate, index) => ({ candidate, index }))
      .filter(({ candidate }) => !candidate.saved && !candidate.busy);
    if (!pending.length) {
      wx.showToast({ title: "没有待入库项", icon: "none" });
      return;
    }
    this.setData({ confirmingAll: true });
    let okCount = 0;
    for (const { candidate, index } of pending) {
      this.patchCandidate(index, { busy: true });
      const payload: InventoryWritePayload = {
        name: candidate.name,
        category: candidate.category || undefined,
        quantity: candidate.quantity || 1,
        unit: candidate.unit || "份",
        zone: candidate.suggestedZone || undefined,
        slot: candidate.suggestedSlot || undefined,
        source: "scan",
        confidence: candidate.confidence,
      };
      try {
        await createInventoryItem(payload);
        okCount += 1;
        this.patchCandidate(index, { busy: false, saved: true });
      } catch {
        this.patchCandidate(index, { busy: false });
      }
    }
    this.setData({ confirmingAll: false });
    wx.showToast({ title: `已入库 ${okCount} 项`, icon: okCount > 0 ? "success" : "none" });
  },

  onEditCandidate(event: WechatMiniprogram.BaseEvent) {
    const idx = Number(event.currentTarget.dataset.index);
    const target = this.data.candidates[idx];
    if (!target) return;
    const params = new URLSearchParams();
    params.set("mode", "create");
    params.set("name", target.name);
    params.set("quantity", String(target.quantity || 1));
    params.set("unit", target.unit || "份");
    if (target.category) params.set("category", target.category);
    if (target.suggestedZone) params.set("zone", target.suggestedZone);
    if (target.suggestedSlot) params.set("slot", target.suggestedSlot);
    params.set("source", "scan");
    params.set("confidence", String(target.confidence));
    wx.navigateTo({
      url: `/pages/inventory-detail/index?${params.toString()}`,
    });
  },

  patchCandidate(idx: number, patch: Partial<CandidateView>) {
    const list = this.data.candidates.slice();
    list[idx] = { ...list[idx], ...patch };
    this.setData({ candidates: list });
  },

  onTapDone() {
    // inventory 已不在 tabBar；用 navigateTo 进入
    wx.navigateTo({ url: "/pages/inventory/index" });
  },

  onManualCreate() {
    const params = new URLSearchParams();
    params.set("mode", "create");
    params.set("source", "manual");
    if (this.data.hintZone) params.set("zone", this.data.hintZone);
    wx.navigateTo({ url: `/pages/inventory-detail/index?${params.toString()}` });
  },
});
