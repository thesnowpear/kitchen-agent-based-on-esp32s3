/**
 * 设备绑定页：本期主推"一键绑定演示设备"（bindCode = "DEMO"，对应 backend lifespan 种入的 DEMO-FRIDGE-001），
 * 保留手输 / 扫码两条备用路径。绑定成功后写入 globalData.activeDevice 并跳回 home。
 */

import type { MiniAppInstance } from "../../app";
import { DEMO_BIND_CODE } from "../../config/env";
import { bindDevice, getPrimaryDevice } from "../../services/api";
import type { DeviceSummary } from "../../types/models";
import { RequestError } from "../../utils/request";

interface BindPageData {
  bindCode: string;
  loading: boolean;
  device: DeviceSummary | null;
  errorText: string;
}

Page({
  data: {
    bindCode: "",
    loading: false,
    device: null,
    errorText: "",
  } as BindPageData,

  async onShow() {
    const app = getApp<MiniAppInstance["globalData"]>() as unknown as MiniAppInstance;
    this.setData({ device: app.globalData.activeDevice });
    // 后台尝试拉一次 primary，让"当前设备"卡片反映最新状态。
    try {
      const fresh = await getPrimaryDevice();
      if (fresh) {
        app.setActiveDevice(fresh);
        this.setData({ device: fresh });
      }
    } catch {
      /* 失败不打扰 */
    }
  },

  onBindCodeInput(event: WechatMiniprogram.Input) {
    this.setData({ bindCode: String(event.detail.value || "").trim() });
  },

  scanBindCode() {
    wx.scanCode({
      onlyFromCamera: true,
      scanType: ["qrCode", "barCode"],
      success: (res) => this.setData({ bindCode: res.result.trim() }),
      fail: () => wx.showToast({ title: "扫码已取消", icon: "none" }),
    });
  },

  async onBindDemo() {
    this.setData({ bindCode: DEMO_BIND_CODE });
    await this.submit(DEMO_BIND_CODE);
  },

  async onBindManual() {
    if (!this.data.bindCode) {
      wx.showToast({ title: "请输入绑定码", icon: "none" });
      return;
    }
    await this.submit(this.data.bindCode);
  },

  async submit(code: string) {
    if (this.data.loading) return;
    this.setData({ loading: true, errorText: "" });
    try {
      const device = await bindDevice({ bindCode: code });
      const app = getApp<MiniAppInstance["globalData"]>() as unknown as MiniAppInstance;
      app.setActiveDevice(device);
      void app.bootstrap(true);
      this.setData({ device });
      wx.showToast({ title: "绑定成功", icon: "success" });
      setTimeout(() => wx.switchTab({ url: "/pages/home/index" }), 600);
    } catch (err) {
      const message =
        err instanceof RequestError
          ? err.message
          : err instanceof Error
            ? err.message
            : "绑定失败";
      this.setData({ errorText: message });
      wx.showToast({ title: "绑定失败", icon: "none" });
    } finally {
      this.setData({ loading: false });
    }
  },
});
