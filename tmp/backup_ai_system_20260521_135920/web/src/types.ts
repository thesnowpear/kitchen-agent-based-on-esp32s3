import type { LucideIcon } from "lucide-react";

export type TransportMode = "mock" | "serial";
export type ConnectionState = "disconnected" | "connecting" | "connected";
export type HealthState = "ok" | "warn" | "danger" | "offline";
export type SafetyLevel = "safe" | "caution" | "danger";
export type LogLevel = "debug" | "info" | "warn" | "error";

export interface DeviceRequest {
  type: "request";
  request_id: string;
  command: DeviceCommand;
  payload?: unknown;
}

export interface DeviceResponse<TPayload = unknown> {
  type: "response";
  request_id: string;
  ok: boolean;
  command: DeviceCommand;
  payload: TPayload;
  error?: string;
}

export interface DeviceEvent<TPayload = unknown> {
  type: "event";
  event: string;
  payload: TPayload;
}

export type DeviceMessage = DeviceResponse | DeviceEvent;

export type DeviceCommand =
  | "get_status"
  | "get_network"
  | "scan_wifi"
  | "set_network"
  | "get_ai_config"
  | "get_ai_profiles"
  | "set_ai_config"
  | "clear_ai_key"
  | "create_ai_profile"
  | "select_ai_profile"
  | "delete_ai_profile"
  | "test_ai_chat"
  | "get_pins"
  | "get_sensors"
  | "get_diagnostics"
  | "get_logs";

export interface DeviceStatus {
  model: string;
  chip: string;
  firmware: string;
  uptime: string;
  flash: string;
  psram: string;
  freeHeapKb: number;
  minHeapKb: number;
  freePsramKb: number;
  temperatureC: number | null;
  wifi: HealthState;
  mqtt: HealthState;
  usb: HealthState;
  ota: HealthState;
  page: string;
  powerNote: string;
  tasks: TaskStatus[];
}

export interface TaskStatus {
  name: string;
  priority: string;
  state: string;
  heartbeat: string;
}

export interface NetworkConfig {
  ssid: string;
  wifiPassword?: string;
  mqttHost: string;
  apiBaseUrl: string;
  ntpServer: string;
  save?: boolean;
  saveAiKey?: false;
  connected?: boolean;
  saved?: boolean;
  internet?: boolean;
  status?: string;
  ip?: string;
  rssi?: number;
  lastError?: string;
}

export interface AIConfig {
  profileId?: number;
  profileName?: string;
  apiBaseUrl: string;
  apiKey?: string;
  model: string;
  systemPrompt: string;
  timeoutMs: number;
  hasApiKey: boolean;
  apiKeyPreview: string;
  lastError: string;
  ready: boolean;
}

export interface AIProfilesResponse {
  activeProfileId: number;
  profiles: AIConfig[];
}

export interface AIChatMessage {
  id: string;
  role: "user" | "assistant";
  content: string;
  at: string;
  status?: "ok" | "error" | "pending";
  latencyMs?: number;
}

export interface AIChatResponse {
  reply: string;
  model: string;
  latencyMs: number;
  status: string;
  httpStatus?: number;
}

export interface WifiNetwork {
  ssid: string;
  band: "2.4G" | "5G";
  signal: number;
  rssi?: number;
  channel?: number;
  secured: boolean;
  authmode?: string;
  note: string;
}

export interface PinInfo {
  gpio: string;
  signal: string;
  usage: string;
  level: SafetyLevel;
  note: string;
  readonly: boolean;
}

export interface SensorSnapshot {
  pir: boolean;
  lux: number;
  lightDelta: number;
  angleDelta: number;
  vibrationPeak: number;
  touch: string;
  display: string;
  buzzer: string;
  doorState: string;
  updatedAt: string;
}

export interface DiagnosticSnapshot {
  psram: string;
  flashPartition: string;
  littlefs: string;
  otaSlot: string;
  brownoutCount: number;
  watchdogCount: number;
  lastError: string;
  riskItems: string[];
}

export interface DeviceLog {
  id: string;
  at: string;
  level: LogLevel;
  source: string;
  message: string;
}

export interface SectionDefinition {
  id:
    | "overview"
    | "usb"
    | "network"
    | "ai"
    | "pins"
    | "sensors"
    | "logs"
    | "diagnostics"
    | "settings";
  label: string;
  icon: LucideIcon;
}
