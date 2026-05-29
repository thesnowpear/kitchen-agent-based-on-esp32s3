/**
 * 三端同步服务：维护本地队列、dirty 域和 serverRevision。
 *
 * 第一版策略：
 * - 初次同步先请求设备上报本地快照，后端接收后再拉云端快照，避免旧云端数据抢先覆盖设备。
 * - 后续同步先 push 本地队列，再 pull 云端快照，最后把云端最新版本下发设备。
 * 失败时只记录错误和队列，不阻塞页面原有 REST 写入与离线展示。
 */

import {
  getSyncStatus,
  pullSyncChanges,
  pushSyncSnapshotToDevice,
  pushSyncChanges,
} from "./api";
import type {
  InventoryItem,
  LocalSyncState,
  RecipeRecommendation,
  ReminderItem,
  ShoppingItem,
  SyncDomain,
  SyncEvent,
  SyncSnapshotData,
} from "../types/models";
import { getStorage, setStorage } from "../utils/storage";
import { RequestError } from "../utils/request";
import {
  RECIPE_RECOMMENDATIONS_KEY,
  SHOPPING_ITEMS_KEY,
  updateOfflineSnapshot,
} from "../utils/localFeatures";
import { setAllFridgeZones } from "../utils/fridgeZones";

export const SYNC_QUEUE_KEY = "syncQueue";
export const SYNC_STATE_KEY = "syncState";

const DEFAULT_SYNC_STATE: LocalSyncState = {
  serverRevision: 0,
  dirtyDomains: [],
};

const DEVICE_SYNC_DOMAINS: SyncDomain[] = [
  "inventory",
  "fridge_zones",
  "ai_config",
  "asr_config",
  "tts_config",
  "shopping_list",
  "recipe_cache",
  "reminder",
  "settings",
];

const DEVICE_SEED_WAIT_MS = 1600;
const DEVICE_SEED_POLL_COUNT = 6;
const DEVICE_SEED_POLL_INTERVAL_MS = 1200;

function isSyncRouteMissing(err: unknown): boolean {
  return err instanceof RequestError && err.kind === "http" && err.statusCode === 404;
}

export function getLocalSyncState(): LocalSyncState {
  return { ...DEFAULT_SYNC_STATE, ...(getStorage<LocalSyncState>(SYNC_STATE_KEY) || {}) };
}

export function saveLocalSyncState(patch: Partial<LocalSyncState>): LocalSyncState {
  const current = getLocalSyncState();
  const next: LocalSyncState = {
    ...current,
    ...patch,
    dirtyDomains: patch.dirtyDomains || current.dirtyDomains || [],
  };
  setStorage(SYNC_STATE_KEY, next);
  return next;
}

export function getSyncQueue(): SyncEvent[] {
  return getStorage<SyncEvent[]>(SYNC_QUEUE_KEY) || [];
}

export function markDomainDirty(domain: SyncDomain): void {
  const state = getLocalSyncState();
  const dirty = new Set(state.dirtyDomains || []);
  dirty.add(domain);
  saveLocalSyncState({ dirtyDomains: Array.from(dirty) });
}

export function enqueueSyncOp(
  domain: SyncDomain,
  op: string,
  payload: Record<string, unknown>,
): SyncEvent {
  const state = getLocalSyncState();
  const event: SyncEvent = {
    clientEventId: `miniapp:${domain}:${Date.now()}:${Math.random().toString(36).slice(2, 8)}`,
    domain,
    op,
    source: "miniapp",
    clientRevision: state.serverRevision || 0,
    payload,
    createdAt: new Date().toISOString(),
  };
  const queue = getSyncQueue();
  queue.push(event);
  setStorage(SYNC_QUEUE_KEY, queue.slice(-100));
  markDomainDirty(domain);
  return event;
}

export async function flushSyncQueue(): Promise<void> {
  const queue = getSyncQueue();
  if (!queue.length) return;
  try {
    const result = await pushSyncChanges({ events: queue });
    setStorage(SYNC_QUEUE_KEY, []);
    saveLocalSyncState({
      serverRevision: result.serverRevision,
      lastPushedAt: Date.now(),
      lastSyncError: "",
      dirtyDomains: [],
    });
  } catch (err) {
    const message = isSyncRouteMissing(err) ? "服务器暂未启用同步接口" : err instanceof Error ? err.message : "同步上传失败";
    saveLocalSyncState({ lastSyncError: message });
    if (isSyncRouteMissing(err)) {
      return;
    }
    throw err;
  }
}

export async function pullLatestSnapshot(): Promise<SyncSnapshotData | null> {
  const state = getLocalSyncState();
  try {
    const pulled = await pullSyncChanges(state.serverRevision || 0);
    if (pulled.snapshot) {
      applySnapshotToLocalCache(pulled.snapshot);
    }
    saveLocalSyncState({
      serverRevision: pulled.serverRevision,
      lastPulledAt: Date.now(),
      lastSyncError: "",
    });
    return pulled.snapshot || null;
  } catch (err) {
    const message = isSyncRouteMissing(err) ? "服务器暂未启用同步接口" : err instanceof Error ? err.message : "同步拉取失败";
    saveLocalSyncState({ lastSyncError: message });
    if (isSyncRouteMissing(err)) {
      return null;
    }
    throw err;
  }
}

function delay(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

async function waitForServerRevisionAfter(baseRevision: number): Promise<boolean> {
  for (let i = 0; i < DEVICE_SEED_POLL_COUNT; i++) {
    await delay(i === 0 ? DEVICE_SEED_WAIT_MS : DEVICE_SEED_POLL_INTERVAL_MS);
    try {
      const remote = await getSyncStatus();
      if ((remote.serverRevision || 0) > baseRevision) {
        saveLocalSyncState({
          serverRevision: remote.serverRevision,
          lastSyncError: "",
        });
        return true;
      }
    } catch {
      // 轮询失败时继续等下一次；最终由调用方写入可读错误。
    }
  }
  return false;
}

async function requestInitialDeviceSeed(): Promise<boolean> {
  const state = getLocalSyncState();
  if (state.initialDevicePulledAt) {
    return true;
  }
  const baseRevision = state.serverRevision || 0;
  const result = await pushSyncSnapshotToDevice({
    requestDeviceInventory: true,
    acceptCleanDeviceSnapshot: true,
    pushCloudSnapshot: false,
    domains: ["inventory"],
  });
  if (result.deviceCount > 0 && result.queuedCount === 0) {
    saveLocalSyncState({ lastSyncError: result.errors?.[0] || "设备未在线，等待初次上报" });
    return false;
  }
  const imported = result.queuedCount > 0 ? await waitForServerRevisionAfter(baseRevision) : false;
  if (!imported && result.deviceCount > 0) {
    saveLocalSyncState({ lastSyncError: "等待设备初次上报，暂不覆盖设备数据" });
    return false;
  }
  saveLocalSyncState({
    initialDevicePulledAt: Date.now(),
    lastPushedAt: Date.now(),
    lastSyncError: "",
  });
  return true;
}

export async function syncNow(options: { forceDeviceRefresh?: boolean } = {}): Promise<SyncSnapshotData | null> {
  await flushSyncQueue();
  if (options.forceDeviceRefresh) {
    const baseRevision = getLocalSyncState().serverRevision || 0;
    const result = await pushSyncSnapshotToDevice({
      requestDeviceInventory: true,
      acceptCleanDeviceSnapshot: true,
      pushCloudSnapshot: false,
      domains: ["inventory"],
    });
    if (result.queuedCount > 0) {
      await waitForServerRevisionAfter(baseRevision);
    }
  } else {
    const seeded = await requestInitialDeviceSeed();
    if (!seeded) {
      return null;
    }
  }
  const snapshot = await pullLatestSnapshot();
  const currentRevision = getLocalSyncState().serverRevision || snapshot?.serverRevision || 0;
  if (currentRevision <= 0) {
    saveLocalSyncState({
      lastSyncError: "等待设备快照导入，暂不下发空云端数据",
    });
    return snapshot;
  }
  try {
    await pushSyncSnapshotToDevice({
      requestDeviceInventory: false,
      pushCloudSnapshot: true,
      domains: DEVICE_SYNC_DOMAINS,
    });
    saveLocalSyncState({
      lastPushedAt: Date.now(),
      lastSyncError: "",
    });
  } catch (err) {
    const message = err instanceof Error ? err.message : "设备同步下发失败";
    saveLocalSyncState({ lastSyncError: message });
    throw err;
  }
  return snapshot;
}

export async function refreshSyncStatus(): Promise<LocalSyncState> {
  try {
    const remote = await getSyncStatus();
    return saveLocalSyncState({
      serverRevision: remote.serverRevision,
      lastSyncError: "",
    });
  } catch (err) {
    const message = isSyncRouteMissing(err) ? "服务器暂未启用同步接口" : err instanceof Error ? err.message : "同步状态读取失败";
    return saveLocalSyncState({ lastSyncError: message });
  }
}

export function applySnapshotToLocalCache(snapshot: SyncSnapshotData): void {
  if (snapshot.fridgeZones?.length) {
    setAllFridgeZones(snapshot.fridgeZones);
  }
  if (snapshot.inventory) {
    updateOfflineSnapshot({ inventoryItems: snapshot.inventory as InventoryItem[] });
  }
  if (snapshot.reminders) {
    updateOfflineSnapshot({ reminderItems: snapshot.reminders as ReminderItem[] });
  }
  if (snapshot.shoppingList?.items) {
    setStorage(SHOPPING_ITEMS_KEY, snapshot.shoppingList.items as unknown as ShoppingItem[]);
  }
  if (snapshot.recipeCache?.items) {
    setStorage(RECIPE_RECOMMENDATIONS_KEY, snapshot.recipeCache.items as unknown as RecipeRecommendation[]);
  }
  if (snapshot.settings) {
    const settings = snapshot.settings as Record<string, unknown>;
    if (settings.privacy) {
      setStorage("privacySettings", settings.privacy);
    }
  }
}
