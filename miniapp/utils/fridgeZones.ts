/**
 * 冰箱分区工具：统一维护标准分区、自定义分区和位置文案。
 *
 * 小程序优先使用本地缓存保证离线可用；联网时通过 /fridge/zones 与云端同步。
 */

import type { FridgeZoneConfig, StandardSlot } from "../types/models";
import { STANDARD_SLOTS } from "../types/models";
import { getFridgeZonesRemote, updateFridgeZonesRemote } from "../services/api";
import { getStorage, setStorage } from "./storage";

export const STANDARD_ZONE_CONFIGS: FridgeZoneConfig[] = [
  { key: "freezer", label: "上层冷冻", hint: "速冻 / 雪糕", custom: false, width: 2, height: 1 },
  { key: "left", label: "左侧冷藏", hint: "蔬菜 / 熟食", custom: false, width: 1, height: 2 },
  { key: "right", label: "右侧冷藏", hint: "肉类 / 乳制品", custom: false, width: 1, height: 2 },
  { key: "door", label: "门架", hint: "饮品 / 调料", custom: false, width: 1, height: 3 },
];

const CUSTOM_ZONES_KEY = "fridgeCustomZones";
const STANDARD_ZONES_KEY = "fridgeStandardZones";
const ALL_ZONES_CACHE_KEY = "fridgeZonesCloudCache";

export function getFridgeZones(): FridgeZoneConfig[] {
  const cached = getStorage<FridgeZoneConfig[]>(ALL_ZONES_CACHE_KEY);
  if (cached?.length) return sanitizeZones(cached);
  const custom = getStorage<FridgeZoneConfig[]>(CUSTOM_ZONES_KEY) || [];
  const standard = getStorage<FridgeZoneConfig[]>(STANDARD_ZONES_KEY) || STANDARD_ZONE_CONFIGS;
  return sanitizeZones([
    ...standard.filter((z) => !z.custom && z.key && z.label).map((z) => ({
      ...z,
      custom: false,
      width: clampSpan(z.width),
      height: clampSpan(z.height),
    })),
    ...custom.filter((z) => z.key && z.label),
  ]);
}

export function setCustomFridgeZones(zones: FridgeZoneConfig[]): void {
  const standard = getFridgeZones().filter((z) => !z.custom);
  setAllFridgeZones([...standard, ...zones.filter((z) => z.custom)]);
  setStorage(
    CUSTOM_ZONES_KEY,
    zones
      .filter((z) => z.custom && z.key && z.label)
      .map((z) => ({
        ...z,
        custom: true,
        width: clampSpan(z.width),
        height: clampSpan(z.height),
      })),
  );
}

export function setStandardFridgeZones(zones: FridgeZoneConfig[]): void {
  const custom = getFridgeZones().filter((z) => z.custom);
  setAllFridgeZones([...zones.filter((z) => !z.custom), ...custom]);
  setStorage(
    STANDARD_ZONES_KEY,
    zones
      .filter((z) => !z.custom && z.key && z.label)
      .map((z) => ({
        ...z,
        custom: false,
        width: clampSpan(z.width),
        height: clampSpan(z.height),
      })),
  );
}

export function setAllFridgeZones(zones: FridgeZoneConfig[]): FridgeZoneConfig[] {
  const next = sanitizeZones(zones);
  setStorage(ALL_ZONES_CACHE_KEY, next);
  setStorage(STANDARD_ZONES_KEY, next.filter((z) => !z.custom));
  setStorage(CUSTOM_ZONES_KEY, next.filter((z) => z.custom));
  return next;
}

export async function syncFridgeZonesFromCloud(): Promise<FridgeZoneConfig[]> {
  const data = await getFridgeZonesRemote();
  return setAllFridgeZones(data.zones || STANDARD_ZONE_CONFIGS);
}

export async function saveFridgeZonesToCloud(zones: FridgeZoneConfig[]): Promise<FridgeZoneConfig[]> {
  const local = setAllFridgeZones(zones);
  const data = await updateFridgeZonesRemote({ zones: local });
  return setAllFridgeZones(data.zones || local);
}

export function updateStandardFridgeZone(
  key: string,
  patch: Partial<Pick<FridgeZoneConfig, "label" | "hint" | "width" | "height">>,
): FridgeZoneConfig[] {
  const current = getStorage<FridgeZoneConfig[]>(STANDARD_ZONES_KEY) || STANDARD_ZONE_CONFIGS;
  const next = current.map((z) =>
    z.key === key
      ? {
          ...z,
          ...patch,
          custom: false,
          width: clampSpan(patch.width ?? z.width),
          height: clampSpan(patch.height ?? z.height),
        }
      : z,
  );
  setStandardFridgeZones(next);
  return getFridgeZones();
}

export function addCustomFridgeZone(label: string): FridgeZoneConfig[] {
  const all = getFridgeZones();
  const nextNumber =
    all
      .map((z) => {
        const m = z.key.match(/^custom_(\d+)$/);
        return m ? Number(m[1]) : 0;
      })
      .reduce((max, n) => Math.max(max, n), 0) + 1;
  const custom = all.filter((z) => z.custom);
  custom.push({
    key: `custom_${nextNumber}`,
    label: label.trim() || `自定义区 ${nextNumber}`,
    hint: "自定义空间",
    custom: true,
    width: 1,
    height: 1,
  });
  setCustomFridgeZones(custom);
  return getFridgeZones();
}

export function updateCustomFridgeZone(
  key: string,
  patch: Partial<Pick<FridgeZoneConfig, "label" | "hint" | "width" | "height">>,
): FridgeZoneConfig[] {
  const custom = getFridgeZones()
    .filter((z) => z.custom)
    .map((z) =>
      z.key === key
        ? {
            ...z,
            ...patch,
            width: clampSpan(patch.width ?? z.width),
            height: clampSpan(patch.height ?? z.height),
          }
        : z,
    );
  setCustomFridgeZones(custom);
  return getFridgeZones();
}

export function deleteCustomFridgeZone(key: string): FridgeZoneConfig[] {
  const custom = getFridgeZones().filter((z) => z.custom && z.key !== key);
  setCustomFridgeZones(custom);
  return getFridgeZones();
}

export function zoneLabel(zone?: string): string {
  if (!zone) return "未指定";
  return getFridgeZones().find((z) => z.key === zone)?.label || zone;
}

export function slotLabel(slot?: string): string {
  if (!slot) return "未指定";
  const vertical: Record<string, string> = { A: "内", B: "中", C: "外" };
  const horizontal: Record<string, string> = { "1": "左", "2": "中", "3": "右" };
  const row = vertical[slot.charAt(0)] || "";
  const col = horizontal[slot.charAt(1)] || "";
  return row && col ? `${row} · ${col}` : slot;
}

export function displayPlace(zone?: string, slot?: string): string {
  if (!zone && !slot) return "未指定位置";
  if (!slot) return zoneLabel(zone);
  return `${zoneLabel(zone)} · ${slotLabel(slot)}`;
}

export function slotOptions(): Array<{ value: StandardSlot | ""; label: string }> {
  return [
    { value: "", label: "未指定" },
    ...STANDARD_SLOTS.map((slot) => ({
      value: slot,
      label: `${slot} · ${slotLabel(slot)}`,
    })),
  ];
}

function clampSpan(value?: number): number {
  const n = Math.round(Number(value || 1));
  return Math.min(3, Math.max(1, n));
}

function sanitizeZones(zones: FridgeZoneConfig[]): FridgeZoneConfig[] {
  const seen = new Set<string>();
  const next: FridgeZoneConfig[] = [];
  for (const zone of zones) {
    if (!zone.key || !zone.label || seen.has(zone.key)) continue;
    seen.add(zone.key);
    next.push({
      key: zone.key,
      label: zone.label,
      hint: zone.hint || "",
      custom: STANDARD_ZONE_CONFIGS.some((item) => item.key === zone.key) ? false : !!zone.custom,
      width: clampSpan(zone.width),
      height: clampSpan(zone.height),
    });
  }
  const missingStandard = STANDARD_ZONE_CONFIGS.filter((standard) => !seen.has(standard.key));
  return [...missingStandard, ...next];
}
