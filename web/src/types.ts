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
  | "get_mqtt_config"
  | "get_mqtt_status"
  | "set_mqtt_config"
  | "clear_mqtt_secret"
  | "mqtt_publish_state"
  | "get_ai_config"
  | "get_ai_profiles"
  | "set_ai_config"
  | "clear_ai_key"
  | "get_asr_config"
  | "set_asr_config"
  | "clear_asr_key"
  | "get_tts_config"
  | "set_tts_config"
  | "clear_tts_key"
  | "create_ai_profile"
  | "select_ai_profile"
  | "delete_ai_profile"
  | "test_ai_chat"
  | "ai_assistant_chat"
  | "wake_start"
  | "wake_stop"
  | "wake_status"
  | "wake_reset_stats"
  | "mic_record_start"
  | "mic_record_status"
  | "mic_record_stop"
  | "mic_record_wav"
  | "voice_chat_start"
  | "voice_chat_stop"
  | "voice_chat_status"
  | "tts_play"
  | "tts_status"
  | "tts_stop"
  | "radar_test_start"
  | "radar_test_status"
  | "radar_test_stop"
  | "get_camera_status"
  | "camera_probe"
  | "camera_capture"
  | "camera_jpeg_diag"
  | "camera_rgb565_diag"
  | "camera_analyze"
  | "clear_camera_frame"
  | "camera_reset"
  | "get_ai_context_preview"
  | "test_ai_task"
  | "get_memory_summary"
  | "set_memory_summary"
  | "clear_memory_summary"
  | "get_chat_history"
  | "clear_chat_history"
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

export interface MQTTConfig {
  brokerUri: string;
  homeId: string;
  deviceId: string;
  username: string;
  password?: string;
  token?: string;
  keepaliveSeconds?: number;
  hasPassword: boolean;
  enabled: boolean;
  configured: boolean;
  connected: boolean;
  reconnectCount: number;
  publishedCount: number;
  receivedCount: number;
  lastError: number;
  statusText: string;
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

export interface ASRConfig {
  apiBaseUrl: string;
  apiKey?: string;
  model: string;
  timeoutMs: number;
  hasApiKey: boolean;
  apiKeyPreview: string;
  lastError: string;
  ready: boolean;
}

export interface TTSConfig {
  apiBaseUrl: string;
  apiKey?: string;
  model: string;
  voice: string;
  timeoutMs: number;
  hasApiKey: boolean;
  apiKeyPreview: string;
  lastError: string;
  ready: boolean;
}

export interface VoiceChatStatus {
  state: "idle" | "recording" | "wake_listening" | "ready" | "error";
  durationMs: number;
  pcmBytes: number;
  rms: number;
  sampleCount?: number;
  peakAbs?: number;
  minSample?: number;
  maxSample?: number;
  meanSample?: number;
  clipCount?: number;
  timeoutCount?: number;
  qualityHint?: "ok" | "silent" | "clipping" | "i2s_timeout" | "too_short" | string;
  error: string;
}

export interface WakeStatus {
  enabled: boolean;
  state: "idle" | "listening" | "error" | string;
  wakeWord: string;
  model: string;
  triggerCount: number;
  lastTriggerMs: number;
  vadState: number;
  rms: number;
  peakAbs: number;
  timeoutCount: number;
  error: string;
}

export type WakeWordDetectedEventPayload = WakeStatus;

export interface VoiceChatResponse {
  transcript: string;
  reply: string;
  asrModel: string;
  aiModel: string;
  asrLatencyMs: number;
  aiLatencyMs: number;
  asrHttpStatus: number;
  aiHttpStatus: number;
  audioBytes: number;
  historyPrunedCount: number;
}

export interface MicRecordWavResponse {
  sampleRate: number;
  channels: number;
  bitsPerSample: number;
  durationMs: number;
  pcmBytes: number;
  wavBytes: number;
  wavBase64: string;
}

export interface TTSStatus {
  state: "idle" | "synthesizing" | "playing" | "done" | "error";
  sampleRate: number;
  audioBytes: number;
  playedBytes: number;
  durationMs: number;
  latencyMs: number;
  httpStatus: number;
  model: string;
  voice: string;
  error: string;
}

export interface CameraStatus {
  initialized: boolean;
  hasFrame: boolean;
  width: number;
  height: number;
  jpegBytes: number;
  captureMs: number;
  frameId: number;
  freeHeapKb: number;
  freePsramKb: number;
  pixelFormat: string;
  frameSize: string;
  lastError: string;
}

export interface CameraProbeResponse {
  ok: boolean;
  xclkEnabled: boolean;
  sccbReady: boolean;
  address: number;
  pidHigh: number;
  pidLow: number;
  pid: number;
  expectedPid: number;
  durationMs: number;
  espErr: number;
  espErrName: string;
  lastError: string;
}

export interface CameraCaptureResponse {
  status: CameraStatus;
  previewDataUrl: string | null;
  previewOmitted: boolean;
  previewMaxBytes: number;
}

export interface CameraRgb565DiagResponse {
  ok: boolean;
  width: number;
  height: number;
  bytes: number;
  captureMs: number;
  checksum: number;
  firstBytes: string;
  espErr: number;
  espErrName: string;
  lastError: string;
}

export interface CameraAnalyzeResponse {
  taskType: ProjectAITaskType;
  reply: string;
  model: string;
  status: string;
  httpStatus: number;
  latencyMs: number;
  width: number;
  height: number;
  jpegBytes: number;
  needsConfirmation: boolean;
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

export type ProjectAITaskType =
  | "chat_assist"
  | "recognize_ingredients"
  | "inventory_parse"
  | "recipe_generate"
  | "shopping_list_generate"
  | "reminder_explain"
  | "voice_intent_parse";

export interface ProjectAITaskRequest {
  taskType: ProjectAITaskType;
  userText: string;
  includeInventory: boolean;
  includeMemory: boolean;
  includeReminders: boolean;
  includePreferences: boolean;
}

export interface AIAssistantHistoryItem {
  role: "user" | "assistant";
  content: string;
}

export interface AIAssistantChatRequest extends ProjectAITaskRequest {
  message: string;
}

export interface AIAssistantChatResponse extends AIChatResponse {
  taskType: ProjectAITaskType;
  contextInjected: boolean;
  localSnapshotVersion: number;
  needsConfirmation: boolean;
  historyInjected: boolean;
  historyCount: number;
  historyPersisted: boolean;
  historyPrunedCount: number;
}

export interface AIContextPreview {
  taskType: ProjectAITaskType;
  localSnapshotVersion: number;
  needsConfirmation: boolean;
  context: unknown;
}

export interface ProjectAITaskResponse {
  taskType: ProjectAITaskType;
  confidence: number;
  needsConfirmation: boolean;
  safetyNote: string;
  result: unknown;
}

export interface MemorySummary {
  schema_version?: number;
  memory_policy?: string;
  family_size?: number;
  taste?: string[];
  avoid?: string[];
  allergies?: string[];
  recent_summary?: string[];
}

export interface DeviceChatHistoryMessage {
  id: string;
  role: "user" | "assistant";
  content: string;
  taskType: string;
  createdAt: number;
}

export interface DeviceChatHistory {
  schemaVersion: number;
  updatedAt: number;
  ttlSeconds: number;
  maxMessages: number;
  timeReady: boolean;
  count: number;
  prunedCount: number;
  messages: DeviceChatHistoryMessage[];
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
  lightRaw12bit?: number;
  lightValue10bit?: number;
  lightPercent?: number;
  lightPolarity?: "raw_high_dark" | "raw_high_bright" | string;
  lightDelta: number;
  imuReady?: boolean;
  imuAddress?: number;
  imuWhoAmI?: number;
  imuError?: number;
  accelXG?: number;
  accelYG?: number;
  accelZG?: number;
  gyroXDps?: number;
  gyroYDps?: number;
  gyroZDps?: number;
  imuTemperatureC?: number;
  pitchDeg?: number;
  rollDeg?: number;
  angleDelta: number;
  vibrationPeak: number;
  radar?: RadarSnapshot;
  touch: string;
  display: string;
  buzzer: string;
  doorState: string;
  updatedAt: string;
}

export interface RadarSnapshot {
  ready: boolean;
  mode: "idle" | "normal" | "report" | "error" | string;
  presence: boolean;
  nearClutter?: boolean;
  staticClutter?: boolean;
  humanCandidate?: boolean;
  stablePresence?: boolean;
  within1m?: boolean;
  approaching?: boolean;
  thresholdPresence?: boolean;
  distanceRaw: number;
  smoothedDistanceRaw?: number;
  gateEnergy: number[];
  peakGate?: number;
  peakEnergy?: number;
  estimatedGate?: number;
  stableGate?: number;
  thresholdGate?: number;
  stableZone?: "unknown" | "near" | "mid" | "far" | string;
  confidence?: number;
  stability?: number;
  approachScore?: number;
  approachFrames?: number;
  approachDistanceDelta?: number;
  motionScore?: number;
  distanceSpan?: number;
  gateSpan?: number;
  energyChangeScore?: number;
  staticScore?: number;
  humanScore?: number;
  thresholdScore?: number;
  holdFramesRemaining?: number;
  nearEnergy?: number;
  midEnergy?: number;
  farEnergy?: number;
  frameCount: number;
  parseErrorCount: number;
  timeoutCount: number;
  ot2Level: number;
  lastText: string;
  targetClass?: string;
  rejectionReason?: string;
  lastError: string;
  updatedAtMs: number;
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
    | "camera"
    | "mic"
    | "speaker"
    | "radar"
    | "pins"
    | "sensors"
    | "logs"
    | "diagnostics"
    | "settings";
  label: string;
  icon: LucideIcon;
}
