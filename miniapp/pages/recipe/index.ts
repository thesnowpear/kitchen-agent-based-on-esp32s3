/**
 * AI 菜谱页：复用现有 /ai/chat，根据库存生成推荐。
 *
 * 后端暂未提供 recipe 表，因此本页把模型结果缓存到本地；缺少食材可写入本地购物清单。
 */

import { getInventory, sendAiChat } from "../../services/api";
import type { InventoryItem, RecipeRecommendation } from "../../types/models";
import { addShoppingItems, getRecipeRecommendations, saveRecipeRecommendations, makeLocalId } from "../../utils/localFeatures";

type RecipeView = RecipeRecommendation & {
  missingCount: number;
  ingredientText: string;
};

Page({
  data: {
    loading: false,
    errorText: "",
    summary: {
      kicker: "今晚吃什么",
      title: "按库存生成",
      hint: "优先使用临期食材，减少浪费。",
      duration: "AI 推荐",
    },
    cards: [] as RecipeView[],
    selected: null as RecipeView | null,
    rawReply: "",
  },

  onShow() {
    const cached = getRecipeRecommendations();
    if (cached.length) this.applyCards(cached);
  },

  async onTapRefresh() {
    if (this.data.loading) return;
    this.setData({ loading: true, errorText: "", rawReply: "" });
    try {
      const inventory = await getInventory();
      const prompt = buildRecipePrompt(inventory.items || []);
      const reply = await sendAiChat(prompt, "miniapp-recipe");
      const cards = parseRecipeReply(reply.reply);
      saveRecipeRecommendations(cards);
      this.applyCards(cards);
      this.setData({ rawReply: reply.reply });
      wx.showToast({ title: "已生成菜谱", icon: "success" });
    } catch (err) {
      const message = err instanceof Error ? err.message : "生成失败";
      this.setData({ errorText: message });
      wx.showToast({ title: "生成失败", icon: "none" });
    } finally {
      this.setData({ loading: false });
    }
  },

  applyCards(cards: RecipeRecommendation[]) {
    const views: RecipeView[] = cards.map((card) => ({
      ...card,
      missingCount: card.ingredients.filter((it) => it.missing).length,
      ingredientText: card.ingredients.map((it) => `${it.name}${it.missing ? "（缺）" : ""}`).join("、"),
    }));
    const first = views[0];
    this.setData({
      cards: views,
      summary: first
        ? {
            kicker: "今晚吃什么",
            title: first.title,
            hint: first.desc,
            duration: first.durationText,
          }
        : this.data.summary,
    });
  },

  onTapCard(e: WechatMiniprogram.BaseEvent) {
    const id = e.currentTarget.dataset.id as string;
    const selected = this.data.cards.find((card) => card.id === id) || null;
    this.setData({ selected });
  },

  closeDetail() {
    this.setData({ selected: null });
  },

  addMissingToShopping(e: WechatMiniprogram.BaseEvent) {
    const id = (e.currentTarget.dataset.id as string) || this.data.selected?.id || "";
    const card = this.data.cards.find((it) => it.id === id) || this.data.selected;
    if (!card) return;
    const missing = card.ingredients.filter((it) => it.missing);
    if (!missing.length) {
      wx.showToast({ title: "没有缺少食材", icon: "none" });
      return;
    }
    addShoppingItems(
      missing.map((it) => ({
        name: it.name,
        quantityText: it.quantityText || "1 份",
        source: "recipe",
        sourceText: `菜谱缺料 · ${card.title}`,
      })),
    );
    wx.showToast({ title: `已加入 ${missing.length} 项`, icon: "success" });
  },

  noop() {
    // 用于阻止 sheet 点击冒泡。
  },
});

function buildRecipePrompt(items: InventoryItem[]): string {
  const today = new Date().toISOString().slice(0, 10);
  const inventoryText = items.length
    ? items
        .slice(0, 30)
        .map((it) => `${it.name} ${it.quantity}${it.unit}${it.expireDate ? ` 到期:${it.expireDate}` : ""}${it.category ? ` 类别:${it.category}` : ""}`)
        .join("\n")
    : "暂无库存";
  return [
    "你是冰箱小精灵的小程序菜谱助手。",
    `今天是 ${today}。请基于库存生成 2 到 3 个中文家常菜推荐，优先使用临期食材。`,
    "请只输出 JSON 数组，不要 markdown，不要额外解释。",
    "数组元素字段：title, desc, durationText, tags(string[]), ingredients([{name,quantityText,missing}]), steps(string[])。",
    "missing=true 表示库存中缺少但做菜需要购买的食材。",
    "库存：",
    inventoryText,
  ].join("\n");
}

function parseRecipeReply(reply: string): RecipeRecommendation[] {
  const jsonText = extractJsonArray(reply);
  if (jsonText) {
    try {
      const parsed = JSON.parse(jsonText) as Array<Partial<RecipeRecommendation>>;
      const cards = parsed.map((item, index) => normalizeRecipe(item, index)).filter((it) => it.title);
      if (cards.length) return cards;
    } catch {
      // 兼容非严格 JSON，落到纯文本卡片。
    }
  }
  return [
    {
      id: makeLocalId("recipe"),
      title: "AI 推荐结果",
      desc: reply.slice(0, 90) || "暂未生成有效菜谱。",
      durationText: "约 20 分钟",
      tags: ["AI 生成"],
      ingredients: [],
      steps: reply ? reply.split(/\n+/).filter(Boolean).slice(0, 6) : ["请稍后重试。"],
      rawText: reply,
    },
  ];
}

function extractJsonArray(text: string): string {
  const start = text.indexOf("[");
  const end = text.lastIndexOf("]");
  return start >= 0 && end > start ? text.slice(start, end + 1) : "";
}

function normalizeRecipe(item: Partial<RecipeRecommendation>, index: number): RecipeRecommendation {
  return {
    id: item.id || makeLocalId(`recipe${index}`),
    title: String(item.title || `推荐菜 ${index + 1}`),
    desc: String(item.desc || "基于当前库存生成。"),
    durationText: String(item.durationText || "约 20 分钟"),
    tags: Array.isArray(item.tags) ? item.tags.map(String).slice(0, 4) : ["家常"],
    ingredients: Array.isArray(item.ingredients)
      ? item.ingredients.map((it) => ({
          name: String(it.name || ""),
          quantityText: String(it.quantityText || "适量"),
          missing: !!it.missing,
        })).filter((it) => it.name)
      : [],
    steps: Array.isArray(item.steps) ? item.steps.map(String).filter(Boolean).slice(0, 8) : [],
    rawText: item.rawText,
  };
}
