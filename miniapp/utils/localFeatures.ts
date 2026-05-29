/**
 * 本地功能数据：购物清单、菜谱缓存、离线快照。
 *
 * 这些数据不包含 API Key、登录 token 或原始图片；用于断网 Demo 和跨页面协作。
 */

import type {
  HomeOverview,
  InventoryItem,
  OfflineSnapshot,
  RecipeRecommendation,
  ReminderItem,
  ShoppingItem,
} from "../types/models";
import { getStorage, setStorage } from "./storage";

export const SHOPPING_ITEMS_KEY = "shoppingItems";
export const RECIPE_RECOMMENDATIONS_KEY = "recipeRecommendations";
export const OFFLINE_SNAPSHOT_KEY = "offlineSnapshot";

export function makeLocalId(prefix: string): string {
  return `${prefix}_${Date.now()}_${Math.random().toString(36).slice(2, 8)}`;
}

export function getShoppingItems(): ShoppingItem[] {
  return getStorage<ShoppingItem[]>(SHOPPING_ITEMS_KEY) || [];
}

export function saveShoppingItems(items: ShoppingItem[]): void {
  setStorage(SHOPPING_ITEMS_KEY, items);
}

export function addShoppingItems(
  incoming: Array<Omit<ShoppingItem, "id" | "checked" | "createdAt">>,
): ShoppingItem[] {
  const existing = getShoppingItems();
  const normalized = incoming
    .map((item) => ({
      id: makeLocalId("shop"),
      checked: false,
      createdAt: Date.now(),
      ...item,
      name: item.name.trim(),
      quantityText: item.quantityText.trim() || "1 份",
    }))
    .filter((item) => item.name);
  const merged = mergeShoppingItems(existing, normalized);
  saveShoppingItems(merged);
  return merged;
}

function mergeShoppingItems(existing: ShoppingItem[], incoming: ShoppingItem[]): ShoppingItem[] {
  const result = existing.slice();
  for (const item of incoming) {
    const same = result.find((it) => it.name === item.name && !it.checked);
    if (same) {
      same.quantityText = same.quantityText || item.quantityText;
      same.source = item.source;
      same.sourceText = item.sourceText;
    } else {
      result.unshift(item);
    }
  }
  return result;
}

export function getRecipeRecommendations(): RecipeRecommendation[] {
  return getStorage<RecipeRecommendation[]>(RECIPE_RECOMMENDATIONS_KEY) || [];
}

export function saveRecipeRecommendations(cards: RecipeRecommendation[]): void {
  setStorage(RECIPE_RECOMMENDATIONS_KEY, cards);
}

export function updateOfflineSnapshot(patch: {
  overview?: HomeOverview | null;
  inventoryItems?: InventoryItem[];
  reminderItems?: ReminderItem[];
}): OfflineSnapshot {
  const prev = getStorage<OfflineSnapshot>(OFFLINE_SNAPSHOT_KEY);
  const next: OfflineSnapshot = {
    overview: patch.overview !== undefined ? patch.overview : prev?.overview || null,
    inventoryItems: patch.inventoryItems !== undefined ? patch.inventoryItems : prev?.inventoryItems || [],
    reminderItems: patch.reminderItems !== undefined ? patch.reminderItems : prev?.reminderItems || [],
    savedAt: Date.now(),
  };
  setStorage(OFFLINE_SNAPSHOT_KEY, next);
  return next;
}

export function getOfflineSnapshot(): OfflineSnapshot | null {
  return getStorage<OfflineSnapshot>(OFFLINE_SNAPSHOT_KEY);
}
