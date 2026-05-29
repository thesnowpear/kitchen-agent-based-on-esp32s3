/**
 * AI 对话页：与冰箱小精灵聊天。
 *
 * 数据流：
 *   1) 加载时读取云端历史；没有历史时显示本地欢迎语；
 *   2) 用户输入 → push 自己的 message → POST /ai/chat → push AI 回复（带 source 徽标）；
 *   3) source=device 时背景偏 mint，source=cloud_fallback 时偏 yolk + 显示 fallbackReason。
 *
 * 历史保存：后端按 sessionId 持久化；设备端短期历史后续通过 MQTT TODO 合并。
 */

import { clearAiChatHistory, getAiChatHistory, sendAiChat } from "../../services/api";
import type { AiChatMessage } from "../../types/models";
import { RequestError } from "../../utils/request";

const DEFAULT_SESSION_ID = "miniapp-default";

function uuid(): string {
  return "msg_" + Date.now().toString(36) + Math.random().toString(36).slice(2, 8);
}

function nowIso(): string {
  return new Date().toISOString();
}

Page({
  data: {
    input: "",
    sending: false,
    loadingHistory: false,
    sessionId: DEFAULT_SESSION_ID,
    messages: [] as AiChatMessage[],
    scrollIntoView: "" as string,
    placeholder: "今晚做什么菜？哪些临期？",
  },

  async onLoad() {
    await this.loadHistory();
  },

  makeIntro(): AiChatMessage {
    return {
      id: uuid(),
      role: "assistant",
      // 注意：欢迎语包含中文引号短语（"今天吃什么" 等），故外层必须用反引号或 ASCII 双引号 + 转义；
      // 这里采用反引号模板字符串，TS strict 下能直接通过编译。
      content: `你好！我是冰箱小精灵。问我"今天吃什么"、"哪些临期"、"番茄怎么储存"都可以。`,
      source: "device",
      sentAt: nowIso(),
    };
  },

  async loadHistory() {
    if (this.data.loadingHistory) return;
    this.setData({ loadingHistory: true });
    try {
      const history = await getAiChatHistory(this.data.sessionId);
      const messages = (history.messages || [])
        .filter((message) => message.role === "user" || message.role === "assistant")
        .map((message) => ({
          id: `hist_${message.id.replace(/[^A-Za-z0-9_-]/g, "_")}`,
          role: message.role as "user" | "assistant",
          content: message.content,
          source:
            message.source === "device" || message.source === "cloud_fallback"
              ? message.source
              : undefined,
          fallbackReason: message.fallbackReason,
          modelUsed: message.modelUsed,
          deviceSn: message.deviceSn,
          sentAt: message.sentAt,
        }));
      const next = messages.length ? messages : [this.makeIntro()];
      this.setData({
        sessionId: history.sessionId || this.data.sessionId,
        messages: next,
        scrollIntoView: next[next.length - 1]?.id || "",
      });
    } catch {
      const intro = this.makeIntro();
      this.setData({ messages: [intro], scrollIntoView: intro.id });
    } finally {
      this.setData({ loadingHistory: false });
    }
  },

  onShow() {
    // 同步自定义 tabBar 高亮（AI = 索引 1）
    const tabBar = (this.getTabBar?.() as unknown) as { setData: (d: { selected: number }) => void } | undefined;
    if (tabBar) tabBar.setData({ selected: 1 });
  },

  onInput(event: WechatMiniprogram.Input) {
    this.setData({ input: String(event.detail.value || "") });
  },

  async onSend() {
    const text = this.data.input.trim();
    if (!text || this.data.sending) return;

    const userMsg: AiChatMessage = {
      id: uuid(),
      role: "user",
      content: text,
      sentAt: nowIso(),
    };
    const placeholderId = uuid();
    const aiPlaceholder: AiChatMessage = {
      id: placeholderId,
      role: "assistant",
      content: "正在思考…",
      sentAt: nowIso(),
    };
    this.setData({
      input: "",
      sending: true,
      messages: [...this.data.messages, userMsg, aiPlaceholder],
      scrollIntoView: placeholderId,
    });

    try {
      const res = await sendAiChat(text, this.data.sessionId);
      this.replaceMessage(placeholderId, {
        ...aiPlaceholder,
        content: res.reply || "（无回复）",
        source: (res.source as "device" | "cloud_fallback") ?? undefined,
        fallbackReason: res.fallbackReason,
        modelUsed: res.modelUsed,
        deviceSn: res.deviceSn,
      });
      if (res.sessionId && res.sessionId !== this.data.sessionId) {
        this.setData({ sessionId: res.sessionId });
      }
    } catch (err) {
      const message =
        err instanceof RequestError
          ? err.message
          : err instanceof Error
            ? err.message
            : "AI 回复失败";
      this.replaceMessage(placeholderId, {
        ...aiPlaceholder,
        content: `（错误）${message}`,
        source: "cloud_fallback",
        fallbackReason: "request_failed",
      });
    } finally {
      this.setData({ sending: false, scrollIntoView: placeholderId });
    }
  },

  replaceMessage(id: string, next: AiChatMessage) {
    this.setData({
      messages: this.data.messages.map((m) => (m.id === id ? next : m)),
    });
  },

  async onClear() {
    wx.showModal({
      title: "清空对话",
      content: "会清空云端保存的当前会话历史，设备端本地短期历史暂不受影响。",
      confirmText: "清空",
      confirmColor: "#d95745",
      success: async (res) => {
        if (!res.confirm) return;
        try {
          await clearAiChatHistory(this.data.sessionId);
          const intro = this.makeIntro();
          this.setData({ messages: [intro], scrollIntoView: intro.id });
          wx.showToast({ title: "已清空", icon: "success" });
        } catch {
          wx.showToast({ title: "清空失败", icon: "none" });
        }
      },
    });
  },
});
