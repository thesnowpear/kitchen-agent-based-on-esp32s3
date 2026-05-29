import {
  Activity,
  Bot,
  Cable,
  Camera,
  Check,
  Cpu,
  Download,
  Info,
  KeyRound,
  LayoutDashboard,
  LockKeyhole,
  Mic,
  Play,
  Plus,
  Power,
  Radio,
  RotateCw,
  Search,
  Send,
  Settings,
  ShieldAlert,
  SlidersHorizontal,
  Volume2,
  Sparkles,
  Terminal,
  Trash2,
  Unplug,
  Wifi,
} from "lucide-react";
import { FormEvent, useCallback, useEffect, useMemo, useRef, useState } from "react";
import { MockTransport } from "./transports/MockTransport";
import { WebSerialTransport } from "./transports/WebSerialTransport";
import type {
  AIAssistantChatResponse,
  AIChatMessage,
  AIConfig,
  AIContextPreview,
  AIProfilesResponse,
  ASRConfig,
  CameraAnalyzeResponse,
  CameraCaptureResponse,
  CameraProbeResponse,
  CameraRgb565DiagResponse,
  CameraStatus,
  ConnectionState,
  DeviceChatHistory,
  DeviceCommand,
  DeviceLog,
  DeviceMessage,
  DeviceStatus,
  DiagnosticSnapshot,
  LogLevel,
  MemorySummary,
  MicRecordWavResponse,
  MQTTConfig,
  NetworkConfig,
  PinInfo,
  ProjectAITaskRequest,
  ProjectAITaskResponse,
  ProjectAITaskType,
  RadarSnapshot,
  SectionDefinition,
  SensorSnapshot,
  StateMachineConfig,
  StateMachineStatus,
  TransportMode,
  TTSConfig,
  TTSStatus,
  WakeStatus,
  WakeWordDetectedEventPayload,
  VoiceChatResponse,
  VoiceChatStatus,
  WifiNetwork,
} from "./types";

const sections: SectionDefinition[] = [
  { id: "overview", label: "总览", icon: LayoutDashboard },
  { id: "usb", label: "USB 连接", icon: Cable },
  { id: "network", label: "网络配置", icon: Wifi },
  { id: "ai", label: "AI 助手", icon: Sparkles },
  { id: "camera", label: "摄像头测试", icon: Camera },
  { id: "mic", label: "麦克风测试", icon: Mic },
  { id: "speaker", label: "扬声器测试", icon: Volume2 },
  { id: "radar", label: "雷达测试", icon: Radio },
  { id: "pins", label: "GPIO/引脚", icon: Cpu },
  { id: "sensors", label: "传感器", icon: Activity },
  { id: "logs", label: "日志", icon: Terminal },
  { id: "diagnostics", label: "诊断", icon: ShieldAlert },
  { id: "settings", label: "设置", icon: Settings },
];

const levelLabel: Record<LogLevel, string> = {
  debug: "调试",
  info: "信息",
  warn: "警告",
  error: "错误",
};

const defaultAiConfig: AIConfig = {
  profileId: 0,
  profileName: "默认配置",
  apiBaseUrl: "https://api.openai.com/v1",
  model: "gpt-4o-mini",
  systemPrompt: "你是冰箱小精灵的开发测试助手，请用简短中文回答。",
  timeoutMs: 30000,
  hasApiKey: false,
  apiKeyPreview: "",
  lastError: "",
  ready: false,
};

const defaultAsrConfig: ASRConfig = {
  apiBaseUrl: "https://api.siliconflow.cn/v1/audio/transcriptions",
  model: "TeleAI/TeleSpeechASR",
  timeoutMs: 45000,
  hasApiKey: false,
  apiKeyPreview: "",
  lastError: "",
  ready: false,
};

const defaultTtsConfig: TTSConfig = {
  apiBaseUrl: "https://api.siliconflow.cn/v1/audio/speech",
  model: "fnlp/MOSS-TTSD-v0.5",
  voice: "fnlp/MOSS-TTSD-v0.5:alex",
  timeoutMs: 45000,
  hasApiKey: false,
  apiKeyPreview: "",
  lastError: "",
  ready: false,
};

type WakeEventRecord = WakeWordDetectedEventPayload & {
  id: string;
  receivedAt: string;
};

type MicPlayback = {
  url: string;
  durationMs: number;
  pcmBytes: number;
  wavBytes: number;
  sampleRate: number;
  channels: number;
  bitsPerSample: number;
  createdAt: string;
};

const AI_SYSTEM_PROMPT_MAX_BYTES = 8192;
const utf8Encoder = new TextEncoder();
const utf8ByteLength = (value: string) => utf8Encoder.encode(value).length;

function base64ToBlob(base64: string, mimeType: string) {
  const binary = window.atob(base64);
  const chunks: BlobPart[] = [];
  for (let offset = 0; offset < binary.length; offset += 8192) {
    const slice = binary.slice(offset, offset + 8192);
    const bytes = new Uint8Array(slice.length);
    for (let index = 0; index < slice.length; index += 1) {
      bytes[index] = slice.charCodeAt(index);
    }
    chunks.push(bytes);
  }
  return new Blob(chunks, { type: mimeType });
}

function normalizeTtsVoiceForRequest(model: string, voice: string) {
  const trimmedModel = model.trim();
  const trimmedVoice = voice.trim();
  if (trimmedModel === "fnlp/MOSS-TTSD-v0.5") {
    if (!trimmedVoice || trimmedVoice === "alloy") {
      return "fnlp/MOSS-TTSD-v0.5:alex";
    }
    if (!trimmedVoice.includes(":")) {
      return `${trimmedModel}:${trimmedVoice}`;
    }
  }
  return trimmedVoice || defaultTtsConfig.voice;
}

const projectAiTaskLabels: Record<ProjectAITaskType, string> = {
  chat_assist: "厨房助手问答",
  recognize_ingredients: "拍照识别候选",
  inventory_parse: "库存变更解析",
  recipe_generate: "菜谱推荐",
  shopping_list_generate: "购物清单",
  reminder_explain: "临期提醒解释",
  voice_intent_parse: "语音意图解析",
  kitchen_tool_control: "厨房工具控制",
};

const defaultProjectAiRequest: ProjectAITaskRequest = {
  taskType: "recipe_generate",
  userText: "今晚想用快过期的食材做一道 30 分钟以内的家常菜。",
  includeInventory: true,
  includeMemory: true,
  includeReminders: true,
  includePreferences: true,
};

const nowTime = () =>
  new Intl.DateTimeFormat("zh-CN", {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  }).format(new Date());

const formatHistoryTime = (createdAt: number) => {
  if (!createdAt) {
    return "--:--:--";
  }
  return new Intl.DateTimeFormat("zh-CN", {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  }).format(new Date(createdAt * 1000));
};

const deviceHistoryToChatMessages = (history: DeviceChatHistory): AIChatMessage[] =>
  history.messages.map((message, index) => ({
    id: message.id || `device-history-${message.createdAt}-${index}`,
    role: message.role === "assistant" ? "assistant" : "user",
    content: message.content,
    at: formatHistoryTime(message.createdAt),
    status: "ok",
  }));

const toErrorMessage = (error: unknown) => (error instanceof Error ? error.message : String(error));

const sleep = (ms: number) => new Promise<void>((resolve) => window.setTimeout(resolve, ms));

const toNetworkErrorMessage = (error: unknown) => {
  const message = toErrorMessage(error);
  if (message.includes("ESP_ERR_WIFI_CONN")) {
    return "Wi-Fi 状态机仍在连接中，请等待上一轮连接结束后重试。新固件会主动收尾旧连接，避免连续点击触发这个错误。";
  }
  if (message.includes("ESP_ERR_TIMEOUT")) {
    return "Wi-Fi 连接超时：请检查密码、热点是否为 2.4GHz、信号强度和开发板供电。";
  }
  return message;
};

const mergeNetworkStatus = (current: NetworkConfig | null, incoming: NetworkConfig, preserveForm: boolean): NetworkConfig => {
  if (!preserveForm || !current) {
    return incoming;
  }
  return {
    ...incoming,
    ssid: current.ssid,
    wifiPassword: current.wifiPassword ?? "",
    mqttHost: current.mqttHost,
    apiBaseUrl: current.apiBaseUrl,
    ntpServer: current.ntpServer,
    save: current.save,
    saveAiKey: false,
  };
};

function App() {
  const [activeSection, setActiveSection] = useState<SectionDefinition["id"]>("overview");
  const [transportMode, setTransportMode] = useState<TransportMode>("mock");
  const [connection, setConnection] = useState<ConnectionState>("disconnected");
  const [connectionNotice, setConnectionNotice] = useState("");
  const [timeoutMs, setTimeoutMs] = useState(45000);
  const [refreshSeconds, setRefreshSeconds] = useState(15);
  const [status, setStatus] = useState<DeviceStatus | null>(null);
  const [network, setNetwork] = useState<NetworkConfig | null>(null);
  const [mqttConfig, setMqttConfig] = useState<MQTTConfig | null>(null);
  const [mqttTokenInput, setMqttTokenInput] = useState("");
  const [aiConfig, setAiConfig] = useState<AIConfig | null>(null);
  const [aiProfiles, setAiProfiles] = useState<AIConfig[]>([]);
  const [aiKeyInput, setAiKeyInput] = useState("");
  const [asrConfig, setAsrConfig] = useState<ASRConfig | null>(defaultAsrConfig);
  const [asrKeyInput, setAsrKeyInput] = useState("");
  const [ttsConfig, setTtsConfig] = useState<TTSConfig | null>(defaultTtsConfig);
  const [ttsKeyInput, setTtsKeyInput] = useState("");
  const [ttsText, setTtsText] = useState("冰箱小精灵扬声器测试，当前播放来自云端 TTS。");
  const [ttsStatus, setTtsStatus] = useState<TTSStatus | null>(null);
  const [ttsBusy, setTtsBusy] = useState(false);
  const [browserTtsBusy, setBrowserTtsBusy] = useState(false);
  const [cameraStatus, setCameraStatus] = useState<CameraStatus | null>(null);
  const [cameraProbe, setCameraProbe] = useState<CameraProbeResponse | null>(null);
  const [cameraRgb565Diag, setCameraRgb565Diag] = useState<CameraRgb565DiagResponse | null>(null);
  const [cameraPreview, setCameraPreview] = useState<string | null>(null);
  const [cameraAnalyzeResult, setCameraAnalyzeResult] = useState<CameraAnalyzeResponse | null>(null);
  const [cameraBusy, setCameraBusy] = useState(false);
  const [cameraLive, setCameraLive] = useState(false);
  const [cameraLiveFrames, setCameraLiveFrames] = useState(0);
  const [cameraLiveLastAt, setCameraLiveLastAt] = useState("");
  const [cameraLiveError, setCameraLiveError] = useState("");
  const [voiceStatus, setVoiceStatus] = useState<VoiceChatStatus | null>(null);
  const [voiceBusy, setVoiceBusy] = useState(false);
  const [micTestMode, setMicTestMode] = useState<"idle" | "hardware" | "asr">("idle");
  const [micSamples, setMicSamples] = useState<VoiceChatStatus[]>([]);
  const [micTranscript, setMicTranscript] = useState("");
  const [micAiReply, setMicAiReply] = useState("");
  const [micAsrResult, setMicAsrResult] = useState<VoiceChatResponse | null>(null);
  const [micPlayback, setMicPlayback] = useState<MicPlayback | null>(null);
  const [wakeStatus, setWakeStatus] = useState<WakeStatus | null>(null);
  const [wakeBusy, setWakeBusy] = useState(false);
  const [wakeEvents, setWakeEvents] = useState<WakeEventRecord[]>([]);
  const [radarStatus, setRadarStatus] = useState<RadarSnapshot | null>(null);
  const [radarSamples, setRadarSamples] = useState<RadarSnapshot[]>([]);
  const [radarRunning, setRadarRunning] = useState(false);
  const [radarBusy, setRadarBusy] = useState(false);
  const [aiChatDraft, setAiChatDraft] = useState("");
  const [aiMessages, setAiMessages] = useState<AIChatMessage[]>([]);
  const [aiBusy, setAiBusy] = useState(false);
  const [projectAiRequest, setProjectAiRequest] = useState<ProjectAITaskRequest>(defaultProjectAiRequest);
  const [aiContextPreview, setAiContextPreview] = useState<AIContextPreview | null>(null);
  const [deviceChatHistory, setDeviceChatHistory] = useState<DeviceChatHistory | null>(null);
  const [memorySummary, setMemorySummary] = useState<MemorySummary | null>(null);
  const [memoryDraft, setMemoryDraft] = useState(JSON.stringify({
    schema_version: 1,
    memory_policy: "硬件测试记忆：只保存结构化摘要，不保存完整聊天记录",
    family_size: 2,
    taste: ["清淡", "少油"],
    avoid: ["香菜"],
    allergies: [],
    recent_summary: ["用户希望优先处理临期食材"],
  }, null, 2));
  const [projectAiBusy, setProjectAiBusy] = useState(false);
  const [wifiNetworks, setWifiNetworks] = useState<WifiNetwork[]>([]);
  const [wifiScanState, setWifiScanState] = useState<"idle" | "scanning" | "done" | "error">("idle");
  const [networkBusy, setNetworkBusy] = useState(false);
  const [pins, setPins] = useState<PinInfo[]>([]);
  const [sensors, setSensors] = useState<SensorSnapshot | null>(null);
  const [stateMachineConfig, setStateMachineConfig] = useState<StateMachineConfig | null>(null);
  const [stateMachineStatus, setStateMachineStatus] = useState<StateMachineStatus | null>(null);
  const [diagnostics, setDiagnostics] = useState<DiagnosticSnapshot | null>(null);
  const [logs, setLogs] = useState<DeviceLog[]>([]);
  const [logFilter, setLogFilter] = useState<LogLevel | "all">("all");
  const [searchTerm, setSearchTerm] = useState("");
  const [busy, setBusy] = useState(false);
  const transportRef = useRef<MockTransport | WebSerialTransport | null>(null);
  const refreshInFlightRef = useRef(false);
  const commandInFlightRef = useRef(false);
  const sensorRefreshInFlightRef = useRef(false);
  const radarRefreshInFlightRef = useRef(false);
  const wakeStatusInFlightRef = useRef(false);
  const staticInfoLoadedRef = useRef(false);
  const networkDirtyRef = useRef(false);
  const aiConfigDirtyRef = useRef(false);
  const asrConfigDirtyRef = useRef(false);
  const ttsConfigDirtyRef = useRef(false);
  const stateMachineConfigDirtyRef = useRef(false);
  const micPlaybackUrlRef = useRef<string | null>(null);

  const appendLog = useCallback((level: LogLevel, message: string, source = "web") => {
    setLogs((current) => [
      {
        id: crypto.randomUUID(),
        at: new Intl.DateTimeFormat("zh-CN", {
          hour: "2-digit",
          minute: "2-digit",
          second: "2-digit",
        }).format(new Date()),
        level,
        source,
        message,
      },
      ...current,
    ].slice(0, 180));
  }, []);

  const pushMicSample = useCallback((sample: VoiceChatStatus) => {
    setMicSamples((current) => [...current.slice(-29), sample]);
  }, []);

  const clearMicPlayback = useCallback(() => {
    if (micPlaybackUrlRef.current) {
      URL.revokeObjectURL(micPlaybackUrlRef.current);
      micPlaybackUrlRef.current = null;
    }
    setMicPlayback(null);
  }, []);

  const pushRadarSample = useCallback((sample: RadarSnapshot) => {
    setRadarSamples((current) => [...current.slice(-59), sample]);
  }, []);

  const handleDeviceMessage = useCallback((message: DeviceMessage) => {
    if (message.type !== "event" || message.event !== "wake_word_detected") {
      return;
    }
    const payload = message.payload as WakeWordDetectedEventPayload;
    setWakeStatus(payload);
    setWakeEvents((current) => [
      {
        ...payload,
        id: crypto.randomUUID(),
        receivedAt: nowTime(),
      },
      ...current,
    ].slice(0, 12));
    appendLog("info", `检测到唤醒词：${payload.wakeWord || "小冰小冰"}，累计 ${payload.triggerCount} 次。`, "wake");
  }, [appendLog]);

  useEffect(() => {
    return () => {
      if (micPlaybackUrlRef.current) {
        URL.revokeObjectURL(micPlaybackUrlRef.current);
        micPlaybackUrlRef.current = null;
      }
    };
  }, []);

  const createTransport = useCallback(() => {
    return transportMode === "mock" ? new MockTransport() : new WebSerialTransport(timeoutMs);
  }, [timeoutMs, transportMode]);

  const clearFrontendData = useCallback(() => {
    setStatus(null);
    setConnectionNotice("");
    setNetwork(null);
    setAiConfig(null);
    setAiProfiles([]);
    setAiKeyInput("");
    setAsrConfig(defaultAsrConfig);
    setAsrKeyInput("");
    setTtsConfig(defaultTtsConfig);
    setTtsKeyInput("");
    setTtsStatus(null);
    setTtsBusy(false);
    setBrowserTtsBusy(false);
    setCameraStatus(null);
    setCameraProbe(null);
    setCameraRgb565Diag(null);
    setCameraPreview(null);
    setCameraAnalyzeResult(null);
    setCameraBusy(false);
    setCameraLive(false);
    setCameraLiveFrames(0);
    setCameraLiveLastAt("");
    setCameraLiveError("");
    setVoiceStatus(null);
    setVoiceBusy(false);
    clearMicPlayback();
    setWakeStatus(null);
    setWakeBusy(false);
    setWakeEvents([]);
    setRadarStatus(null);
    setRadarSamples([]);
    setRadarRunning(false);
    setRadarBusy(false);
    setAiChatDraft("");
    setAiMessages([]);
    setAiBusy(false);
    setProjectAiRequest(defaultProjectAiRequest);
    setAiContextPreview(null);
    setDeviceChatHistory(null);
    setMemorySummary(null);
    setMemoryDraft(JSON.stringify({
      schema_version: 1,
      memory_policy: "硬件测试记忆：只保存结构化摘要，不保存完整聊天记录",
      family_size: 2,
      taste: ["清淡", "少油"],
      avoid: ["香菜"],
      allergies: [],
      recent_summary: ["用户希望优先处理临期食材"],
    }, null, 2));
    setProjectAiBusy(false);
    setWifiNetworks([]);
    setWifiScanState("idle");
    setNetworkBusy(false);
    setPins([]);
    setSensors(null);
    setStateMachineConfig(null);
    setStateMachineStatus(null);
    setDiagnostics(null);
    setLogs([]);
    setBusy(false);
    sensorRefreshInFlightRef.current = false;
    radarRefreshInFlightRef.current = false;
    wakeStatusInFlightRef.current = false;
    staticInfoLoadedRef.current = false;
    networkDirtyRef.current = false;
    aiConfigDirtyRef.current = false;
    asrConfigDirtyRef.current = false;
    ttsConfigDirtyRef.current = false;
    stateMachineConfigDirtyRef.current = false;
    setSearchTerm("");
    setLogFilter("all");
  }, [clearMicPlayback]);

  const setNetworkDraft = useCallback((next: NetworkConfig) => {
    networkDirtyRef.current = true;
    setNetwork(next);
  }, []);

  const setAiConfigDraft = useCallback((next: AIConfig) => {
    aiConfigDirtyRef.current = true;
    setAiConfig(next);
  }, []);

  const setAsrConfigDraft = useCallback((next: ASRConfig) => {
    asrConfigDirtyRef.current = true;
    setAsrConfig(next);
  }, []);

  const setTtsConfigDraft = useCallback((next: TTSConfig) => {
    ttsConfigDirtyRef.current = true;
    setTtsConfig(next);
  }, []);

  const setStateMachineConfigDraft = useCallback((next: StateMachineConfig) => {
    stateMachineConfigDirtyRef.current = true;
    setStateMachineConfig(next);
  }, []);

  const changeTransportMode = useCallback(
    async (mode: TransportMode) => {
      if (mode === transportMode) {
        return;
      }
      await transportRef.current?.disconnect().catch(() => undefined);
      transportRef.current = null;
      setConnection("disconnected");
      clearFrontendData();
      setTransportMode(mode);
    },
    [clearFrontendData, transportMode],
  );

  const refreshAll = useCallback(async (options: { full?: boolean } = {}) => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      return;
    }
    if (refreshInFlightRef.current || commandInFlightRef.current) {
      return;
    }
    refreshInFlightRef.current = true;
    setBusy(true);
    try {
      const shouldReadStatic = options.full === true || !staticInfoLoadedRef.current;
      const failures: string[] = [];
      const read = async <TPayload,>(command: DeviceCommand) => {
        const response = await transport.sendCommand<TPayload>(command);
        return response.payload;
      };
      const optionalRead = async <TPayload,>(
        command: DeviceCommand,
        label: string,
        apply: (payload: TPayload) => void,
      ) => {
        try {
          apply(await read<TPayload>(command));
        } catch (error) {
          const message = toErrorMessage(error);
          failures.push(`${label}: ${message}`);
          appendLog("warn", `${label}读取失败：${message}`, "web");
        }
      };

      setStatus(await read<DeviceStatus>("get_status"));
      await optionalRead<NetworkConfig>("get_network", "网络状态", (payload) => {
        setNetwork((current) => mergeNetworkStatus(current, payload, networkDirtyRef.current));
      });
      await optionalRead<MQTTConfig>("get_mqtt_config", "MQTT 配置", setMqttConfig);
      await optionalRead<AIConfig>("get_ai_config", "AI API 配置", (payload) => {
        setAiConfig((current) => (aiConfigDirtyRef.current && current ? { ...payload, ...current } : payload));
      });
      await optionalRead<SensorSnapshot>("get_sensors", "传感器状态", setSensors);
      await optionalRead<StateMachineStatus>("get_state_machine_status", "状态机状态", setStateMachineStatus);
      await optionalRead<StateMachineConfig>("get_state_machine_config", "状态机配置", (payload) => {
        setStateMachineConfig((current) => (stateMachineConfigDirtyRef.current && current ? { ...payload, ...current } : payload));
      });

      await optionalRead<ASRConfig>("get_asr_config", "ASR API config", (payload) => {
        setAsrConfig((current) => (asrConfigDirtyRef.current && current ? { ...payload, ...current } : payload));
      });
      await optionalRead<TTSConfig>("get_tts_config", "TTS API config", (payload) => {
        setTtsConfig((current) => (ttsConfigDirtyRef.current && current ? { ...payload, ...current } : payload));
      });
      await optionalRead<WakeStatus>("wake_status", "唤醒词状态", setWakeStatus);

      if (shouldReadStatic) {
        await optionalRead<PinInfo[]>("get_pins", "GPIO 信息", setPins);
        await optionalRead<DiagnosticSnapshot>("get_diagnostics", "诊断信息", setDiagnostics);
        await optionalRead<DeviceLog[]>("get_logs", "设备日志", (payload) => {
          setLogs((current) => [...payload, ...current].slice(0, 180));
        });
        staticInfoLoadedRef.current = true;
      }
      setConnectionNotice(
        failures.length > 0
          ? `设备已连接，但部分命令未响应：${failures.slice(0, 2).join("；")}${failures.length > 2 ? "；..." : ""}`
          : "",
      );
    } catch (error) {
      const message = toErrorMessage(error);
      setConnectionNotice(`设备已连接但没有正常响应：${message}`);
      appendLog("error", message, "web");
    } finally {
      refreshInFlightRef.current = false;
      setBusy(false);
    }
  }, [appendLog, connection]);

  const refreshSensorsOnly = useCallback(async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected" || commandInFlightRef.current || sensorRefreshInFlightRef.current) {
      return;
    }
    sensorRefreshInFlightRef.current = true;
    try {
      const response = await transport.sendCommand<SensorSnapshot>("get_sensors");
      setSensors(response.payload);
    } catch (error) {
      appendLog("warn", `传感器快照读取失败：${toErrorMessage(error)}`, "sensors");
    } finally {
      sensorRefreshInFlightRef.current = false;
    }
  }, [appendLog, connection]);

  const refreshRadarStatus = useCallback(async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected" || commandInFlightRef.current || radarRefreshInFlightRef.current) {
      return;
    }
    radarRefreshInFlightRef.current = true;
    try {
      const response = await transport.sendCommand<RadarSnapshot>("radar_test_status");
      setRadarStatus(response.payload);
      setRadarRunning(response.payload.mode === "report");
      pushRadarSample(response.payload);
    } catch (error) {
      appendLog("warn", `雷达状态读取失败：${toErrorMessage(error)}`, "radar");
    } finally {
      radarRefreshInFlightRef.current = false;
    }
  }, [appendLog, connection, pushRadarSample]);

  const startRadarTest = useCallback(async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接设备，再测试 24G 雷达。", "radar");
      return;
    }
    try {
      commandInFlightRef.current = true;
      setRadarBusy(true);
      const response = await transport.sendCommand<RadarSnapshot>("radar_test_start");
      setRadarStatus(response.payload);
      setRadarSamples([response.payload]);
      setRadarRunning(response.payload.mode === "report");
      appendLog("info", "24G 雷达已切换到上报模式。", "radar");
    } catch (error) {
      appendLog("error", toErrorMessage(error), "radar");
    } finally {
      commandInFlightRef.current = false;
      setRadarBusy(false);
    }
  }, [appendLog, connection]);

  const stopRadarTest = useCallback(async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接设备，再停止 24G 雷达测试。", "radar");
      return;
    }
    try {
      commandInFlightRef.current = true;
      setRadarBusy(true);
      const response = await transport.sendCommand<RadarSnapshot>("radar_test_stop");
      setRadarStatus(response.payload);
      pushRadarSample(response.payload);
      setRadarRunning(response.payload.mode === "report");
      appendLog("info", "24G 雷达已切回正常模式。", "radar");
    } catch (error) {
      appendLog("error", toErrorMessage(error), "radar");
    } finally {
      commandInFlightRef.current = false;
      setRadarBusy(false);
    }
  }, [appendLog, connection, pushRadarSample]);

  const refreshWakeStatus = useCallback(async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected" || commandInFlightRef.current || wakeStatusInFlightRef.current) {
      return;
    }
    wakeStatusInFlightRef.current = true;
    try {
      const response = await transport.sendCommand<WakeStatus>("wake_status");
      setWakeStatus(response.payload);
    } catch (error) {
      appendLog("warn", `唤醒词状态读取失败：${toErrorMessage(error)}`, "wake");
    } finally {
      wakeStatusInFlightRef.current = false;
    }
  }, [appendLog, connection]);

  const startWakeListening = useCallback(async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接设备，再启动唤醒词监听。", "wake");
      return;
    }
    if (voiceBusy || aiBusy || ttsBusy || commandInFlightRef.current || voiceStatus?.state === "recording") {
      appendLog("warn", "录音、AI 或 TTS 正在占用音频链路，请稍后再启动唤醒监听。", "wake");
      return;
    }
    try {
      commandInFlightRef.current = true;
      setWakeBusy(true);
      const response = await transport.sendCommand<WakeStatus>("wake_start");
      setWakeStatus(response.payload);
      setVoiceStatus((current) => current ? { ...current, state: "wake_listening", error: "" } : current);
      appendLog("info", `WakeNet 已开始监听：${response.payload.wakeWord} / ${response.payload.model}`, "wake");
    } catch (error) {
      appendLog("error", toErrorMessage(error), "wake");
    } finally {
      commandInFlightRef.current = false;
      setWakeBusy(false);
    }
  }, [aiBusy, appendLog, connection, ttsBusy, voiceBusy, voiceStatus?.state]);

  const stopWakeListening = useCallback(async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接设备，再停止唤醒词监听。", "wake");
      return;
    }
    try {
      commandInFlightRef.current = true;
      setWakeBusy(true);
      const response = await transport.sendCommand<WakeStatus>("wake_stop");
      setWakeStatus(response.payload);
      setVoiceStatus((current) => current?.state === "wake_listening" ? { ...current, state: "idle" } : current);
      appendLog("info", "WakeNet 监听已停止。", "wake");
    } catch (error) {
      appendLog("error", toErrorMessage(error), "wake");
    } finally {
      commandInFlightRef.current = false;
      setWakeBusy(false);
    }
  }, [appendLog, connection]);

  const resetWakeStats = useCallback(async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接设备，再清空唤醒统计。", "wake");
      return;
    }
    try {
      commandInFlightRef.current = true;
      setWakeBusy(true);
      const response = await transport.sendCommand<WakeStatus>("wake_reset_stats");
      setWakeStatus(response.payload);
      setWakeEvents([]);
      appendLog("info", "WakeNet 触发统计已清空。", "wake");
    } catch (error) {
      appendLog("error", toErrorMessage(error), "wake");
    } finally {
      commandInFlightRef.current = false;
      setWakeBusy(false);
    }
  }, [appendLog, connection]);

  const connect = async () => {
    if (connection !== "disconnected") {
      return;
    }
    setConnection("connecting");
    setConnectionNotice("正在请求浏览器串口权限；选择端口后会打开串口，若端口被占用会在 10 秒内提示。");
    const transport = createTransport();
    transport.onLog(appendLog);
    transport.onMessage(handleDeviceMessage);
    transportRef.current = transport;

    try {
      await transport.connect();
      if (transportMode === "serial") {
        setConnectionNotice("USB 串口已打开，等待开发板复位后启动协议任务...");
        await sleep(1800);
      }
      setConnection("connected");
      setConnectionNotice("");
      appendLog("info", transportMode === "mock" ? "已进入 Mock 运维模式。" : "已连接 USB 串口。");
    } catch (error) {
      await transport.disconnect().catch(() => undefined);
      transportRef.current = null;
      setConnection("disconnected");
      const message = toErrorMessage(error);
      setConnectionNotice(`连接失败：${message}`);
      appendLog("error", message, "web");
    }
  };

  const disconnect = async () => {
    await transportRef.current?.disconnect();
    transportRef.current = null;
    setConnection("disconnected");
    setConnectionNotice("");
  };

  const saveNetwork = async (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    if (!network || !transportRef.current) {
      return;
    }
    if (commandInFlightRef.current || networkBusy) {
      appendLog("warn", "设备正在处理上一条命令，请等待完成后再连接。", "network");
      return;
    }
    try {
      commandInFlightRef.current = true;
      setNetworkBusy(true);
      setConnectionNotice(`正在连接 Wi-Fi：${network.ssid}，请等待设备完成认证和获取 IP。`);
      appendLog("info", `正在连接 Wi-Fi：${network.ssid}`, "network");
      const response = await transportRef.current.sendCommand<NetworkConfig>("set_network", {
        ...network,
        save: true,
      });
      networkDirtyRef.current = false;
      setNetwork(response.payload);
      setConnectionNotice(response.payload.connected ? `Wi-Fi 已连接：${response.payload.ssid || network.ssid}` : "Wi-Fi 配置已发送，请查看网络状态。");
      appendLog("info", response.payload.connected ? `Wi-Fi 已连接：${response.payload.ssid || network.ssid}` : "网络配置已发送。", "network");
      commandInFlightRef.current = false;
      await refreshAll({ full: false });
    } catch (error) {
      const message = toNetworkErrorMessage(error);
      setConnectionNotice(`Wi-Fi 连接失败：${message}`);
      appendLog("error", message, "network");
    } finally {
      commandInFlightRef.current = false;
      setNetworkBusy(false);
    }
  };

  const saveMqttConfig = async (next: MQTTConfig, token: string) => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接 Mock 或 USB 设备，再写入 MQTT 配置。", "mqtt");
      return;
    }
    try {
      commandInFlightRef.current = true;
      const response = await transport.sendCommand<MQTTConfig>("set_mqtt_config", {
        ...next,
        token: token.trim() || undefined,
      });
      setMqttConfig(response.payload);
      setMqttTokenInput("");
      appendLog("info", "MQTT 云端绑定配置已写入设备，token 不会回显。", "mqtt");
      await refreshAll({ full: false });
    } catch (error) {
      appendLog("error", toErrorMessage(error), "mqtt");
    } finally {
      commandInFlightRef.current = false;
    }
  };

  const publishMqttState = async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接 Mock 或 USB 设备，再发布 MQTT 状态。", "mqtt");
      return;
    }
    try {
      commandInFlightRef.current = true;
      const response = await transport.sendCommand<MQTTConfig>("mqtt_publish_state");
      setMqttConfig(response.payload);
      appendLog("info", "设备状态已发布到 MQTT。", "mqtt");
    } catch (error) {
      appendLog("error", toErrorMessage(error), "mqtt");
    } finally {
      commandInFlightRef.current = false;
    }
  };

  const scanWifi = async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接 Mock 或 USB 设备，再扫描 Wi-Fi。", "network");
      return;
    }
    if (networkBusy || commandInFlightRef.current) {
      appendLog("warn", "设备正在连接 Wi-Fi，请等待完成后再扫描。", "network");
      return;
    }
    setWifiScanState("scanning");
    try {
      commandInFlightRef.current = true;
      const response = await transport.sendCommand<WifiNetwork[]>("scan_wifi");
      setWifiNetworks(response.payload);
      setWifiScanState("done");
      appendLog("info", `扫描完成，发现 ${response.payload.length} 个 Wi-Fi。`, "network");
    } catch (error) {
      setWifiNetworks([]);
      setWifiScanState("error");
      appendLog("error", String(error), "network");
    } finally {
      commandInFlightRef.current = false;
    }
  };

  const saveAiConfig = async (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    const transport = transportRef.current;
    const current = aiConfig ?? defaultAiConfig;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接 Mock 或 USB 设备，再保存 AI API 配置。", "ai");
      return;
    }

    const systemPrompt = current.systemPrompt.trim();
    const systemPromptBytes = utf8ByteLength(systemPrompt);
    if (systemPromptBytes > AI_SYSTEM_PROMPT_MAX_BYTES) {
      appendLog("warn", `系统提示词太长：${systemPromptBytes}/${AI_SYSTEM_PROMPT_MAX_BYTES} UTF-8 字节，请精简后再保存。`, "ai");
      return;
    }

    try {
      commandInFlightRef.current = true;
      const response = await transport.sendCommand<AIConfig>("set_ai_config", {
        profileId: current.profileId ?? 0,
        profileName: (current.profileName || "默认配置").trim(),
        apiBaseUrl: current.apiBaseUrl.trim(),
        apiKey: aiKeyInput.trim(),
        model: current.model.trim(),
        systemPrompt,
        timeoutMs: current.timeoutMs,
      });
      aiConfigDirtyRef.current = false;
      setAiConfig(response.payload);
      const profiles = await transport.sendCommand<AIProfilesResponse>("get_ai_profiles").catch(() => null);
      if (profiles) {
        setAiProfiles(profiles.payload.profiles);
      }
      setAiKeyInput("");
      appendLog("info", "AI API 配置已保存，API Key 不会从设备回显。", "ai");
    } catch (error) {
      appendLog("error", String(error), "ai");
    } finally {
      commandInFlightRef.current = false;
    }
  };

  const clearAiKey = async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接 Mock 或 USB 设备，再清除 API Key。", "ai");
      return;
    }

    try {
      commandInFlightRef.current = true;
      const response = await transport.sendCommand<AIConfig>("clear_ai_key");
      aiConfigDirtyRef.current = false;
      setAiConfig(response.payload);
      setAiProfiles((current) => current.map((item) => (item.profileId === response.payload.profileId ? response.payload : item)));
      setAiKeyInput("");
      appendLog("warn", "设备本地 API Key 已清除。", "ai");
    } catch (error) {
      appendLog("error", String(error), "ai");
    } finally {
      commandInFlightRef.current = false;
    }
  };

  const saveAsrConfig = async () => {
    const transport = transportRef.current;
    const current = asrConfig ?? defaultAsrConfig;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接设备，再保存 ASR 配置。", "asr");
      return;
    }
    try {
      commandInFlightRef.current = true;
      const response = await transport.sendCommand<ASRConfig>("set_asr_config", {
        apiBaseUrl: current.apiBaseUrl.trim(),
        apiKey: asrKeyInput.trim(),
        model: current.model.trim(),
        timeoutMs: current.timeoutMs,
      });
      asrConfigDirtyRef.current = false;
      setAsrConfig(response.payload);
      setAsrKeyInput("");
      appendLog("info", "ASR 配置已保存，Key 不会从设备回显。", "asr");
    } catch (error) {
      appendLog("error", String(error), "asr");
    } finally {
      commandInFlightRef.current = false;
    }
  };

  const clearAsrKey = async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接设备，再清除 ASR Key。", "asr");
      return;
    }
    try {
      commandInFlightRef.current = true;
      const response = await transport.sendCommand<ASRConfig>("clear_asr_key");
      asrConfigDirtyRef.current = false;
      setAsrConfig(response.payload);
      setAsrKeyInput("");
      appendLog("warn", "ASR Key 已清除。", "asr");
    } catch (error) {
      appendLog("error", String(error), "asr");
    } finally {
      commandInFlightRef.current = false;
    }
  };

  const saveTtsConfig = async () => {
    const transport = transportRef.current;
    const current = ttsConfig ?? defaultTtsConfig;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接设备，再保存 TTS 配置。", "tts");
      return;
    }
    try {
      commandInFlightRef.current = true;
      const response = await transport.sendCommand<TTSConfig>("set_tts_config", {
        apiBaseUrl: current.apiBaseUrl.trim(),
        apiKey: ttsKeyInput.trim(),
        model: current.model.trim(),
        voice: current.voice.trim(),
        timeoutMs: current.timeoutMs,
      });
      ttsConfigDirtyRef.current = false;
      setTtsConfig(response.payload);
      setTtsKeyInput("");
      appendLog("info", "TTS 配置已保存，Key 不会从设备回显。", "tts");
    } catch (error) {
      appendLog("error", String(error), "tts");
    } finally {
      commandInFlightRef.current = false;
    }
  };

  const clearTtsKey = async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接设备，再清除 TTS Key。", "tts");
      return;
    }
    try {
      commandInFlightRef.current = true;
      const response = await transport.sendCommand<TTSConfig>("clear_tts_key");
      ttsConfigDirtyRef.current = false;
      setTtsConfig(response.payload);
      setTtsKeyInput("");
      appendLog("warn", "TTS Key 已清除。", "tts");
    } catch (error) {
      appendLog("error", String(error), "tts");
    } finally {
      commandInFlightRef.current = false;
    }
  };

  const saveStateMachineConfig = async () => {
    const transport = transportRef.current;
    const current = stateMachineConfig;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接设备，再保存状态机配置。", "state");
      return;
    }
    if (!current) {
      appendLog("warn", "状态机配置尚未读取。", "state");
      return;
    }
    try {
      commandInFlightRef.current = true;
      const response = await transport.sendCommand<StateMachineConfig>("set_state_machine_config", current);
      stateMachineConfigDirtyRef.current = false;
      setStateMachineConfig(response.payload);
      appendLog("info", "状态机阈值已保存到设备。", "state");
      const statusResponse = await transport.sendCommand<StateMachineStatus>("get_state_machine_status").catch(() => null);
      if (statusResponse) {
        setStateMachineStatus(statusResponse.payload);
      }
    } catch (error) {
      appendLog("error", String(error), "state");
    } finally {
      commandInFlightRef.current = false;
    }
  };

  const refreshAiProfiles = useCallback(async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      return;
    }
    const response = await transport.sendCommand<AIProfilesResponse>("get_ai_profiles");
    setAiProfiles(response.payload.profiles);
  }, [connection]);

  const createAiProfile = async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接 Mock 或 USB 设备，再添加 AI API 配置。", "ai");
      return;
    }
    try {
      commandInFlightRef.current = true;
      const profileName = `备用配置 ${Math.min(aiProfiles.length + 1, 5)}`;
      const response = await transport.sendCommand<AIConfig>("create_ai_profile", { profileName });
      aiConfigDirtyRef.current = false;
      setAiConfig(response.payload);
      setAiKeyInput("");
      await refreshAiProfiles();
      appendLog("info", `已添加 AI API 配置：${response.payload.profileName ?? profileName}`, "ai");
    } catch (error) {
      appendLog("error", String(error), "ai");
    } finally {
      commandInFlightRef.current = false;
    }
  };

  const selectAiProfile = async (profileId: number) => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接 Mock 或 USB 设备，再切换 AI API 配置。", "ai");
      return;
    }
    try {
      commandInFlightRef.current = true;
      const response = await transport.sendCommand<AIConfig>("select_ai_profile", { profileId });
      aiConfigDirtyRef.current = false;
      setAiConfig(response.payload);
      setAiKeyInput("");
      await refreshAiProfiles();
      appendLog("info", `已切换 AI API 配置：${response.payload.profileName ?? profileId}`, "ai");
    } catch (error) {
      appendLog("error", String(error), "ai");
    } finally {
      commandInFlightRef.current = false;
    }
  };

  const deleteAiProfile = async (profileId: number) => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接 Mock 或 USB 设备，再删除 AI API 配置。", "ai");
      return;
    }
    try {
      commandInFlightRef.current = true;
      const response = await transport.sendCommand<AIConfig>("delete_ai_profile", { profileId });
      aiConfigDirtyRef.current = false;
      setAiConfig(response.payload);
      setAiKeyInput("");
      await refreshAiProfiles();
      appendLog("warn", "已删除备用 AI API 配置。", "ai");
    } catch (error) {
      appendLog("error", String(error), "ai");
    } finally {
      commandInFlightRef.current = false;
    }
  };

  const sendAiChat = async (event?: FormEvent<HTMLFormElement>) => {
    event?.preventDefault();
    const transport = transportRef.current;
    const message = aiChatDraft.trim().slice(0, 512);
    if (!message) {
      return;
    }
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接 Mock 或 USB 设备，再发送 AI 对话。", "ai");
      return;
    }

    const userMessage: AIChatMessage = {
      id: crypto.randomUUID(),
      role: "user",
      content: message,
      at: nowTime(),
      status: "ok",
    };
    const pendingId = crypto.randomUUID();
    const pendingMessage: AIChatMessage = {
      id: pendingId,
      role: "assistant",
      content: "正在整理上下文并请求 AI API...",
      at: nowTime(),
      status: "pending",
    };

    setAiMessages((current) => [...current, userMessage, pendingMessage].slice(-18));
    setAiChatDraft("");
    setAiBusy(true);
    try {
      commandInFlightRef.current = true;
      const request = {
        ...projectAiRequest,
        message,
        userText: message,
      };
      const preview = await transport.sendCommand<AIContextPreview>("get_ai_context_preview", request);
      setAiContextPreview(preview.payload);
      const response = await transport.sendCommand<AIAssistantChatResponse>("ai_assistant_chat", request);
      const historyResponse = await transport.sendCommand<DeviceChatHistory>("get_chat_history").catch(() => null);
      if (historyResponse) {
        setDeviceChatHistory(historyResponse.payload);
        setAiMessages(deviceHistoryToChatMessages(historyResponse.payload));
      }
      if (!historyResponse) {
        setAiMessages((current) =>
          current.map((item) =>
            item.id === pendingId
              ? {
                  ...item,
                  content: response.payload.reply,
                  status: "ok",
                  latencyMs: response.payload.latencyMs,
                }
              : item,
          ),
        );
      }
      appendLog(
        "info",
        `AI 助手已完成 ${projectAiTaskLabels[response.payload.taskType]}，模型 ${response.payload.model}，耗时 ${response.payload.latencyMs} ms，注入历史 ${response.payload.historyCount} 条。`,
        "ai",
      );
    } catch (error) {
      setAiMessages((current) =>
        current.map((item) =>
          item.id === pendingId
            ? {
                ...item,
                content: String(error),
                status: "error",
              }
            : item,
        ),
      );
      appendLog("error", String(error), "ai");
    } finally {
      commandInFlightRef.current = false;
      setAiBusy(false);
    }
  };
  const refreshDeviceChatHistory = useCallback(async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接 Mock 或 USB 设备，再读取设备会话历史。", "ai_context");
      return;
    }
    try {
      commandInFlightRef.current = true;
      const response = await transport.sendCommand<DeviceChatHistory>("get_chat_history");
      setDeviceChatHistory(response.payload);
      setAiMessages(deviceHistoryToChatMessages(response.payload));
      appendLog("info", `已读取设备会话历史：${response.payload.count} 条。`, "ai_context");
    } catch (error) {
      appendLog("error", String(error), "ai_context");
    } finally {
      commandInFlightRef.current = false;
    }
  }, [appendLog, connection]);

  const clearDeviceChatHistory = async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接 Mock 或 USB 设备，再清空设备会话历史。", "ai_context");
      return;
    }
    try {
      commandInFlightRef.current = true;
      const response = await transport.sendCommand<DeviceChatHistory>("clear_chat_history");
      setDeviceChatHistory(response.payload);
      setAiMessages([]);
      appendLog("warn", "设备会话历史已清空，对话窗口也已同步清空。", "ai_context");
    } catch (error) {
      appendLog("error", String(error), "ai_context");
    } finally {
      commandInFlightRef.current = false;
    }
  };

  const toggleVoiceChat = async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接设备，再使用语音对话。", "asr");
      return;
    }
    if (voiceBusy || aiBusy || commandInFlightRef.current) {
      appendLog("warn", "设备正在处理上一条命令，请稍后再试。", "asr");
      return;
    }
    if (wakeStatus?.enabled) {
      appendLog("warn", "WakeNet 正在占用麦克风，请先停止唤醒词监听。", "asr");
      return;
    }

    const recording = voiceStatus?.state === "recording";
    if (recording && micTestMode !== "asr") {
      appendLog("warn", "当前正在进行麦克风硬件测试，请先到“麦克风测试”页停止硬件录音。", "asr");
      return;
    }
    try {
      commandInFlightRef.current = true;
      setVoiceBusy(true);
      if (!recording) {
        setMicTestMode("asr");
        setMicTranscript("");
        setMicAiReply("");
        setMicAsrResult(null);
        const response = await transport.sendCommand<VoiceChatStatus>("voice_chat_start");
        setVoiceStatus(response.payload);
        pushMicSample(response.payload);
        appendLog("info", "语音录音已开始。", "asr");
        return;
      }

      const pendingId = crypto.randomUUID();
      const pendingMessage: AIChatMessage = {
        id: pendingId,
        role: "assistant",
        content: "正在转文字并请求 AI...",
        at: nowTime(),
        status: "pending",
      };
      setAiMessages((current) => [...current, pendingMessage].slice(-18));
      const response = await transport.sendCommand<VoiceChatResponse>("voice_chat_stop");
      setVoiceStatus({
        state: "ready",
        durationMs: 0,
        pcmBytes: response.payload.audioBytes,
        rms: 0,
        sampleCount: Math.max(0, Math.floor((response.payload.audioBytes - 44) / 2)),
        peakAbs: 0,
        minSample: 0,
        maxSample: 0,
        meanSample: 0,
        clipCount: 0,
        timeoutCount: 0,
        qualityHint: "ok",
        error: "",
      });
      setMicTestMode("idle");
      setMicTranscript(response.payload.transcript);
      setMicAiReply(response.payload.reply);
      setMicAsrResult(response.payload);
      const voiceUserMessage: AIChatMessage = {
        id: crypto.randomUUID(),
        role: "user",
        content: response.payload.transcript,
        at: nowTime(),
        status: "ok",
      };
      const voiceAssistantMessage: AIChatMessage = {
        id: crypto.randomUUID(),
        role: "assistant",
        content: response.payload.reply,
        at: nowTime(),
        status: "ok",
        latencyMs: response.payload.asrLatencyMs + response.payload.aiLatencyMs,
      };
      setAiMessages((current) => [
        ...current.filter((item) => item.id !== pendingId),
        voiceUserMessage,
        voiceAssistantMessage,
      ].slice(-18));
      const historyResponse = await transport.sendCommand<DeviceChatHistory>("get_chat_history").catch(() => null);
      if (historyResponse) {
        setDeviceChatHistory(historyResponse.payload);
      }
      appendLog("info", `语音对话完成：ASR ${response.payload.asrLatencyMs} ms / AI ${response.payload.aiLatencyMs} ms。`, "asr");
    } catch (error) {
      setAiMessages((current) => current.map((item) => item.status === "pending" ? { ...item, content: String(error), status: "error" } : item));
      appendLog("error", String(error), "asr");
    } finally {
      commandInFlightRef.current = false;
      setVoiceBusy(false);
    }
  };

  const startMicHardwareTest = async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接设备，再测试麦克风。", "asr");
      return;
    }
    if (voiceBusy || aiBusy || commandInFlightRef.current || voiceStatus?.state === "recording") {
      appendLog("warn", "录音或设备命令正在进行，请稍后再试。", "asr");
      return;
    }
    if (wakeStatus?.enabled) {
      appendLog("warn", "WakeNet 正在占用麦克风，请先停止唤醒词监听。", "asr");
      return;
    }
    try {
      commandInFlightRef.current = true;
      setVoiceBusy(true);
      setMicTestMode("hardware");
      setMicSamples([]);
      setMicTranscript("");
      setMicAiReply("");
      setMicAsrResult(null);
      clearMicPlayback();
      const response = await transport.sendCommand<VoiceChatStatus>("mic_record_start");
      setVoiceStatus(response.payload);
      pushMicSample(response.payload);
      appendLog("info", "麦克风硬件录音测试已开始。", "asr");
    } catch (error) {
      setMicTestMode("idle");
      appendLog("error", String(error), "asr");
    } finally {
      commandInFlightRef.current = false;
      setVoiceBusy(false);
    }
  };

  const refreshMicHardwareStatus = async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      return;
    }
    if (commandInFlightRef.current) {
      return;
    }
    try {
      commandInFlightRef.current = true;
      const response = await transport.sendCommand<VoiceChatStatus>("mic_record_status");
      setVoiceStatus(response.payload);
      pushMicSample(response.payload);
    } catch (error) {
      appendLog("error", String(error), "asr");
    } finally {
      commandInFlightRef.current = false;
    }
  };

  const stopMicHardwareTest = async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接设备，再停止麦克风测试。", "asr");
      return;
    }
    if (voiceBusy || aiBusy || commandInFlightRef.current) {
      appendLog("warn", "设备正在处理上一条命令，请稍后再试。", "asr");
      return;
    }
    try {
      commandInFlightRef.current = true;
      setVoiceBusy(true);
      const response = await transport.sendCommand<VoiceChatStatus>("mic_record_stop");
      setVoiceStatus(response.payload);
      pushMicSample(response.payload);
      setMicTestMode("idle");
      try {
        const wavResponse = await transport.sendCommand<MicRecordWavResponse>("mic_record_wav");
        const blob = base64ToBlob(wavResponse.payload.wavBase64, "audio/wav");
        const url = URL.createObjectURL(blob);
        clearMicPlayback();
        micPlaybackUrlRef.current = url;
        setMicPlayback({
          url,
          durationMs: wavResponse.payload.durationMs,
          pcmBytes: wavResponse.payload.pcmBytes,
          wavBytes: wavResponse.payload.wavBytes,
          sampleRate: wavResponse.payload.sampleRate,
          channels: wavResponse.payload.channels,
          bitsPerSample: wavResponse.payload.bitsPerSample,
          createdAt: nowTime(),
        });
        appendLog("info", `麦克风录音已取回，可在面板播放：${wavResponse.payload.wavBytes} B WAV。`, "asr");
      } catch (wavError) {
        appendLog("warn", `录音已停止，但 WAV 取回失败：${toErrorMessage(wavError)}`, "asr");
      }
      appendLog("info", "麦克风硬件录音测试已停止，未触发 ASR/AI。", "asr");
    } catch (error) {
      appendLog("error", String(error), "asr");
    } finally {
      commandInFlightRef.current = false;
      setVoiceBusy(false);
    }
  };

  const playDeviceTts = async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接设备，再测试扬声器。", "tts");
      return;
    }
    if (ttsBusy || commandInFlightRef.current) {
      appendLog("warn", "设备正在处理上一条命令，请稍后再试。", "tts");
      return;
    }
    if (!ttsText.trim()) {
      appendLog("warn", "请输入要合成播放的测试文本。", "tts");
      return;
    }
    try {
      commandInFlightRef.current = true;
      setTtsBusy(true);
      const response = await transport.sendCommand<TTSStatus>("tts_play", { text: ttsText.trim() });
      setTtsStatus(response.payload);
      setWakeStatus((current) => current ? { ...current, enabled: false, state: "idle" } : current);
      appendLog("info", `设备 TTS 已启动：HTTP ${response.payload.httpStatus}，音频 ${response.payload.audioBytes} B。`, "tts");
    } catch (error) {
      appendLog("error", String(error), "tts");
    } finally {
      commandInFlightRef.current = false;
      setTtsBusy(false);
    }
  };

  const refreshTtsStatus = useCallback(async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected" || commandInFlightRef.current) {
      return;
    }
    try {
      commandInFlightRef.current = true;
      const response = await transport.sendCommand<TTSStatus>("tts_status");
      setTtsStatus(response.payload);
    } catch (error) {
      appendLog("warn", `扬声器状态读取失败：${toErrorMessage(error)}`, "tts");
    } finally {
      commandInFlightRef.current = false;
    }
  }, [appendLog, connection]);

  const stopDeviceTts = async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接设备，再停止扬声器播放。", "tts");
      return;
    }
    try {
      commandInFlightRef.current = true;
      setTtsBusy(true);
      const response = await transport.sendCommand<TTSStatus>("tts_stop");
      setTtsStatus(response.payload);
      appendLog("info", "设备扬声器播放已停止。", "tts");
    } catch (error) {
      appendLog("error", String(error), "tts");
    } finally {
      commandInFlightRef.current = false;
      setTtsBusy(false);
    }
  };

  const playBrowserTts = async () => {
    const current = ttsConfig ?? defaultTtsConfig;
    const apiKey = ttsKeyInput.trim();
    if (!apiKey) {
      appendLog("warn", "浏览器试听需要在本次页面输入 TTS API Key；设备保存的 Key 不会回显给浏览器。", "tts");
      return;
    }
    if (!ttsText.trim()) {
      appendLog("warn", "请输入要合成试听的文本。", "tts");
      return;
    }
    try {
      setBrowserTtsBusy(true);
      const response = await fetch(current.apiBaseUrl.trim(), {
        method: "POST",
        headers: {
          Authorization: `Bearer ${apiKey}`,
          "Content-Type": "application/json",
          Accept: "audio/mpeg",
        },
        body: JSON.stringify({
          model: current.model.trim(),
          voice: normalizeTtsVoiceForRequest(current.model, current.voice),
          input: ttsText.trim(),
          response_format: "mp3",
        }),
      });
      if (!response.ok) {
        throw new Error(`浏览器 TTS HTTP ${response.status}`);
      }
      const blob = await response.blob();
      const url = URL.createObjectURL(blob);
      const audio = new Audio(url);
      audio.onended = () => URL.revokeObjectURL(url);
      await audio.play();
      appendLog("info", `浏览器 TTS 试听已播放，音频 ${blob.size} B。`, "tts");
    } catch (error) {
      appendLog("error", `${toErrorMessage(error)}。如果是 CORS 限制，请改用设备扬声器播放。`, "tts");
    } finally {
      setBrowserTtsBusy(false);
    }
  };

  const refreshCameraStatus = useCallback(async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      return;
    }
    if (commandInFlightRef.current) {
      return;
    }
    try {
      commandInFlightRef.current = true;
      const response = await transport.sendCommand<CameraStatus>("get_camera_status");
      setCameraStatus(response.payload);
    } catch (error) {
      appendLog("warn", `摄像头状态读取失败：${toErrorMessage(error)}`, "camera");
    } finally {
      commandInFlightRef.current = false;
    }
  }, [appendLog, connection]);

  const captureCameraFrame = useCallback(async (source: "manual" | "live" = "manual") => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接设备，再拍照。", "camera");
      return;
    }
    if (cameraBusy || commandInFlightRef.current) {
      appendLog("warn", "设备正在处理上一条命令，请稍后再试。", "camera");
      return;
    }
    try {
      commandInFlightRef.current = true;
      setCameraBusy(true);
      setCameraAnalyzeResult(null);
      const response = await transport.sendCommand<CameraCaptureResponse>("camera_capture");
      setCameraStatus(response.payload.status);
      setCameraPreview(response.payload.previewDataUrl);
      if (source === "live") {
        setCameraLiveFrames((current) => current + 1);
        setCameraLiveLastAt(nowTime());
        setCameraLiveError("");
      }
      if (response.payload.previewOmitted) {
        appendLog("warn", `JPEG ${response.payload.status.jpegBytes} B 超过串口预览上限 ${response.payload.previewMaxBytes} B，已省略预览。`, "camera");
      } else {
        appendLog("info", `${source === "live" ? "实时预览刷新" : "拍照完成"}：${response.payload.status.width}x${response.payload.status.height} / ${response.payload.status.jpegBytes} B，稳定路径为 YUV422 采集后软件 JPEG。`, "camera");
      }
    } catch (error) {
      const message = toErrorMessage(error);
      setCameraLiveError(message);
      if (source === "live") {
        setCameraLive(false);
      }
      appendLog("error", message, "camera");
    } finally {
      commandInFlightRef.current = false;
      setCameraBusy(false);
    }
  }, [appendLog, cameraBusy, connection]);

  const probeCamera = useCallback(async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接设备，再执行摄像头安全探测。", "camera");
      return;
    }
    if (cameraBusy || commandInFlightRef.current) {
      appendLog("warn", "设备正在处理上一条命令，请稍后再试。", "camera");
      return;
    }
    try {
      commandInFlightRef.current = true;
      setCameraBusy(true);
      const response = await transport.sendCommand<CameraProbeResponse>("camera_probe");
      setCameraProbe(response.payload);
      if (response.payload.ok) {
        appendLog("info", `OV3660 安全探测成功：PID=0x${response.payload.pid.toString(16).padStart(4, "0")}，${response.payload.durationMs} ms。`, "camera");
      } else {
        appendLog("warn", `OV3660 安全探测未通过：${response.payload.lastError || response.payload.espErrName}`, "camera");
      }
    } catch (error) {
      appendLog("error", `摄像头安全探测失败：${toErrorMessage(error)}`, "camera");
    } finally {
      commandInFlightRef.current = false;
      setCameraBusy(false);
    }
  }, [appendLog, cameraBusy, connection]);

  const toggleCameraLive = useCallback(() => {
    if (!cameraLive) {
      setCameraLiveFrames(0);
      setCameraLiveLastAt("");
      setCameraLiveError("");
      appendLog("info", "摄像头低频实时预览已启动：约每 2 秒抓拍一帧。", "camera");
      setCameraLive(true);
      return;
    }
    setCameraLive(false);
    appendLog("info", "摄像头实时预览已停止。", "camera");
  }, [appendLog, cameraLive]);

  const analyzeCameraFrame = async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接设备，再识别照片。", "camera");
      return;
    }
    if (cameraBusy || commandInFlightRef.current) {
      appendLog("warn", "设备正在处理上一条命令，请稍后再试。", "camera");
      return;
    }
    try {
      commandInFlightRef.current = true;
      setCameraBusy(true);
      appendLog("info", "开始拍摄 OV3660 XGA 高分辨率图片并提交 AI；该路径为当前实测稳定成品链路，不会通过串口返回大图预览。", "camera");
      const response = await transport.sendCommand<CameraAnalyzeResponse>("camera_analyze");
      setCameraAnalyzeResult(response.payload);
      appendLog("info", `摄像头 AI 识别完成：HTTP ${response.payload.httpStatus}，${response.payload.latencyMs} ms，提交图片 ${response.payload.width}x${response.payload.height} / ${response.payload.jpegBytes} B。`, "camera");
    } catch (error) {
      appendLog("error", String(error), "camera");
    } finally {
      commandInFlightRef.current = false;
      setCameraBusy(false);
    }
  };

  const runCameraRgb565Diag = async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接设备，再执行 RGB565 诊断。", "camera");
      return;
    }
    if (cameraBusy || commandInFlightRef.current) {
      appendLog("warn", "设备正在处理上一条命令，请稍后再试。", "camera");
      return;
    }
    try {
      commandInFlightRef.current = true;
      setCameraBusy(true);
      setCameraLive(false);
      setCameraPreview(null);
      const response = await transport.sendCommand<CameraRgb565DiagResponse>("camera_rgb565_diag");
      setCameraRgb565Diag(response.payload);
      await refreshCameraStatus();
      if (response.payload.ok) {
        appendLog("info", `RGB565 诊断成帧：${response.payload.width}x${response.payload.height} / ${response.payload.bytes} B / checksum=0x${response.payload.checksum.toString(16).padStart(8, "0")}`, "camera");
      } else {
        appendLog("warn", `RGB565 诊断失败：${response.payload.lastError || response.payload.espErrName}`, "camera");
      }
    } catch (error) {
      appendLog("error", `RGB565 诊断失败：${toErrorMessage(error)}`, "camera");
    } finally {
      commandInFlightRef.current = false;
      setCameraBusy(false);
    }
  };

  const runCameraJpegDiag = async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接设备，再执行硬件 JPEG 诊断。", "camera");
      return;
    }
    if (cameraBusy || commandInFlightRef.current) {
      appendLog("warn", "设备正在处理上一条命令，请稍后再试。", "camera");
      return;
    }
    try {
      commandInFlightRef.current = true;
      setCameraBusy(true);
      setCameraLive(false);
      setCameraAnalyzeResult(null);
      const response = await transport.sendCommand<CameraCaptureResponse>("camera_jpeg_diag");
      setCameraStatus(response.payload.status);
      setCameraPreview(response.payload.previewDataUrl);
      if (response.payload.previewOmitted) {
        appendLog("warn", `硬件 JPEG ${response.payload.status.jpegBytes} B 超过串口预览上限 ${response.payload.previewMaxBytes} B，已省略预览。`, "camera");
      } else {
        appendLog("info", `硬件 JPEG 码流诊断完成：${response.payload.status.width}x${response.payload.status.height} / ${response.payload.status.jpegBytes} B。注意：该片上 JPEG 路径当前仅看 SOI/码流稳定性，颜色可能不准。`, "camera");
      }
    } catch (error) {
      appendLog("error", `硬件 JPEG 诊断失败：${toErrorMessage(error)}`, "camera");
    } finally {
      commandInFlightRef.current = false;
      setCameraBusy(false);
    }
  };

  const resetCameraDevice = async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接设备，再重置摄像头驱动。", "camera");
      return;
    }
    if (cameraBusy || commandInFlightRef.current) {
      appendLog("warn", "设备正在处理上一条命令，请稍后再试。", "camera");
      return;
    }
    try {
      commandInFlightRef.current = true;
      setCameraBusy(true);
      setCameraLive(false);
      const response = await transport.sendCommand<CameraStatus>("camera_reset");
      setCameraStatus(response.payload);
      setCameraPreview(null);
      setCameraAnalyzeResult(null);
      appendLog("info", "摄像头驱动已反初始化，可重新安全探测或切换诊断模式。", "camera");
    } catch (error) {
      appendLog("error", `摄像头重置失败：${toErrorMessage(error)}`, "camera");
    } finally {
      commandInFlightRef.current = false;
      setCameraBusy(false);
    }
  };

  const clearCameraFrame = async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接设备，再清除照片缓存。", "camera");
      return;
    }
    if (cameraBusy || commandInFlightRef.current) {
      appendLog("warn", "设备正在处理上一条命令，请稍后再试。", "camera");
      return;
    }
    try {
      commandInFlightRef.current = true;
      setCameraBusy(true);
      const response = await transport.sendCommand<CameraStatus>("clear_camera_frame");
      setCameraStatus(response.payload);
      setCameraPreview(null);
      setCameraAnalyzeResult(null);
      setCameraLive(false);
      setCameraLiveFrames(0);
      setCameraLiveLastAt("");
      setCameraLiveError("");
      appendLog("info", "最近一张照片已从设备 RAM/PSRAM 清除。", "camera");
    } catch (error) {
      appendLog("error", String(error), "camera");
    } finally {
      commandInFlightRef.current = false;
      setCameraBusy(false);
    }
  };

  const previewProjectAiContext = async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接 Mock 或 USB 设备，再预览项目 AI 上下文。", "ai_context");
      return;
    }
    try {
      commandInFlightRef.current = true;
      setProjectAiBusy(true);
      const response = await transport.sendCommand<AIContextPreview>("get_ai_context_preview", projectAiRequest);
      setAiContextPreview(response.payload);
      appendLog("info", `已生成 ${projectAiTaskLabels[response.payload.taskType]} 上下文预览。`, "ai_context");
    } catch (error) {
      appendLog("error", String(error), "ai_context");
    } finally {
      commandInFlightRef.current = false;
      setProjectAiBusy(false);
    }
  };

  const runProjectAiTask = async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接 Mock 或 USB 设备，再测试项目 AI 任务。", "ai_context");
      return;
    }
    try {
      commandInFlightRef.current = true;
      setProjectAiBusy(true);
      const preview = await transport.sendCommand<AIContextPreview>("get_ai_context_preview", projectAiRequest);
      const result = await transport.sendCommand<ProjectAITaskResponse>("test_ai_task", projectAiRequest);
      setAiContextPreview(preview.payload);
      appendLog("info", `项目 AI Mock 任务完成：${projectAiTaskLabels[result.payload.taskType]}。`, "ai_context");
    } catch (error) {
      appendLog("error", String(error), "ai_context");
    } finally {
      commandInFlightRef.current = false;
      setProjectAiBusy(false);
    }
  };

  const refreshMemorySummary = async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接 Mock 或 USB 设备，再读取记忆摘要。", "ai_context");
      return;
    }
    try {
      commandInFlightRef.current = true;
      const response = await transport.sendCommand<MemorySummary>("get_memory_summary");
      setMemorySummary(response.payload);
      setMemoryDraft(JSON.stringify(response.payload, null, 2));
      appendLog("info", "已读取结构化记忆摘要。", "ai_context");
    } catch (error) {
      appendLog("error", String(error), "ai_context");
    } finally {
      commandInFlightRef.current = false;
    }
  };

  const clearMemorySummary = async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接 Mock 或 USB 设备，再清空记忆摘要。", "ai_context");
      return;
    }
    try {
      commandInFlightRef.current = true;
      const response = await transport.sendCommand<MemorySummary>("clear_memory_summary");
      setMemorySummary(response.payload);
      setMemoryDraft(JSON.stringify(response.payload, null, 2));
      appendLog("warn", "已清空结构化记忆摘要，库存和提醒不受影响。", "ai_context");
    } catch (error) {
      appendLog("error", String(error), "ai_context");
    } finally {
      commandInFlightRef.current = false;
    }
  };

  const saveMemorySummary = async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接 Mock 或 USB 设备，再写入硬件测试记忆。", "ai_context");
      return;
    }
    let parsed: MemorySummary;
    try {
      parsed = JSON.parse(memoryDraft) as MemorySummary;
    } catch (error) {
      appendLog("error", `测试记忆不是合法 JSON：${toErrorMessage(error)}`, "ai_context");
      return;
    }
    try {
      commandInFlightRef.current = true;
      const response = await transport.sendCommand<MemorySummary>("set_memory_summary", { memory: parsed });
      setMemorySummary(response.payload);
      setMemoryDraft(JSON.stringify(response.payload, null, 2));
      appendLog("info", "硬件测试记忆已写入，后续 AI 助手可选择注入。", "ai_context");
    } catch (error) {
      appendLog("error", String(error), "ai_context");
    } finally {
      commandInFlightRef.current = false;
    }
  };

  const clearAiMessages = () => {
    setAiMessages([]);
    appendLog("info", "AI 助手对话已在浏览器中清空；硬件测试记忆不受影响。", "ai");
  };

  const exportLogs = () => {
    const body = filteredLogs
      .map((log) => `[${log.at}] ${log.level.toUpperCase()} ${log.source}: ${log.message}`)
      .join("\n");
    const blob = new Blob([body], { type: "text/plain;charset=utf-8" });
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement("a");
    anchor.href = url;
    anchor.download = "fridge-spirit-usb-logs.txt";
    anchor.click();
    URL.revokeObjectURL(url);
  };

  const filteredLogs = useMemo(() => {
    return logs.filter((log) => {
      const levelMatch = logFilter === "all" || log.level === logFilter;
      const text = `${log.source} ${log.message}`.toLowerCase();
      return levelMatch && text.includes(searchTerm.trim().toLowerCase());
    });
  }, [logFilter, logs, searchTerm]);

  useEffect(() => {
    if (connection === "connected") {
      refreshAll({ full: true });
    }
  }, [connection, refreshAll]);

  useEffect(() => {
    if (connection === "connected" && activeSection === "ai") {
      refreshAiProfiles().catch((error) => appendLog("warn", `AI API 配置列表读取失败：${toErrorMessage(error)}`, "web"));
      refreshDeviceChatHistory().catch((error) => appendLog("warn", `设备会话历史读取失败：${toErrorMessage(error)}`, "web"));
    }
  }, [activeSection, appendLog, connection, refreshAiProfiles, refreshDeviceChatHistory]);

  useEffect(() => {
    if (connection === "connected" && activeSection === "camera") {
      void refreshCameraStatus();
    }
  }, [activeSection, connection, refreshCameraStatus]);

  useEffect(() => {
    if (connection !== "connected" || activeSection !== "camera" || !cameraLive) {
      return undefined;
    }
    void captureCameraFrame("live");
    const timer = window.setInterval(() => {
      if (document.visibilityState === "visible" && !cameraBusy && !commandInFlightRef.current) {
        void captureCameraFrame("live");
      }
    }, 2000);
    return () => window.clearInterval(timer);
  }, [activeSection, cameraBusy, cameraLive, captureCameraFrame, connection]);

  useEffect(() => {
    if (connection !== "connected") {
      setCameraLive(false);
    }
  }, [connection]);

  useEffect(() => {
    if (connection !== "connected" || activeSection !== "mic" || micTestMode !== "hardware" || voiceStatus?.state !== "recording") {
      return undefined;
    }
    const timer = window.setInterval(() => {
      void refreshMicHardwareStatus();
    }, 500);
    return () => window.clearInterval(timer);
  }, [activeSection, connection, micTestMode, voiceStatus?.state]);

  useEffect(() => {
    if (connection !== "connected" || activeSection !== "mic") {
      return undefined;
    }
    void refreshWakeStatus();
    if (!wakeStatus?.enabled) {
      return undefined;
    }
    const timer = window.setInterval(() => {
      if (document.visibilityState === "visible") {
        void refreshWakeStatus();
      }
    }, 1000);
    return () => window.clearInterval(timer);
  }, [activeSection, connection, refreshWakeStatus, wakeStatus?.enabled]);

  useEffect(() => {
    if (connection !== "connected" || activeSection !== "speaker") {
      return undefined;
    }
    void refreshTtsStatus();
    const timer = window.setInterval(() => {
      if (document.visibilityState === "visible" && (ttsStatus?.state === "playing" || ttsStatus?.state === "synthesizing")) {
        void refreshTtsStatus();
      }
    }, 500);
    return () => window.clearInterval(timer);
  }, [activeSection, connection, refreshTtsStatus, ttsStatus?.state]);

  useEffect(() => {
    if (connection !== "connected" || activeSection !== "sensors") {
      return undefined;
    }
    void refreshSensorsOnly();
    const timer = window.setInterval(() => {
      if (document.visibilityState === "visible") {
        void refreshSensorsOnly();
      }
    }, 300);
    return () => window.clearInterval(timer);
  }, [activeSection, connection, refreshSensorsOnly]);

  useEffect(() => {
    if (connection !== "connected" || activeSection !== "radar") {
      return undefined;
    }
    void refreshRadarStatus();
    const radarPollingActive = radarRunning || radarStatus?.mode === "report";
    if (!radarPollingActive) {
      return undefined;
    }
    const timer = window.setInterval(() => {
      if (document.visibilityState === "visible") {
        void refreshRadarStatus();
      }
    }, 250);
    return () => window.clearInterval(timer);
  }, [activeSection, connection, radarRunning, radarStatus?.mode, refreshRadarStatus]);

  useEffect(() => {
    if (connection !== "connected") {
      return undefined;
    }
    const timer = window.setInterval(() => {
      if (document.visibilityState === "visible") {
        refreshAll({ full: false });
      }
    }, refreshSeconds * 1000);
    return () => window.clearInterval(timer);
  }, [connection, refreshAll, refreshSeconds]);

  const healthRows = [
    ["Wi-Fi", status?.wifi ?? "offline"],
    ["MQTT", status?.mqtt ?? "offline"],
    ["USB", status?.usb ?? "offline"],
    ["OTA", status?.ota ?? "offline"],
  ] as const;

  return (
    <div className="app-shell">
      <aside className="sidebar" aria-label="主导航">
        <div className="brand-block">
          <div className="brand-mark">冰</div>
          <div>
            <strong>冰箱小精灵</strong>
            <span>USB 运维面板</span>
          </div>
        </div>

        <nav className="nav-list">
          {sections.map((section) => {
            const Icon = section.icon;
            return (
              <button
                key={section.id}
                className={activeSection === section.id ? "nav-item active" : "nav-item"}
                onClick={() => setActiveSection(section.id)}
                type="button"
                title={section.label}
              >
                <Icon size={18} />
                <span>{section.label}</span>
              </button>
            );
          })}
        </nav>

        <div className="sidebar-mode" aria-label="连接模式">
          <span>连接模式</span>
          <div className="sidebar-mode-switch">
            <button
              className={transportMode === "mock" ? "active" : ""}
              onClick={() => void changeTransportMode("mock")}
              type="button"
              title="切换到 Mock 数据，切换时清空当前显示数据"
            >
              Mock
            </button>
            <button
              className={transportMode === "serial" ? "active" : ""}
              onClick={() => void changeTransportMode("serial")}
              type="button"
              title="切换到 Web Serial，切换时清空当前显示数据"
            >
              USB
            </button>
          </div>
          <small>{transportMode === "mock" ? "模拟数据，不需要硬件" : "真实串口，需要 Chrome/Edge"}</small>
        </div>

        <div className="safety-note">
          <ShieldAlert size={18} />
          <span>面板不提供 GPIO 输出控制；所有引脚仅展示和诊断。</span>
        </div>
      </aside>

      <main className="workspace">
        <header className="topbar">
          <div>
            <p className="eyebrow">ESP32-S3 DevKitC-1 N8R8</p>
            <h1>{sections.find((item) => item.id === activeSection)?.label}</h1>
          </div>
          <div className="topbar-actions">
            <StatusPill state={connection === "connected" ? "ok" : connection === "connecting" ? "warn" : "offline"}>
              {connection === "connected" ? "已连接" : connection === "connecting" ? "连接中" : "未连接"}
            </StatusPill>
            <button className="icon-button" onClick={() => void refreshAll({ full: true })} disabled={connection !== "connected" || busy} title="刷新">
              <RotateCw size={18} className={busy ? "spin" : ""} />
            </button>
            {connection === "connected" ? (
              <button className="action-button danger" onClick={disconnect} type="button">
                <Unplug size={17} />
                断开
              </button>
            ) : (
              <button className="action-button" onClick={connect} type="button">
                <Power size={17} />
                连接
              </button>
            )}
          </div>
        </header>

        <section className="status-strip" aria-label="设备状态">
          <Metric label="固件" value={status?.firmware ?? "等待连接"} />
          <Metric label="Flash" value={status?.flash ?? "N8R8 规划"} />
          <Metric label="PSRAM" value={status?.psram ?? "8 MB 规划"} />
          <Metric label="运行时间" value={status?.uptime ?? "--"} />
          {healthRows.map(([label, state]) => (
            <div className="inline-health" key={label}>
              <span>{label}</span>
              <StatusDot state={state} />
            </div>
          ))}
        </section>

        {connectionNotice && (
          <div className="top-alert">
            <Info size={16} />
            <span>{connectionNotice}</span>
          </div>
        )}

        <div className="content-grid">
          <section className="primary-panel">{renderSection()}</section>
          <aside className="context-panel">
            <h2>硬件风险</h2>
            <p>{status?.powerNote ?? "连接设备后显示供电、GPIO、启动绑带脚与日志风险。"}</p>
            <div className="risk-list">
              {(diagnostics?.riskItems ?? [
                "屏幕 VCC 为 5V，GPIO 逻辑为 3.3V。",
                "GPIO35-37 在 N8R8 上禁用；GPIO0 不接；GPIO3/45/46/48 属于摄像头高风险脚。",
                "I2C 上拉必须到 3.3V。",
              ]).map((item) => (
                <div className="risk-row" key={item}>
                  <Info size={15} />
                  <span>{item}</span>
                </div>
              ))}
            </div>
          </aside>
        </div>
      </main>
    </div>
  );

  function renderSection() {
    switch (activeSection) {
      case "overview":
        return <Overview status={status} sensors={sensors} diagnostics={diagnostics} logs={logs} />;
      case "usb":
        return (
          <UsbPanel
            mode={transportMode}
            connection={connection}
            notice={connectionNotice}
            onConnect={connect}
            onDisconnect={disconnect}
          />
        );
      case "network":
        return (
          <NetworkPanel
            mode={transportMode}
            connection={connection}
            network={network}
            networkBusy={networkBusy}
            wifiNetworks={wifiNetworks}
            wifiScanState={wifiScanState}
            mqttConfig={mqttConfig}
            mqttTokenInput={mqttTokenInput}
            setMqttTokenInput={setMqttTokenInput}
            setNetwork={setNetworkDraft}
            setMqttConfig={setMqttConfig}
            saveNetwork={saveNetwork}
            saveMqttConfig={saveMqttConfig}
            publishMqttState={publishMqttState}
            scanWifi={scanWifi}
          />
        );
      case "ai":
        return (
          <AiPanel
            connection={connection}
            aiConfig={aiConfig}
            aiProfiles={aiProfiles}
            setAiConfig={setAiConfigDraft}
            apiKeyInput={aiKeyInput}
            setApiKeyInput={setAiKeyInput}
            asrConfig={asrConfig}
            setAsrConfig={setAsrConfigDraft}
            asrKeyInput={asrKeyInput}
            setAsrKeyInput={setAsrKeyInput}
            voiceStatus={voiceStatus}
            voiceBusy={voiceBusy}
            micTestMode={micTestMode}
            messages={aiMessages}
            draft={aiChatDraft}
            setDraft={setAiChatDraft}
            busy={aiBusy}
            projectRequest={projectAiRequest}
            setProjectRequest={setProjectAiRequest}
            contextPreview={aiContextPreview}
            projectBusy={projectAiBusy}
            deviceChatHistory={deviceChatHistory}
            memorySummary={memorySummary}
            memoryDraft={memoryDraft}
            setMemoryDraft={setMemoryDraft}
            saveAiConfig={saveAiConfig}
            clearAiKey={clearAiKey}
            saveAsrConfig={saveAsrConfig}
            clearAsrKey={clearAsrKey}
            createAiProfile={createAiProfile}
            selectAiProfile={selectAiProfile}
            deleteAiProfile={deleteAiProfile}
            sendAiChat={sendAiChat}
            toggleVoiceChat={toggleVoiceChat}
            clearMessages={clearAiMessages}
            previewContext={previewProjectAiContext}
            runMockTask={runProjectAiTask}
            refreshDeviceHistory={refreshDeviceChatHistory}
            clearDeviceHistory={clearDeviceChatHistory}
            refreshMemory={refreshMemorySummary}
            saveMemory={saveMemorySummary}
            clearMemory={clearMemorySummary}
          />
        );
      case "mic":
        return (
          <MicrophoneTestPanel
            connection={connection}
            asrConfig={asrConfig}
            voiceStatus={voiceStatus}
            voiceBusy={voiceBusy}
            micTestMode={micTestMode}
            micSamples={micSamples}
            micTranscript={micTranscript}
            micAiReply={micAiReply}
            micAsrResult={micAsrResult}
            wakeStatus={wakeStatus}
            wakeBusy={wakeBusy}
            wakeEvents={wakeEvents}
            micPlayback={micPlayback}
            startHardwareTest={startMicHardwareTest}
            stopHardwareTest={stopMicHardwareTest}
            refreshHardwareStatus={refreshMicHardwareStatus}
            toggleVoiceChat={toggleVoiceChat}
            startWakeListening={startWakeListening}
            stopWakeListening={stopWakeListening}
            refreshWakeStatus={refreshWakeStatus}
            resetWakeStats={resetWakeStats}
          />
        );
      case "speaker":
        return (
          <SpeakerTestPanel
            connection={connection}
            config={ttsConfig}
            setConfig={setTtsConfigDraft}
            keyInput={ttsKeyInput}
            setKeyInput={setTtsKeyInput}
            text={ttsText}
            setText={setTtsText}
            status={ttsStatus}
            busy={ttsBusy}
            browserBusy={browserTtsBusy}
            saveConfig={saveTtsConfig}
            clearKey={clearTtsKey}
            playDevice={playDeviceTts}
            stopDevice={stopDeviceTts}
            playBrowser={playBrowserTts}
            refreshStatus={refreshTtsStatus}
          />
        );
      case "radar":
        return (
          <RadarTestPanel
            connection={connection}
            status={radarStatus}
            samples={radarSamples}
            running={radarRunning}
            busy={radarBusy}
            startTest={startRadarTest}
            stopTest={stopRadarTest}
            refreshStatus={refreshRadarStatus}
          />
        );
      case "camera":
        return (
          <CameraTestPanel
            connection={connection}
            aiConfig={aiConfig}
            status={cameraStatus}
            probe={cameraProbe}
            rgb565Diag={cameraRgb565Diag}
            previewDataUrl={cameraPreview}
            analyzeResult={cameraAnalyzeResult}
            busy={cameraBusy}
            live={cameraLive}
            liveFrames={cameraLiveFrames}
            liveLastAt={cameraLiveLastAt}
            liveError={cameraLiveError}
            refreshStatus={refreshCameraStatus}
            probeCamera={probeCamera}
            captureFrame={captureCameraFrame}
            toggleLive={toggleCameraLive}
            analyzeFrame={analyzeCameraFrame}
            runJpegDiag={runCameraJpegDiag}
            runRgb565Diag={runCameraRgb565Diag}
            resetCamera={resetCameraDevice}
            clearFrame={clearCameraFrame}
          />
        );
      case "pins":
        return <PinsPanel pins={pins} />;
      case "sensors":
        return <SensorsPanel sensors={sensors} />;
      case "logs":
        return (
          <LogsPanel
            logs={filteredLogs}
            level={logFilter}
            setLevel={setLogFilter}
            searchTerm={searchTerm}
            setSearchTerm={setSearchTerm}
            clearLogs={() => setLogs([])}
            exportLogs={exportLogs}
          />
        );
      case "diagnostics":
        return <DiagnosticsPanel diagnostics={diagnostics} />;
      case "settings":
        return (
          <SettingsPanel
            timeoutMs={timeoutMs}
            setTimeoutMs={setTimeoutMs}
            refreshSeconds={refreshSeconds}
            setRefreshSeconds={setRefreshSeconds}
            stateMachineConfig={stateMachineConfig}
            setStateMachineConfig={setStateMachineConfigDraft}
            stateMachineStatus={stateMachineStatus ?? sensors?.stateMachine ?? null}
            saveStateMachineConfig={saveStateMachineConfig}
            connected={connection === "connected"}
          />
        );
    }
  }
}

function Overview({
  status,
  sensors,
  diagnostics,
  logs,
}: {
  status: DeviceStatus | null;
  sensors: SensorSnapshot | null;
  diagnostics: DiagnosticSnapshot | null;
  logs: DeviceLog[];
}) {
  const lightValue = sensors?.lightValue10bit ?? sensors?.lux;
  return (
    <div className="section-flow">
      <div className="section-heading">
        <div>
          <p className="eyebrow">运行视图</p>
          <h2>设备快照</h2>
        </div>
        <StatusPill state={status ? "ok" : "offline"}>{status ? status.page : "等待连接"}</StatusPill>
      </div>
      <div className="metric-grid">
        <Metric label="Free Heap" value={status ? `${status.freeHeapKb} KB` : "--"} />
        <Metric label="Min Heap" value={status ? `${status.minHeapKb} KB` : "--"} />
        <Metric label="Free PSRAM" value={status ? `${status.freePsramKb} KB` : "--"} />
        <Metric label="温度" value={status?.temperatureC ? `${status.temperatureC}°C` : "未接入"} />
      </div>
      <DataTable
        title="任务心跳"
        headers={["任务", "优先级", "状态", "心跳"]}
        rows={(status?.tasks ?? []).map((task) => [task.name, task.priority, task.state, task.heartbeat])}
        empty="连接后显示 FreeRTOS 任务状态。"
      />
      <div className="two-column">
        <KeyValue title="传感器摘要" rows={[
          ["门状态", sensors?.doorState ?? "--"],
          ["亮度", sensors ? `${lightValue} / 1023` : "--"],
          ["PIR", sensors?.pir ? "触发" : "未触发"],
          ["更新时间", sensors?.updatedAt ?? "--"],
        ]} />
        <KeyValue title="诊断摘要" rows={[
          ["PSRAM", diagnostics?.psram ?? "--"],
          ["OTA", diagnostics?.otaSlot ?? "--"],
          ["Brownout", String(diagnostics?.brownoutCount ?? "--")],
          ["最近错误", diagnostics?.lastError ?? "--"],
        ]} />
      </div>
      <LogPreview logs={logs.slice(0, 5)} />
    </div>
  );
}

function UsbPanel({
  mode,
  connection,
  notice,
  onConnect,
  onDisconnect,
}: {
  mode: TransportMode;
  connection: ConnectionState;
  notice: string;
  onConnect: () => void;
  onDisconnect: () => void;
}) {
  const serialSupported = Boolean(navigator.serial);
  return (
    <div className="section-flow">
      <div className="section-heading">
        <div>
          <p className="eyebrow">USB CDC / Web Serial</p>
          <h2>连接控制</h2>
        </div>
        <StatusPill state={connection === "connected" ? "ok" : "offline"}>{connection}</StatusPill>
      </div>
      <div className="usb-mode-summary">
        <Radio size={18} />
        <div>
          <strong>{mode === "mock" ? "当前为 Mock 数据模式" : "当前为 Web Serial 真实串口模式"}</strong>
          <span>模式开关已放在左侧导航下方；切换模式会清空面板中的前端显示数据。</span>
        </div>
      </div>
      <div className="usb-layout">
        <div>
          <h3>连接步骤</h3>
          <ol className="ordered-list">
            <li>确认 ESP32-S3 已通过 USB 连接电脑。</li>
            <li>使用 Chrome 或 Edge 通过 localhost 打开本面板。</li>
            <li>选择 Web Serial 后点击连接，并在浏览器弹窗中选择串口。</li>
            <li>固件端按 JSON Lines 协议响应请求。</li>
          </ol>
        </div>
        <div className="command-box">
          <code>{'{"type":"request","command":"get_status","request_id":"..."}'}</code>
          <span>每条消息以换行结束，波特率默认 115200。</span>
        </div>
      </div>
      {!serialSupported && (
        <div className="warning-line">
          <ShieldAlert size={16} />
          当前浏览器未暴露 Web Serial。真实串口模式需要 Chrome/Edge 与 localhost。
        </div>
      )}
      {notice && (
        <div className="warning-line">
          <Info size={16} />
          {notice}
        </div>
      )}
      <div className="button-row">
        <button className="action-button" onClick={onConnect} disabled={connection !== "disconnected"} type="button">
          <Play size={17} />
          连接
        </button>
        <button className="action-button secondary" onClick={onDisconnect} disabled={connection === "disconnected"} type="button">
          <Unplug size={17} />
          断开
        </button>
      </div>
    </div>
  );
}

function NetworkPanel({
  mode,
  connection,
  network,
  networkBusy,
  wifiNetworks,
  wifiScanState,
  mqttConfig,
  mqttTokenInput,
  setMqttTokenInput,
  setNetwork,
  setMqttConfig,
  saveNetwork,
  saveMqttConfig,
  publishMqttState,
  scanWifi,
}: {
  mode: TransportMode;
  connection: ConnectionState;
  network: NetworkConfig | null;
  networkBusy: boolean;
  wifiNetworks: WifiNetwork[];
  wifiScanState: "idle" | "scanning" | "done" | "error";
  mqttConfig: MQTTConfig | null;
  mqttTokenInput: string;
  setMqttTokenInput: (value: string) => void;
  setNetwork: (network: NetworkConfig) => void;
  setMqttConfig: (config: MQTTConfig) => void;
  saveNetwork: (event: FormEvent<HTMLFormElement>) => void;
  saveMqttConfig: (config: MQTTConfig, token: string) => Promise<void>;
  publishMqttState: () => Promise<void>;
  scanWifi: () => Promise<void>;
}) {
  const [showAdvanced, setShowAdvanced] = useState(false);
  const current = network ?? {
    ssid: "",
    wifiPassword: "",
    mqttHost: "",
    apiBaseUrl: "",
    ntpServer: "",
    save: true,
    saveAiKey: false,
  };
  const update = (key: keyof NetworkConfig, value: string) => {
    setNetwork({ ...current, [key]: value, saveAiKey: false });
  };
  const selectedNetwork = wifiNetworks.find((item) => item.ssid === current.ssid);
  const usableNetworks = [...wifiNetworks].sort((a, b) => b.signal - a.signal);
  const scanBusy = wifiScanState === "scanning";
  const canSubmit = connection === "connected" && !networkBusy && current.ssid.trim().length > 0;
  const currentMqtt = mqttConfig ?? {
    brokerUri: current.mqttHost || "",
    homeId: "",
    deviceId: "",
    username: "",
    hasPassword: false,
    enabled: true,
    configured: false,
    connected: false,
    reconnectCount: 0,
    publishedCount: 0,
    receivedCount: 0,
    lastError: 0,
    statusText: "not configured",
  };
  const updateMqtt = (key: keyof MQTTConfig, value: string | boolean | number) => {
    setMqttConfig({ ...currentMqtt, [key]: value });
  };

  return (
    <form className="section-flow" onSubmit={saveNetwork}>
      <div className="section-heading">
        <div>
          <p className="eyebrow">Wi-Fi 快速连接</p>
          <h2>选择网络并连接</h2>
        </div>
        <div className="button-row compact">
          <button className="action-button secondary" type="button" onClick={() => void scanWifi()} disabled={connection !== "connected" || scanBusy || networkBusy}>
            <RotateCw size={17} className={scanBusy ? "spin" : ""} />
            扫描
          </button>
          <button className="action-button" type="submit" disabled={!canSubmit}>
            <Wifi size={17} className={networkBusy ? "spin" : ""} />
            {networkBusy ? "连接中..." : "连接"}
          </button>
        </div>
      </div>

      <div className="wifi-console">
        <div className="wifi-list">
          <div className="wifi-list-head">
            <strong>可用网络</strong>
            <span>{mode === "mock" ? "Mock 扫描" : connection === "connected" ? "真实扫描" : "先连接 USB"}</span>
          </div>
          {usableNetworks.length > 0 && usableNetworks.map((item) => (
            <button
              className={current.ssid === item.ssid ? "wifi-row active" : "wifi-row"}
              key={item.ssid}
              type="button"
              onClick={() => update("ssid", item.ssid)}
            >
              <div className="wifi-row-main">
                <WifiSignal strength={item.signal} />
                <div>
                  <strong>{item.ssid}</strong>
                  <span>{item.note}</span>
                </div>
              </div>
              <div className="wifi-row-meta">
                {item.secured && <LockKeyhole size={15} />}
                <span>{item.band}</span>
                {typeof item.channel === "number" && <span>CH {item.channel}</span>}
                {current.ssid === item.ssid && <Check size={16} />}
              </div>
            </button>
          ))}
          {usableNetworks.length === 0 && (
            <div className="wifi-empty">
              <Wifi size={20} />
              <strong>{wifiScanState === "done" ? "未扫描到 Wi-Fi" : "还没有扫描结果"}</strong>
              <span>{connection === "connected" ? "点击扫描读取 ESP32-S3 附近的真实 2.4GHz 网络。" : "先在左侧切到 USB 并连接设备，或使用 Mock 模式演示。"}</span>
            </div>
          )}
        </div>

        <div className="wifi-connect-panel">
          <div className="wifi-current">
            <span>当前选择</span>
            <strong>{current.ssid || "尚未选择网络"}</strong>
            {networkBusy && <p className="network-progress">正在连接，请等待认证、获取 IP 和联网校时完成。</p>}
            <p>{selectedNetwork?.note ?? "也可以直接手动输入隐藏网络 SSID。ESP32-S3 仅支持 2.4GHz Wi-Fi。"}</p>
            {current.connected && <p>已连接：{current.ip || "等待 IP"} / RSSI {current.rssi ?? "--"} dBm / 外网 {current.internet ? "可用" : "待校时"}</p>}
            {current.lastError && <p className="network-error">{current.lastError}</p>}
          </div>
          <label>
            <span>Wi-Fi 密码</span>
            <input
              type="password"
              value={current.wifiPassword ?? ""}
              placeholder="输入后点击连接"
              autoComplete="new-password"
              onChange={(event) => update("wifiPassword", event.target.value)}
            />
          </label>
          <label>
            <span>手动 SSID</span>
            <input value={current.ssid} required onChange={(event) => update("ssid", event.target.value)} />
          </label>
          <button className="advanced-toggle" type="button" onClick={() => setShowAdvanced((value) => !value)}>
            {showAdvanced ? "收起高级参数" : "展开 MQTT / NTP 高级参数"}
          </button>
        </div>
      </div>

      {showAdvanced && (
        <div className="form-grid">
        <label>
          <span>MQTT 地址</span>
          <input value={current.mqttHost} required onChange={(event) => update("mqttHost", event.target.value)} />
        </label>
        <label>
          <span>NTP 服务器</span>
          <input value={current.ntpServer} onChange={(event) => update("ntpServer", event.target.value)} />
        </label>
        </div>
      )}

      <div className="warning-line">
        <Info size={16} />
        像手机联网一样先选 Wi-Fi、输密码、点连接。AI API 地址和 Key 请到 AI 助手页面的“AI 设置”里配置。
      </div>

      <div className="settings-card">
        <div className="section-heading compact-heading">
          <div>
            <p className="eyebrow">云端 MQTT</p>
            <h3>后端绑定与状态上报</h3>
          </div>
          <span className={`status-pill ${currentMqtt.connected ? "ok" : currentMqtt.configured ? "warn" : "offline"}`}>
            {currentMqtt.connected ? "已连接" : currentMqtt.configured ? "已配置" : "未配置"}
          </span>
        </div>
        <div className="form-grid">
          <label>
            <span>Broker URI</span>
            <input value={currentMqtt.brokerUri} placeholder="mqtts://你的域名:8883" onChange={(event) => updateMqtt("brokerUri", event.target.value)} />
          </label>
          <label>
            <span>Home ID</span>
            <input value={currentMqtt.homeId} placeholder="home_xxx" onChange={(event) => updateMqtt("homeId", event.target.value)} />
          </label>
          <label>
            <span>Device ID</span>
            <input value={currentMqtt.deviceId} placeholder="s3_xxx" onChange={(event) => updateMqtt("deviceId", event.target.value)} />
          </label>
          <label>
            <span>Username</span>
            <input value={currentMqtt.username} placeholder="device_s3_xxx" onChange={(event) => updateMqtt("username", event.target.value)} />
          </label>
          <label>
            <span>Token / Password</span>
            <input
              type="password"
              value={mqttTokenInput}
              placeholder={currentMqtt.hasPassword ? "已保存，留空不覆盖" : "输入后写入设备 NVS"}
              onChange={(event) => setMqttTokenInput(event.target.value)}
            />
          </label>
          <label>
            <span>Keepalive 秒</span>
            <input
              type="number"
              min={15}
              max={300}
              value={currentMqtt.keepaliveSeconds ?? 60}
              onChange={(event) => updateMqtt("keepaliveSeconds", Number(event.target.value))}
            />
          </label>
        </div>
        <div className="button-row">
          <button className="action-button" type="button" disabled={connection !== "connected"} onClick={() => void saveMqttConfig(currentMqtt, mqttTokenInput)}>
            <KeyRound size={17} />
            写入绑定
          </button>
          <button className="action-button secondary" type="button" disabled={connection !== "connected" || !currentMqtt.configured} onClick={() => void publishMqttState()}>
            <Send size={17} />
            发布状态
          </button>
        </div>
        <p className="muted-copy">
          状态：{currentMqtt.statusText || "unknown"}；发布 {currentMqtt.publishedCount}；接收 {currentMqtt.receivedCount}；重连 {currentMqtt.reconnectCount}。
        </p>
      </div>
    </form>
  );
}

function AiPanel({
  connection,
  aiConfig,
  aiProfiles,
  setAiConfig,
  apiKeyInput,
  setApiKeyInput,
  asrConfig,
  setAsrConfig,
  asrKeyInput,
  setAsrKeyInput,
  voiceStatus,
  voiceBusy,
  micTestMode,
  messages,
  draft,
  setDraft,
  busy,
  projectRequest,
  setProjectRequest,
  contextPreview,
  projectBusy,
  deviceChatHistory,
  memorySummary,
  memoryDraft,
  setMemoryDraft,
  saveAiConfig,
  clearAiKey,
  saveAsrConfig,
  clearAsrKey,
  createAiProfile,
  selectAiProfile,
  deleteAiProfile,
  sendAiChat,
  toggleVoiceChat,
  clearMessages,
  previewContext,
  runMockTask,
  refreshDeviceHistory,
  clearDeviceHistory,
  refreshMemory,
  saveMemory,
  clearMemory,
}: {
  connection: ConnectionState;
  aiConfig: AIConfig | null;
  aiProfiles: AIConfig[];
  setAiConfig: (config: AIConfig) => void;
  apiKeyInput: string;
  setApiKeyInput: (value: string) => void;
  asrConfig: ASRConfig | null;
  setAsrConfig: (config: ASRConfig) => void;
  asrKeyInput: string;
  setAsrKeyInput: (value: string) => void;
  voiceStatus: VoiceChatStatus | null;
  voiceBusy: boolean;
  micTestMode: "idle" | "hardware" | "asr";
  messages: AIChatMessage[];
  draft: string;
  setDraft: (value: string) => void;
  busy: boolean;
  projectRequest: ProjectAITaskRequest;
  setProjectRequest: (request: ProjectAITaskRequest) => void;
  contextPreview: AIContextPreview | null;
  projectBusy: boolean;
  deviceChatHistory: DeviceChatHistory | null;
  memorySummary: MemorySummary | null;
  memoryDraft: string;
  setMemoryDraft: (value: string) => void;
  saveAiConfig: (event: FormEvent<HTMLFormElement>) => void;
  clearAiKey: () => Promise<void>;
  saveAsrConfig: () => Promise<void>;
  clearAsrKey: () => Promise<void>;
  createAiProfile: () => Promise<void>;
  selectAiProfile: (profileId: number) => Promise<void>;
  deleteAiProfile: (profileId: number) => Promise<void>;
  sendAiChat: (event?: FormEvent<HTMLFormElement>) => Promise<void>;
  toggleVoiceChat: () => Promise<void>;
  clearMessages: () => void;
  previewContext: () => Promise<void>;
  runMockTask: () => Promise<void>;
  refreshDeviceHistory: () => Promise<void>;
  clearDeviceHistory: () => Promise<void>;
  refreshMemory: () => Promise<void>;
  saveMemory: () => Promise<void>;
  clearMemory: () => Promise<void>;
}) {
  const current = aiConfig ?? defaultAiConfig;
  const currentAsr = asrConfig ?? defaultAsrConfig;
  const update = (patch: Partial<AIConfig>) => {
    setAiConfig({ ...current, ...patch });
  };
  const updateAsr = (patch: Partial<ASRConfig>) => {
    setAsrConfig({ ...currentAsr, ...patch });
  };
  const updateProject = (patch: Partial<ProjectAITaskRequest>) => {
    setProjectRequest({ ...projectRequest, ...patch });
  };
  const connected = connection === "connected";
  const systemPromptBytes = utf8ByteLength(current.systemPrompt);
  const systemPromptTooLong = systemPromptBytes > AI_SYSTEM_PROMPT_MAX_BYTES;
  const contextBytes = utf8ByteLength(JSON.stringify(contextPreview?.context ?? {}, null, 2));
  const historyBytes = utf8ByteLength(JSON.stringify(deviceChatHistory ?? {}, null, 2));
  const memoryBytes = utf8ByteLength(memoryDraft);
  const voiceRecording = voiceStatus?.state === "recording";
  const voiceFromHardwareTest = voiceRecording && micTestMode === "hardware";

  return (
    <div className="section-flow">
      <div className="section-heading">
        <div>
          <p className="eyebrow">Project Context / OpenAI-compatible</p>
          <h2>AI 助手</h2>
        </div>
        <StatusPill state={current.ready ? "ok" : current.hasApiKey ? "warn" : "offline"}>
          {busy ? "对话中" : current.ready ? "已就绪" : current.hasApiKey ? "待测试" : "缺少 Key"}
        </StatusPill>
      </div>

      <div className="warning-line">
        <Info size={16} />
        这里是开发调试入口。真实上下文注入、短期会话历史和测试记忆都由开发板负责，Web 面板只做配置、预览和调试。
      </div>

      <div className="ai-assistant-grid">
        <div className="section-flow">
          <form className="ai-chat-panel assistant-chat" onSubmit={(event) => void sendAiChat(event)}>
            <div className="ai-chat-head">
              <div>
                <h3>真实上下文对话</h3>
                <p>发送后会调用 `ai_assistant_chat`，由开发板注入库存、提醒、偏好、记忆和 48 小时内最多 15 轮设备侧会话历史。</p>
              </div>
              <div className="button-row compact">
                <button className="action-button secondary" type="button" disabled={!connected || busy} onClick={() => void refreshDeviceHistory()}>
                  加载设备历史
                </button>
                <button className="action-button secondary" type="button" disabled={messages.length === 0 || busy} onClick={clearMessages}>
                  清空窗口
                </button>
                <button className="action-button secondary danger-soft" type="button" disabled={!connected || busy} onClick={() => void clearDeviceHistory()}>
                  清空设备历史
                </button>
                <StatusPill state={busy ? "warn" : current.ready ? "ok" : "offline"}>
                  {busy ? "等待中" : current.ready ? "在线" : "未就绪"}
                </StatusPill>
              </div>
            </div>
            <div className="ai-chat-stream">
              {messages.map((message) => (
                <div className={`ai-message ${message.role} ${message.status ?? "ok"}`} key={message.id}>
                  <span>{message.role === "user" ? "?" : "AI"}</span>
                  <p>{message.content}</p>
                  <small>{message.at}{message.latencyMs ? ` / ${message.latencyMs} ms` : ""}</small>
                </div>
              ))}
              {messages.length === 0 && (
                <div className="ai-chat-empty">
                  <Bot size={22} />
                  <strong>还没有对话</strong>
                  <span>先配置可用的 API，再试试“今晚用快过期食材做什么”这类真实问题。</span>
                </div>
              )}
            </div>
            <div className="ai-chat-input">
              <textarea
                value={draft}
                maxLength={512}
                rows={3}
                placeholder="输入一条消息，最多 512 个字符"
                onChange={(event) => setDraft(event.target.value)}
              />
              <button
                className={voiceRecording ? "action-button danger-soft" : "action-button secondary"}
                type="button"
                disabled={!connected || busy || voiceBusy || voiceFromHardwareTest}
                onClick={() => void toggleVoiceChat()}
                title={voiceFromHardwareTest ? "硬件录音测试请到麦克风测试页停止" : voiceRecording ? "stop voice recording" : "start voice recording"}
              >
                <Mic size={17} />
                {voiceFromHardwareTest ? "硬件测试中" : voiceRecording ? "停止" : "语音"}
              </button>
              <button className="action-button" type="submit" disabled={!connected || busy || draft.trim().length === 0}>
                <Send size={17} />
                发送
              </button>
            </div>
            <div className="form-hint">
              Voice: {voiceStatus?.state ?? "idle"} / {voiceStatus?.durationMs ?? 0} ms / rms {voiceStatus?.rms ?? 0}
              {" / "}peak {voiceStatus?.peakAbs ?? 0}
              {" / "}range {voiceStatus?.minSample ?? 0}..{voiceStatus?.maxSample ?? 0}
              {" / "}timeout {voiceStatus?.timeoutCount ?? 0}
              {" / "}hint {voiceStatus?.qualityHint ?? "idle"}。更详细的采样诊断请到“麦克风测试”页。
            </div>
          </form>

          <div className="project-ai-card">
            <div className="ai-chat-head compact-head">
              <h3>上下文预览</h3>
              <StatusPill state={contextPreview?.needsConfirmation ? "warn" : contextPreview ? "ok" : "offline"}>
                {contextPreview ? `${contextBytes} bytes` : "未生成"}
              </StatusPill>
            </div>
            <pre className="json-preview tall">{JSON.stringify(contextPreview?.context ?? { note: "点击右侧“预览上下文”后，这里会显示开发板将注入给模型的数据。" }, null, 2)}</pre>
          </div>

          <div className="project-ai-card">
            <div className="ai-chat-head compact-head">
              <h3>设备会话历史</h3>
              <StatusPill state={deviceChatHistory ? "ok" : "offline"}>
                {deviceChatHistory ? `${Math.floor(deviceChatHistory.count / 2)}/${Math.floor(deviceChatHistory.maxMessages / 2)} 轮` : "未读取"}
              </StatusPill>
            </div>
            <pre className="json-preview tall">{JSON.stringify(deviceChatHistory ?? { note: "这里显示开发板本地保存的短期会话历史，超过 48 小时或超出 15 轮上限会自动裁剪。" }, null, 2)}</pre>
            <div className="button-row">
              <button className="action-button secondary" type="button" disabled={!connected} onClick={() => void refreshDeviceHistory()}>
                刷新历史
              </button>
              <button className="action-button secondary danger-soft" type="button" disabled={!connected} onClick={() => void clearDeviceHistory()}>
                清空历史
              </button>
              <span className="form-hint">{historyBytes} bytes</span>
            </div>
          </div>
        </div>

        <aside className="section-flow">
          <form className="project-ai-card" onSubmit={(event) => { event.preventDefault(); void previewContext(); }}>
            <div className="ai-chat-head compact-head">
              <h3>任务与注入</h3>
              <StatusPill state={projectBusy ? "warn" : connected ? "ok" : "offline"}>{projectBusy ? "处理中" : "可调试"}</StatusPill>
            </div>
            <label>
              <span>任务类型</span>
              <select value={projectRequest.taskType} onChange={(event) => updateProject({ taskType: event.target.value as ProjectAITaskType })}>
                {Object.entries(projectAiTaskLabels).map(([value, label]) => (
                  <option value={value} key={value}>{label}</option>
                ))}
              </select>
            </label>
            <div className="context-toggle-grid">
              <label><input type="checkbox" checked={projectRequest.includeInventory} onChange={(event) => updateProject({ includeInventory: event.target.checked })} />库存</label>
              <label><input type="checkbox" checked={projectRequest.includeReminders} onChange={(event) => updateProject({ includeReminders: event.target.checked })} />提醒</label>
              <label><input type="checkbox" checked={projectRequest.includePreferences} onChange={(event) => updateProject({ includePreferences: event.target.checked })} />偏好</label>
              <label><input type="checkbox" checked={projectRequest.includeMemory} onChange={(event) => updateProject({ includeMemory: event.target.checked })} />记忆</label>
            </div>
            <div className="button-row">
              <button className="action-button secondary" type="submit" disabled={!connected || projectBusy}>
                <Search size={17} />
                预览上下文
              </button>
              <button className="action-button secondary" type="button" disabled={!connected || projectBusy} onClick={() => void runMockTask()}>
                <Play size={17} />
                Mock 任务
              </button>
            </div>
          </form>

          <form className="ai-config-main" onSubmit={saveAiConfig}>
            <div className="ai-chat-head compact-head">
              <h3>AI 设置</h3>
              <StatusPill state={current.ready ? "ok" : current.hasApiKey ? "warn" : "offline"}>
                {current.ready ? "可调用" : "待配置"}
              </StatusPill>
            </div>
            <div className="ai-profile-bar compact-card">
              <div className="ai-profile-head">
                <strong>配置槽</strong>
                <button className="action-button secondary" type="button" disabled={!connected || aiProfiles.length >= 5} onClick={() => void createAiProfile()}>
                  <Plus size={17} />
                  新增
                </button>
              </div>
              <div className="ai-profile-list">
                {(aiProfiles.length > 0 ? aiProfiles : [current]).map((profile) => {
                  const profileId = profile.profileId ?? 0;
                  const active = profileId === (current.profileId ?? 0);
                  return (
                    <button
                      className={active ? "ai-profile-item active" : "ai-profile-item"}
                      disabled={!connected || active}
                      key={profileId}
                      onClick={() => void selectAiProfile(profileId)}
                      type="button"
                    >
                      <span>{profile.profileName || `配置 ${profileId}`}</span>
                      <small>{profile.hasApiKey ? profile.apiKeyPreview || "已保存 Key" : "无 Key"} / {profile.model || "--"}</small>
                    </button>
                  );
                })}
              </div>
            </div>
            <div className="form-grid single">
              <label>
                <span>配置名称</span>
                <input value={current.profileName ?? "默认配置"} maxLength={32} onChange={(event) => update({ profileName: event.target.value })} />
              </label>
              <label>
                <span>API Base URL</span>
                <input value={current.apiBaseUrl} placeholder="https://api.openai.com/v1" inputMode="url" required onChange={(event) => update({ apiBaseUrl: event.target.value })} />
              </label>
              <label>
                <span>模型</span>
                <input value={current.model} placeholder="gpt-4o-mini" required onChange={(event) => update({ model: event.target.value })} />
              </label>
              <label>
                <span>API Key</span>
                <input
                  type="password"
                  value={apiKeyInput}
                  placeholder={current.hasApiKey ? `当前已保存 Key：${current.apiKeyPreview || "已脱敏"}` : "输入后保存到设备 NVS"}
                  maxLength={256}
                  autoComplete="new-password"
                  onChange={(event) => setApiKeyInput(event.target.value)}
                />
              </label>
              <label>
                <span>超时时间 ms</span>
                <input type="number" min={5000} max={45000} value={current.timeoutMs} onChange={(event) => update({ timeoutMs: Number(event.target.value) })} />
              </label>
            </div>
            <label>
              <span>系统提示词</span>
              <textarea
                value={current.systemPrompt}
                rows={4}
                aria-invalid={systemPromptTooLong}
                placeholder="用于约束 AI 助手的基础人格、输出风格和项目规则"
                onChange={(event) => update({ systemPrompt: event.target.value })}
              />
              <small className={systemPromptTooLong ? "form-hint danger" : "form-hint"}>
                {systemPromptBytes}/{AI_SYSTEM_PROMPT_MAX_BYTES} UTF-8 字节
              </small>
            </label>
            <div className="button-row">
              <button className="action-button" type="submit" disabled={!connected || systemPromptTooLong}>
                <KeyRound size={17} />
                保存配置
              </button>
              <button className="action-button secondary" type="button" disabled={!connected || !current.hasApiKey} onClick={() => void clearAiKey()}>
                <Trash2 size={17} />
                清空 Key
              </button>
              <button className="action-button secondary danger-soft" type="button" disabled={!connected || (current.profileId ?? 0) === 0} onClick={() => void deleteAiProfile(current.profileId ?? 0)}>
                删除配置
              </button>
            </div>
            <div className="warning-line">
              <ShieldAlert size={16} />
              当前是开发模式，真实 AI API 的 Key 会保存在设备 NVS 中，但不会在日志或串口响应里明文回显。
            </div>
          </form>

          <form className="ai-config-main" onSubmit={(event) => { event.preventDefault(); void saveAsrConfig(); }}>
            <div className="ai-chat-head compact-head">
              <h3>ASR 设置</h3>
              <StatusPill state={currentAsr.ready ? "ok" : currentAsr.hasApiKey ? "warn" : "offline"}>
                {currentAsr.ready ? "ready" : "need key"}
              </StatusPill>
            </div>
            <div className="form-grid single">
              <label>
                <span>ASR Base URL</span>
                <input value={currentAsr.apiBaseUrl} inputMode="url" required onChange={(event) => updateAsr({ apiBaseUrl: event.target.value })} />
              </label>
              <label>
                <span>ASR 模型</span>
                <input value={currentAsr.model} required onChange={(event) => updateAsr({ model: event.target.value })} />
              </label>
              <label>
                <span>ASR Key</span>
                <input
                  type="password"
                  value={asrKeyInput}
                  placeholder={currentAsr.hasApiKey ? `已保存：${currentAsr.apiKeyPreview || "hidden"}` : "输入后保存到设备 NVS"}
                  maxLength={256}
                  autoComplete="new-password"
                  onChange={(event) => setAsrKeyInput(event.target.value)}
                />
              </label>
              <label>
                <span>ASR 超时 ms</span>
                <input type="number" min={10000} max={90000} value={currentAsr.timeoutMs} onChange={(event) => updateAsr({ timeoutMs: Number(event.target.value) })} />
              </label>
            </div>
            <div className="button-row">
              <button className="action-button" type="submit" disabled={!connected}>
                <Check size={17} />
                保存 ASR
              </button>
              <button className="action-button secondary danger-soft" type="button" disabled={!connected || !currentAsr.hasApiKey} onClick={() => void clearAsrKey()}>
                清除 ASR Key
              </button>
            </div>
          </form>

          <div className="project-ai-card">
            <div className="ai-chat-head compact-head">
              <h3>硬件测试记忆</h3>
              <StatusPill state={memorySummary ? "ok" : "offline"}>{memoryBytes} bytes</StatusPill>
            </div>
            <textarea
              className="json-editor"
              value={memoryDraft}
              rows={9}
              spellCheck={false}
              onChange={(event) => setMemoryDraft(event.target.value)}
            />
            <div className="button-row">
              <button className="action-button secondary" type="button" disabled={!connected} onClick={() => void refreshMemory()}>
                读取
              </button>
              <button className="action-button" type="button" disabled={!connected} onClick={() => void saveMemory()}>
                写入记忆
              </button>
              <button className="action-button secondary danger-soft" type="button" disabled={!connected} onClick={() => void clearMemory()}>
                清空
              </button>
            </div>
          </div>
        </aside>
      </div>
    </div>
  );
}
function PinsPanel({ pins }: { pins: PinInfo[] }) {
  return (
    <div className="section-flow">
      <div className="section-heading">
        <div>
          <p className="eyebrow">只读硬件表</p>
          <h2>GPIO 与引脚风险</h2>
        </div>
        <StatusPill state="warn">禁止直接控制电平</StatusPill>
      </div>
      <div className="table-wrap">
        <table>
          <thead>
            <tr>
              <th>GPIO</th>
              <th>信号</th>
              <th>用途</th>
              <th>安全</th>
              <th>注意事项</th>
            </tr>
          </thead>
          <tbody>
            {pins.map((pin) => (
              <tr key={`${pin.gpio}-${pin.signal}`} className={`pin-${pin.level}`}>
                <td>{pin.gpio}</td>
                <td>{pin.signal}</td>
                <td>{pin.usage}</td>
                <td><SafetyBadge level={pin.level} /></td>
                <td>{pin.note}</td>
              </tr>
            ))}
          </tbody>
        </table>
        {pins.length === 0 && <p className="empty-state">连接后显示推荐引脚与启动敏感脚。</p>}
      </div>
    </div>
  );
}

const micQualityText: Record<string, string> = {
  ok: "采样基本可用，可以继续测试 ASR。",
  silent: "声音过小或接近静音：检查 VDD/GND/SD、L/R 声道和说话距离。",
  clipping: "采样可能削顶：后续可调大 AUDIO_PCM_SHIFT，或降低输入增益。",
  i2s_timeout: "I2S 读取不连续：优先检查 SCK/WS/SD/GND、线长和接触。",
  too_short: "录音太短或没有有效样本：至少录 1-2 秒再判断。",
};

function SpeakerTestPanel({
  connection,
  config,
  setConfig,
  keyInput,
  setKeyInput,
  text,
  setText,
  status,
  busy,
  browserBusy,
  saveConfig,
  clearKey,
  playDevice,
  stopDevice,
  playBrowser,
  refreshStatus,
}: {
  connection: ConnectionState;
  config: TTSConfig | null;
  setConfig: (value: TTSConfig) => void;
  keyInput: string;
  setKeyInput: (value: string) => void;
  text: string;
  setText: (value: string) => void;
  status: TTSStatus | null;
  busy: boolean;
  browserBusy: boolean;
  saveConfig: () => Promise<void>;
  clearKey: () => Promise<void>;
  playDevice: () => Promise<void>;
  stopDevice: () => Promise<void>;
  playBrowser: () => Promise<void>;
  refreshStatus: () => Promise<void>;
}) {
  const connected = connection === "connected";
  const current = config ?? defaultTtsConfig;
  const statusState: "ok" | "warn" | "danger" | "offline" =
    status?.state === "error" ? "danger" : status?.state === "playing" || status?.state === "done" ? "ok" : current.ready ? "warn" : "offline";
  const progress = status?.audioBytes ? Math.min(100, Math.round((status.playedBytes / status.audioBytes) * 100)) : 0;
  const update = (patch: Partial<TTSConfig>) => setConfig({ ...current, ...patch });

  return (
    <div className="section-flow">
      <div className="section-heading">
        <div>
          <p className="eyebrow">MAX98357 / I2S TX / 云端 TTS</p>
          <h2>扬声器测试</h2>
        </div>
        <StatusPill state={statusState}>{status?.state ?? (current.ready ? "ready" : "need key")}</StatusPill>
      </div>

      <div className="warning-line">
        <ShieldAlert size={16} />
        默认测试脚位：BCLK=GPIO40，LRC=GPIO41，DIN=GPIO39。BCLK/WS 与 INMP441 共用；MAX98357 可 5V 供电，但所有逻辑脚必须是 3.3V，首轮避免录音和播放同时进行。
      </div>

      <div className="speaker-layout">
        <form className="project-ai-card" onSubmit={(event) => { event.preventDefault(); void saveConfig(); }}>
          <div className="ai-chat-head compact-head">
            <h3>TTS 配置</h3>
            <StatusPill state={current.ready ? "ok" : current.hasApiKey ? "warn" : "offline"}>{current.ready ? "ready" : "need key"}</StatusPill>
          </div>
          <div className="form-grid single">
            <label>
              <span>TTS URL</span>
              <input value={current.apiBaseUrl} inputMode="url" required onChange={(event) => update({ apiBaseUrl: event.target.value })} />
            </label>
            <label>
              <span>模型</span>
              <input value={current.model} required onChange={(event) => update({ model: event.target.value })} />
            </label>
            <label>
              <span>音色</span>
              <input value={current.voice} required onChange={(event) => update({ voice: event.target.value })} />
            </label>
            <label>
              <span>TTS Key</span>
              <input
                type="password"
                value={keyInput}
                placeholder={current.hasApiKey ? `已保存：${current.apiKeyPreview || "hidden"}` : "输入后保存到设备 NVS"}
                maxLength={256}
                autoComplete="new-password"
                onChange={(event) => setKeyInput(event.target.value)}
              />
            </label>
            <label>
              <span>超时 ms</span>
              <input type="number" min={5000} max={90000} value={current.timeoutMs} onChange={(event) => update({ timeoutMs: Number(event.target.value) })} />
            </label>
          </div>
          <div className="button-row">
            <button className="action-button" type="submit" disabled={!connected}>
              <Check size={17} />
              保存 TTS
            </button>
            <button className="action-button secondary danger-soft" type="button" disabled={!connected || !current.hasApiKey} onClick={() => void clearKey()}>
              清除 TTS Key
            </button>
          </div>
        </form>

        <div className="project-ai-card speaker-control-card">
          <div className="ai-chat-head compact-head">
            <h3>播放测试</h3>
            <span className="form-hint">设备 PCM / 浏览器 MP3</span>
          </div>
          <label>
            <span>测试文本</span>
            <textarea value={text} rows={5} maxLength={512} onChange={(event) => setText(event.target.value)} />
          </label>
          <div className="button-row">
            <button className="action-button" type="button" disabled={!connected || busy || !text.trim()} onClick={() => void playDevice()}>
              <Play size={17} />
              设备播放
            </button>
            <button className="action-button secondary" type="button" disabled={!connected || busy} onClick={() => void stopDevice()}>
              停止
            </button>
            <button className="action-button secondary" type="button" disabled={browserBusy || !text.trim()} onClick={() => void playBrowser()}>
              <Volume2 size={17} />
              浏览器试听
            </button>
            <button className="icon-button" type="button" disabled={!connected || busy} onClick={() => void refreshStatus()} title="刷新扬声器状态">
              <RotateCw size={17} className={busy ? "spin" : ""} />
            </button>
          </div>
          <p className="form-hint">浏览器试听不会读取设备已保存的 Key；如果本页 Key 为空，点击后会提示重新输入，设备播放不受影响。</p>
        </div>
      </div>

      <div className="project-ai-card">
        <div className="ai-chat-head compact-head">
          <h3>设备播放状态</h3>
          <StatusPill state={statusState}>{status?.state ?? "idle"}</StatusPill>
        </div>
        <div className="speaker-progress" aria-label="扬声器播放进度">
          <span style={{ width: `${progress}%` }} />
        </div>
        <div className="sensor-grid">
          <GaugeBlock label="进度" value={status ? `${progress}%` : "--"} state={statusState} />
          <GaugeBlock label="采样率" value={status ? `${status.sampleRate} Hz` : "--"} state="ok" />
          <GaugeBlock label="音频大小" value={status ? `${status.audioBytes} B` : "--"} state={status?.audioBytes ? "ok" : "offline"} />
          <GaugeBlock label="已播放" value={status ? `${status.playedBytes} B` : "--"} state={status?.playedBytes ? "ok" : "offline"} />
          <GaugeBlock label="合成耗时" value={status ? `${status.latencyMs} ms` : "--"} state={status?.latencyMs ? "ok" : "offline"} />
          <GaugeBlock label="HTTP" value={status ? `${status.httpStatus}` : "--"} state={status?.httpStatus && status.httpStatus >= 200 && status.httpStatus < 300 ? "ok" : status?.httpStatus ? "danger" : "offline"} />
        </div>
        <KeyValue title="当前播放参数" rows={[
          ["模型", status?.model || current.model],
          ["音色", status?.voice || current.voice],
          ["时长估算", status?.durationMs ? `${status.durationMs} ms` : "--"],
          ["错误", status?.error || current.lastError || "--"],
        ]} />
      </div>
    </div>
  );
}

function CameraTestPanel({
  connection,
  aiConfig,
  status,
  probe,
  rgb565Diag,
  previewDataUrl,
  analyzeResult,
  busy,
  live,
  liveFrames,
  liveLastAt,
  liveError,
  refreshStatus,
  probeCamera,
  captureFrame,
  toggleLive,
  analyzeFrame,
  runJpegDiag,
  runRgb565Diag,
  resetCamera,
  clearFrame,
}: {
  connection: ConnectionState;
  aiConfig: AIConfig | null;
  status: CameraStatus | null;
  probe: CameraProbeResponse | null;
  rgb565Diag: CameraRgb565DiagResponse | null;
  previewDataUrl: string | null;
  analyzeResult: CameraAnalyzeResponse | null;
  busy: boolean;
  live: boolean;
  liveFrames: number;
  liveLastAt: string;
  liveError: string;
  refreshStatus: () => Promise<void>;
  probeCamera: () => Promise<void>;
  captureFrame: (source?: "manual" | "live") => Promise<void>;
  toggleLive: () => void;
  analyzeFrame: () => Promise<void>;
  runJpegDiag: () => Promise<void>;
  runRgb565Diag: () => Promise<void>;
  resetCamera: () => Promise<void>;
  clearFrame: () => Promise<void>;
}) {
  const connected = connection === "connected";
  const readyForAi = Boolean(aiConfig?.ready);
  const hasFrame = Boolean(status?.hasFrame);
  const probeOk = Boolean(probe?.ok);
  const statusState = !connected ? "offline" : status?.lastError ? "danger" : status?.initialized ? "ok" : "warn";

  return (
    <div className="section-flow">
      <div className="section-heading">
        <div>
          <p className="eyebrow">OV3660 / YUV422 到软件 JPEG / PSRAM</p>
          <h2>摄像头测试</h2>
        </div>
        <StatusPill state={statusState}>{status?.initialized ? "camera ready" : connected ? "等待初始化" : "未连接"}</StatusPill>
      </div>

      <div className="warning-line">
        <Info size={16} />
        先用“安全探测”验证 XCLK + SCCB 是否能读到 OV3660 ID；正式拍照、预览和识别使用稳定的 YUV422 采集后软件 JPEG。
      </div>

      <div className="camera-layout">
        <div className="camera-preview-panel">
          <div className="ai-chat-head compact-head">
            <h3>最近一张照片</h3>
            <StatusPill state={hasFrame ? "ok" : "offline"}>{hasFrame ? `#${status?.frameId}` : "无缓存"}</StatusPill>
          </div>
          <div className="camera-preview">
            {previewDataUrl ? (
              <img src={previewDataUrl} alt="OV3660 最近一张 JPEG 预览" />
            ) : (
              <div>
                <Camera size={38} />
                <span>{hasFrame ? "设备有照片缓存，但本次未返回预览。" : "拍照后显示 QVGA 软件 JPEG 预览。"}</span>
              </div>
            )}
          </div>
          <div className="button-row">
            <button className="action-button secondary" type="button" disabled={!connected || busy} onClick={() => void probeCamera()}>
              <Search size={17} />
              安全探测
            </button>
            <button className="action-button" type="button" disabled={!connected || busy || !probeOk} onClick={() => void captureFrame()} title={probeOk ? "完整 DVP 接线后抓拍一帧" : "请先完成安全探测并确认 PID=0x3660"}>
              <Camera size={17} />
              完整接线后拍照
            </button>
            <button className={`action-button ${live ? "danger-soft" : "secondary"}`} type="button" disabled={!connected || (busy && !live) || (!live && !probeOk)} onClick={toggleLive} title={live ? "停止实时预览" : probeOk ? "完整 DVP 接线后启动低频预览" : "请先完成安全探测并确认 PID=0x3660"}>
              {live ? <Power size={17} /> : <Play size={17} />}
              {live ? "停止实时预览" : "完整接线后预览"}
            </button>
            <button className="action-button secondary" type="button" disabled={!connected || busy || !probeOk || !readyForAi} onClick={() => void analyzeFrame()} title="重新拍摄 OV3660 XGA 高分辨率照片并提交 AI；不依赖当前串口预览图">
              <Sparkles size={17} />
              拍高分辨率并识别
            </button>
            <button className="action-button secondary" type="button" disabled={!connected || busy || !probeOk} onClick={() => void runJpegDiag()} title="仅验证 OV3660 片上 JPEG SOI/码流稳定性；当前颜色可能偏灰偏绿，不作为正式拍照路径">
              片上JPEG码流诊断
            </button>
            <button className="action-button secondary" type="button" disabled={!connected || busy || !probeOk} onClick={() => void runRgb565Diag()} title="抓一帧 QQVGA RGB565，只返回统计信息，用于判断 DVP 是否能成帧">
              RGB565诊断
            </button>
            <button className="action-button secondary" type="button" disabled={!connected || busy} onClick={() => void resetCamera()} title="反初始化摄像头驱动，释放 XCLK/SCCB/DMA 资源">
              重置摄像头
            </button>
            <button className="action-button secondary danger-soft" type="button" disabled={!connected || busy || !hasFrame} onClick={() => void clearFrame()}>
              清除照片
            </button>
            <button className="icon-button" type="button" disabled={!connected || busy} onClick={() => void refreshStatus()} title="刷新摄像头状态">
              <RotateCw size={17} className={busy ? "spin" : ""} />
            </button>
          </div>
          <div className={`camera-live-strip ${live ? "active" : ""}`}>
            <span>{live ? "低频预览运行中" : "实时预览未启动"}</span>
            <strong>{liveFrames} 帧</strong>
            <em>{liveLastAt || "约 2 秒 / 帧，基于串口单帧软件 JPEG"}</em>
          </div>
          <p className="form-hint">AI 识别会重新拍摄 XGA YUV422 并软件压缩 JPEG，这是当前实测稳定的正式提交路径；片上 JPEG 码流诊断仍只用于排查，不作为正式提交图片。</p>
          {liveError && <p className="form-hint danger">实时预览已停止：{liveError}</p>}
          <p className="form-hint">当前只接 4/5/47 时请只点“安全探测”；拍照/预览需要 DVP 全部接好且避开启动复位问题后再做。</p>
          {!readyForAi && <p className="form-hint danger">AI 配置未 ready：识别前需要在 AI 助手页保存支持视觉的模型、Base URL 和 API Key。</p>}
        </div>

        <div className="project-ai-card">
          <div className="ai-chat-head compact-head">
            <h3>采集状态</h3>
            <StatusPill state={status?.lastError ? "danger" : status?.initialized ? "ok" : "offline"}>{status?.lastError || status?.frameSize || "未读取"}</StatusPill>
          </div>
          <div className="camera-metrics">
            <GaugeBlock label="尺寸" value={status?.width ? `${status.width} x ${status.height}` : "--"} state={status?.width ? "ok" : "offline"} />
            <GaugeBlock label="JPEG" value={status ? `${status.jpegBytes} B` : "--"} state={hasFrame ? "ok" : "offline"} />
            <GaugeBlock label="路径" value={status?.pixelFormat || "--"} state={status?.pixelFormat ? "ok" : "offline"} />
            <GaugeBlock label="耗时" value={status ? `${status.captureMs} ms` : "--"} state={hasFrame ? "ok" : "offline"} />
            <GaugeBlock label="PSRAM" value={status ? `${status.freePsramKb} KB` : "--"} state="ok" />
          </div>
          <p className="form-hint">当前可用主路径：YUV422 原始帧 + 低光软件增亮 + 软件 JPEG；“片上 JPEG 码流诊断”仅用于排查 OV3660 内置压缩链路，不建议作为正式拍照结果。</p>
          <KeyValue title="安全探测" rows={[
            ["结果", probe ? (probe.ok ? "通过，读到 OV3660 ID" : "未通过") : "未执行"],
            ["SCCB 地址", probe ? `0x${probe.address.toString(16).padStart(2, "0")}` : "--"],
            ["PID", probe ? `0x${probe.pid.toString(16).padStart(4, "0")} / 期望 0x${probe.expectedPid.toString(16).padStart(4, "0")}` : "--"],
            ["耗时", probe ? `${probe.durationMs} ms` : "--"],
            ["错误", probe?.lastError || probe?.espErrName || "--"],
          ]} />
          <KeyValue title="RGB565 诊断" rows={[
            ["结果", rgb565Diag ? (rgb565Diag.ok ? "成帧，DVP 同步至少部分可用" : "失败") : "未执行"],
            ["尺寸/字节", rgb565Diag ? `${rgb565Diag.width}x${rgb565Diag.height} / ${rgb565Diag.bytes} B` : "--"],
            ["耗时", rgb565Diag ? `${rgb565Diag.captureMs} ms` : "--"],
            ["校验和", rgb565Diag ? `0x${rgb565Diag.checksum.toString(16).padStart(8, "0")}` : "--"],
            ["前16字节", rgb565Diag?.firstBytes || "--"],
            ["错误", rgb565Diag?.lastError || rgb565Diag?.espErrName || "--"],
          ]} />
          <KeyValue title="硬件约束" rows={[
            ["供电", "VDD/DVDD1.5V=1.5V，VDD2.8V/IOVDD/AVDD=3V3，LED 不接"],
            ["SCCB", "SDA=GPIO4 / SCL=GPIO5，上拉到 3.3V"],
            ["XCLK/PWDN", "XCLK=GPIO47，PWDN 不接"],
            ["DVP", "VSYNC=GPIO2，HREF=GPIO38，PCLK=GPIO19，D0-D7=GPIO17/18/8/3/46/48/45/16"],
          ]} />
        </div>
      </div>

      <div className="project-ai-card">
        <div className="ai-chat-head compact-head">
          <h3>AI 识别结果</h3>
          <StatusPill state={analyzeResult ? "ok" : "offline"}>{analyzeResult ? `${analyzeResult.latencyMs} ms` : "未识别"}</StatusPill>
        </div>
        <pre className="json-preview tall">{analyzeResult?.reply || "识别后显示结构化候选。结果只用于确认页，不会直接写入库存。"}</pre>
        {analyzeResult && (
          <p className="form-hint">
            HTTP {analyzeResult.httpStatus} / {analyzeResult.model} / {analyzeResult.width}x{analyzeResult.height} / {analyzeResult.jpegBytes} B / 需要确认：{analyzeResult.needsConfirmation ? "是" : "否"}
          </p>
        )}
      </div>
    </div>
  );
}

function MicrophoneTestPanel({
  connection,
  asrConfig,
  voiceStatus,
  voiceBusy,
  micTestMode,
  micSamples,
  micTranscript,
  micAiReply,
  micAsrResult,
  wakeStatus,
  wakeBusy,
  wakeEvents,
  micPlayback,
  startHardwareTest,
  stopHardwareTest,
  refreshHardwareStatus,
  toggleVoiceChat,
  startWakeListening,
  stopWakeListening,
  refreshWakeStatus,
  resetWakeStats,
}: {
  connection: ConnectionState;
  asrConfig: ASRConfig | null;
  voiceStatus: VoiceChatStatus | null;
  voiceBusy: boolean;
  micTestMode: "idle" | "hardware" | "asr";
  micSamples: VoiceChatStatus[];
  micTranscript: string;
  micAiReply: string;
  micAsrResult: VoiceChatResponse | null;
  wakeStatus: WakeStatus | null;
  wakeBusy: boolean;
  wakeEvents: WakeEventRecord[];
  micPlayback: MicPlayback | null;
  startHardwareTest: () => Promise<void>;
  stopHardwareTest: () => Promise<void>;
  refreshHardwareStatus: () => Promise<void>;
  toggleVoiceChat: () => Promise<void>;
  startWakeListening: () => Promise<void>;
  stopWakeListening: () => Promise<void>;
  refreshWakeStatus: () => Promise<void>;
  resetWakeStats: () => Promise<void>;
}) {
  const connected = connection === "connected";
  const recording = voiceStatus?.state === "recording";
  const wakeListening = Boolean(wakeStatus?.enabled);
  const hardwareRecording = recording && micTestMode === "hardware";
  const asrRecording = recording && micTestMode === "asr";
  const hint = voiceStatus?.qualityHint ?? "too_short";
  const maxBar = Math.max(1, ...micSamples.map((item) => Math.max(item.rms ?? 0, item.peakAbs ?? 0)));
  const wakeState = !connected ? "offline" : wakeStatus?.error ? "danger" : wakeListening ? "warn" : "ok";
  const wakeDisabled = !connected || wakeBusy || recording || voiceBusy;

  return (
    <div className="section-flow">
      <div className="section-heading">
        <div>
          <p className="eyebrow">INMP441 / I2S / ASR</p>
          <h2>麦克风测试</h2>
        </div>
        <StatusPill state={hint === "ok" ? "ok" : hint === "i2s_timeout" || hint === "clipping" ? "danger" : "warn"}>
          {voiceStatus?.qualityHint ?? "等待测试"}
        </StatusPill>
      </div>

      <div className="warning-line">
        <Info size={16} />
        当前接线：VDD=3V3，GND=GND，SCK=GPIO40，WS=GPIO41，SD=GPIO42，L/R=GND。GPIO40/41 同时也接到 MAX98357A 的 BCLK/LRC；这里的硬件录音测试不会触发 ASR 或 AI。
      </div>

      <div className="project-ai-card wake-card">
        <div className="ai-chat-head compact-head">
          <div>
            <h3>WakeNet 唤醒词测试</h3>
            <p className="form-hint">{wakeStatus?.wakeWord ?? "小冰小冰"} / {wakeStatus?.model ?? "wn9_xiaobinxiaobin_tts"}</p>
          </div>
          <StatusPill state={wakeState}>{wakeStatus?.state ?? "未读取"}</StatusPill>
        </div>
        <div className="button-row">
          <button className="action-button" type="button" disabled={wakeDisabled || wakeListening} onClick={() => void startWakeListening()}>
            <Mic size={17} />
            启动监听
          </button>
          <button className="action-button secondary danger-soft" type="button" disabled={!connected || wakeBusy || !wakeListening} onClick={() => void stopWakeListening()}>
            停止监听
          </button>
          <button className="action-button secondary" type="button" disabled={!connected || wakeBusy} onClick={() => void refreshWakeStatus()}>
            <RotateCw size={17} className={wakeBusy ? "spin" : ""} />
            刷新状态
          </button>
          <button className="action-button secondary" type="button" disabled={!connected || wakeBusy} onClick={() => void resetWakeStats()}>
            <Trash2 size={17} />
            清空统计
          </button>
        </div>
        <div className="mic-metrics-grid wake-metrics-grid">
          <GaugeBlock label="触发次数" value={`${wakeStatus?.triggerCount ?? 0}`} state={wakeStatus?.triggerCount ? "warn" : "ok"} />
          <GaugeBlock label="最近触发" value={wakeStatus?.lastTriggerMs ? `${wakeStatus.lastTriggerMs} ms` : "--"} state={wakeStatus?.lastTriggerMs ? "warn" : "offline"} />
          <GaugeBlock label="VAD" value={`${wakeStatus?.vadState ?? -1}`} state={(wakeStatus?.vadState ?? -1) > 0 ? "warn" : "ok"} />
          <GaugeBlock label="RMS / Peak" value={`${wakeStatus?.rms ?? 0} / ${wakeStatus?.peakAbs ?? 0}`} state={(wakeStatus?.rms ?? 0) > 0 ? "ok" : "offline"} />
          <GaugeBlock label="Timeout" value={`${wakeStatus?.timeoutCount ?? 0}`} state={(wakeStatus?.timeoutCount ?? 0) > 0 ? "danger" : "ok"} />
          <GaugeBlock label="错误" value={wakeStatus?.error || "无"} state={wakeStatus?.error ? "danger" : "ok"} />
        </div>
        <div className="wake-event-list" aria-label="最近唤醒事件">
          {wakeEvents.length === 0 ? (
            <span>等待 wake_word_detected 事件</span>
          ) : (
            wakeEvents.map((event) => (
              <span key={event.id}>
                <strong>{event.receivedAt}</strong>
                <em>{event.wakeWord}</em>
                <b>#{event.triggerCount}</b>
              </span>
            ))
          )}
        </div>
      </div>

      <div className="mic-test-grid">
        <div className="project-ai-card mic-control-card">
          <div className="ai-chat-head compact-head">
            <h3>硬件录音测试</h3>
            <StatusPill state={hardwareRecording ? "warn" : connected ? "ok" : "offline"}>
              {hardwareRecording ? "录音中" : connected ? "可测试" : "未连接"}
            </StatusPill>
          </div>
          <div className="button-row">
            <button className="action-button" type="button" disabled={!connected || voiceBusy || recording || wakeListening} onClick={() => void startHardwareTest()}>
              <Mic size={17} />
              开始硬件录音
            </button>
            <button className="action-button secondary danger-soft" type="button" disabled={!connected || voiceBusy || !hardwareRecording} onClick={() => void stopHardwareTest()}>
              停止硬件录音
            </button>
            <button className="action-button secondary" type="button" disabled={!connected || voiceBusy} onClick={() => void refreshHardwareStatus()}>
              <RotateCw size={17} />
              刷新状态
            </button>
          </div>
          <p className="form-hint">录音中会每 500 ms 自动刷新状态；停止后只保留诊断结果，不上传音频。</p>
          {micPlayback && (
            <div className="mic-playback-box">
              <div className="ai-chat-head compact-head">
                <h4>开发板录音回放</h4>
                <span className="form-hint">{micPlayback.createdAt}</span>
              </div>
              <audio controls src={micPlayback.url} />
              <p className="form-hint">
                {micPlayback.sampleRate} Hz / {micPlayback.channels} ch / {micPlayback.bitsPerSample} bit / {micPlayback.durationMs} ms / PCM {micPlayback.pcmBytes} B / WAV {micPlayback.wavBytes} B
              </p>
            </div>
          )}
        </div>

        <div className="project-ai-card">
          <div className="ai-chat-head compact-head">
            <h3>ASR 识别测试</h3>
            <StatusPill state={asrConfig?.ready ? "ok" : asrConfig?.hasApiKey ? "warn" : "offline"}>
              {asrConfig?.ready ? "ASR ready" : asrConfig?.hasApiKey ? "待测试" : "缺少 Key"}
            </StatusPill>
          </div>
          <div className="button-row">
            <button className={asrRecording ? "action-button danger-soft" : "action-button secondary"} type="button" disabled={!connected || voiceBusy || (recording && !asrRecording) || (!asrRecording && wakeListening)} onClick={() => void toggleVoiceChat()}>
              <Mic size={17} />
              {asrRecording ? "停止并识别" : "开始 ASR 测试"}
            </button>
          </div>
          <p className="form-hint">{asrConfig?.apiBaseUrl ?? "未读取 ASR 配置"} / {asrConfig?.model ?? "--"}</p>
        </div>
      </div>

      <div className="mic-metrics-grid">
        <GaugeBlock label="状态" value={voiceStatus?.state ?? "idle"} state={recording ? "warn" : hint === "ok" ? "ok" : "offline"} />
        <GaugeBlock label="时长" value={`${voiceStatus?.durationMs ?? 0} ms`} state="ok" />
        <GaugeBlock label="PCM" value={`${voiceStatus?.pcmBytes ?? 0} B`} state="ok" />
        <GaugeBlock label="样本" value={`${voiceStatus?.sampleCount ?? 0}`} state="ok" />
        <GaugeBlock label="RMS" value={`${voiceStatus?.rms ?? 0}`} state={voiceStatus?.rms ? "ok" : "warn"} />
        <GaugeBlock label="Peak" value={`${voiceStatus?.peakAbs ?? 0}`} state="ok" />
        <GaugeBlock label="Min/Max" value={`${voiceStatus?.minSample ?? 0} / ${voiceStatus?.maxSample ?? 0}`} state="ok" />
        <GaugeBlock label="Timeout" value={`${voiceStatus?.timeoutCount ?? 0}`} state={(voiceStatus?.timeoutCount ?? 0) > 0 ? "danger" : "ok"} />
      </div>

      <div className="mic-test-grid">
        <div className="project-ai-card">
          <div className="ai-chat-head compact-head">
            <h3>音量历史</h3>
            <span className="form-hint">最近 {micSamples.length}/30 次</span>
          </div>
          <div className="mic-bars" aria-label="麦克风音量历史">
            {Array.from({ length: 30 }).map((_, index) => {
              const sample = micSamples[index];
              const rmsHeight = sample ? Math.max(4, Math.round(((sample.rms ?? 0) / maxBar) * 100)) : 4;
              const peakHeight = sample ? Math.max(rmsHeight, Math.round(((sample.peakAbs ?? 0) / maxBar) * 100)) : 4;
              return (
                <span className="mic-bar" key={index}>
                  <i style={{ height: `${peakHeight}%` }} />
                  <b style={{ height: `${rmsHeight}%` }} />
                </span>
              );
            })}
          </div>
        </div>

        <div className="project-ai-card">
          <div className="ai-chat-head compact-head">
            <h3>诊断建议</h3>
            <StatusPill state={hint === "ok" ? "ok" : hint === "i2s_timeout" || hint === "clipping" ? "danger" : "warn"}>{hint}</StatusPill>
          </div>
          <p>{micQualityText[hint] ?? "等待更多录音数据后判断。"}</p>
          <KeyValue title="原始诊断" rows={[
            ["mean", `${voiceStatus?.meanSample ?? 0}`],
            ["clip", `${voiceStatus?.clipCount ?? 0}`],
            ["error", voiceStatus?.error || "无"],
          ]} />
        </div>
      </div>

      <div className="project-ai-card">
        <div className="ai-chat-head compact-head">
          <h3>ASR / AI 结果</h3>
          <StatusPill state={micAsrResult ? "ok" : "offline"}>{micAsrResult ? `${micAsrResult.asrLatencyMs + micAsrResult.aiLatencyMs} ms` : "未测试"}</StatusPill>
        </div>
        <div className="mic-result-grid">
          <div>
            <span className="form-hint">转写文本</span>
            <p>{micTranscript || "完成 ASR 测试后显示。"}</p>
          </div>
          <div>
            <span className="form-hint">AI 回复</span>
            <p>{micAiReply || "完成 ASR + AI 后显示。"}</p>
          </div>
        </div>
        {micAsrResult && (
          <p className="form-hint">ASR HTTP {micAsrResult.asrHttpStatus} / AI HTTP {micAsrResult.aiHttpStatus} / 音频 {micAsrResult.audioBytes} B</p>
        )}
      </div>
    </div>
  );
}

function RadarTestPanel({
  connection,
  status,
  samples,
  running,
  busy,
  startTest,
  stopTest,
  refreshStatus,
}: {
  connection: ConnectionState;
  status: RadarSnapshot | null;
  samples: RadarSnapshot[];
  running: boolean;
  busy: boolean;
  startTest: () => Promise<void>;
  stopTest: () => Promise<void>;
  refreshStatus: () => Promise<void>;
}) {
  const connected = connection === "connected";
  const gateEnergy = status?.gateEnergy ?? Array.from({ length: 16 }, () => 0);
  const maxEnergy = Math.max(1, ...gateEnergy);
  const totalEnergy = Math.max(1, gateEnergy.reduce((sum, value) => sum + value, 0));
  const peakIndex = gateEnergy.reduce((bestIndex, value, index, values) => (value > values[bestIndex] ? index : bestIndex), 0);
  const peakValue = gateEnergy[peakIndex] ?? 0;
  const nearEnergy = status?.nearEnergy ?? gateEnergy.slice(0, 4).reduce((sum, value) => sum + value, 0);
  const midEnergy = status?.midEnergy ?? gateEnergy.slice(4, 9).reduce((sum, value) => sum + value, 0);
  const farEnergy = status?.farEnergy ?? gateEnergy.slice(9).reduce((sum, value) => sum + value, 0);
  const nearShare = Math.round((nearEnergy / totalEnergy) * 100);
  const midShare = Math.round((midEnergy / totalEnergy) * 100);
  const farShare = Math.round((farEnergy / totalEnergy) * 100);
  const estimatedGate = status?.estimatedGate ?? peakIndex;
  const stableGate = status?.stableGate ?? estimatedGate;
  const confidence = status?.confidence ?? 0;
  const stability = status?.stability ?? 0;
  const motionScore = status?.motionScore ?? 0;
  const staticScore = status?.staticScore ?? 0;
  const humanScore = status?.humanScore ?? 0;
  const staticClutter = Boolean(status?.staticClutter);
  const humanCandidate = Boolean(status?.humanCandidate);
  const stableTarget = Boolean(status?.stablePresence);
  const within1m = Boolean(status?.within1m);
  const approaching = Boolean(status?.approaching);
  const approachScore = status?.approachScore ?? 0;
  const approachFrames = status?.approachFrames ?? 0;
  const approachDistanceDelta = status?.approachDistanceDelta ?? 0;
  const thresholdScore = status?.thresholdScore ?? 0;
  const radarPresenceText = !status ? "等待测试" : status.presence ? "模块上报有目标" : "模块上报无目标";
  const zoneLabel = status?.stableZone === "near" ? "近区" : status?.stableZone === "mid" ? "中区" : status?.stableZone === "far" ? "远区" : "未稳定";
  const semanticStateLabel = !status
      ? "等待测试"
    : status.targetClass === "reliable_approaching"
      ? "可靠靠近人体"
      : status.targetClass === "reliable_within_1m"
        ? "可靠 1 米内人体"
      : status.targetClass === "reliable_human" || stableTarget
          ? "可靠人体"
          : status.targetClass === "human_candidate" || humanCandidate
            ? "人体候选"
            : status.targetClass === "static_reflection" || staticClutter
              ? "静态反射"
              : status.targetClass === "near_clutter" || status.nearClutter
                ? "近场杂波"
                : status.presence
                  ? "原始目标"
                  : "无目标";
  const within1mText = !status ? "--" : status.nearClutter ? "近场杂波，不计入人体" : staticClutter ? "静态反射，不计入人体" : within1m ? "可靠 1 米内人体" : humanCandidate ? "人体候选，未确认 1 米内" : "未确认";
  const approachingText = !status ? "--" : status.nearClutter ? "近场杂波，不判断" : staticClutter ? "静态反射，不判断" : approaching ? "可靠靠近" : humanCandidate ? "人体候选，靠近证据不足" : "未确认";
  const targetLabel = !status
    ? "等待测试"
    : status.lastError || status.mode === "error"
      ? "诊断异常"
      : stableTarget
        ? `${zoneLabel}可靠人体`
        : humanCandidate
          ? "人体候选，待连续靠近/微动确认"
        : status.nearClutter
          ? "近场杂波，未计为人体"
        : staticClutter
          ? "静态反射，疑似墙面/柜体"
        : status.presence
          ? "原始上报有目标，距离未稳定"
          : "未见稳定目标";
  const targetState: "ok" | "warn" | "danger" | "offline" = !status
    ? "offline"
    : status.lastError || status.mode === "error"
      ? "danger"
      : stableTarget
        ? "warn"
        : humanCandidate
          ? "warn"
        : staticClutter
          ? "danger"
        : "ok";
  const timeline = samples.slice(-60);
  const healthState = !status ? "offline" : status.lastError || status.mode === "error" || staticClutter ? "danger" : stableTarget || humanCandidate ? "warn" : "ok";
  const latestTrend = timeline.slice(-8);
  const recentSamples = timeline.slice(-12);
  const distanceOf = (sample?: RadarSnapshot | null) => {
    if (!sample) {
      return 0;
    }
    return sample.smoothedDistanceRaw && sample.smoothedDistanceRaw > 0 ? sample.smoothedDistanceRaw : sample.distanceRaw ?? 0;
  };
  const latestSample = recentSamples.length > 0 ? recentSamples[recentSamples.length - 1] : status;
  const previousSample = recentSamples.length > 1 ? recentSamples[recentSamples.length - 2] : null;
  const latestDistance = distanceOf(latestSample);
  const previousDistance = distanceOf(previousSample);
  const distanceDelta = latestSample ? latestDistance - previousDistance : 0;
  const distanceTrendLabel = !status
    ? "--"
    : distanceDelta <= -25
      ? `靠近 ${Math.abs(distanceDelta)}`
      : distanceDelta >= 25
        ? `远离 ${distanceDelta}`
        : "平稳";
  const distanceBandLabel = !status
    ? "--"
    : latestDistance === 0
      ? "无原始目标"
      : latestDistance <= 120
        ? "1 米内候选"
        : latestDistance <= 300
          ? "近距离候选"
          : latestDistance <= 700
            ? "中距离候选"
            : "远距离候选";
  const motionHintText = !status
    ? "--"
    : status.nearClutter || staticClutter
      ? status.nearClutter ? "近场杂波" : "静态反射"
      : distanceDelta <= -25
        ? "目标在靠近"
        : distanceDelta >= 25
          ? "目标在远离"
          : status.presence
            ? "目标距离平稳"
            : "无目标";
  const latestRawDistanceText = !status ? "--" : `${latestSample?.distanceRaw ?? status.distanceRaw}`;
  const latestSmoothedDistanceText = !status ? "--" : `${latestSample?.smoothedDistanceRaw ?? status.smoothedDistanceRaw ?? 0}`;

  return (
    <div className="section-flow">
      <div className="section-heading">
        <div>
          <p className="eyebrow">HMMD / 24GHz / UART</p>
          <h2>雷达测试</h2>
        </div>
        <StatusPill state={healthState}>{status ? `${radarPresenceText} / ${status.mode}` : "等待测试"}</StatusPill>
      </div>

      <div className="warning-line">
        <Info size={16} />
        当前测试接线：3V3、GND、雷达 TX 接 GPIO21、雷达 RX 接 GPIO20，OT2 不接。模块和 UART 均为 3.3V 逻辑，雷达结果只作为靠近/有人上下文，不单独判定开门。
      </div>

      <div className="mic-test-grid">
        <div className="project-ai-card mic-control-card">
          <div className="ai-chat-head compact-head">
            <h3>上报模式</h3>
            <StatusPill state={running ? "warn" : connected ? "ok" : "offline"}>
              {running ? "轮询中" : connected ? "可测试" : "未连接"}
            </StatusPill>
          </div>
          <div className="button-row">
            <button className="action-button" type="button" disabled={!connected || busy || running} onClick={() => void startTest()}>
              <Radio size={17} />
              开始雷达测试
            </button>
            <button className="action-button secondary danger-soft" type="button" disabled={!connected || busy || !running} onClick={() => void stopTest()}>
              停止测试
            </button>
            <button className="action-button secondary" type="button" disabled={!connected || busy} onClick={() => void refreshStatus()}>
              <RotateCw size={17} />
              刷新
            </button>
          </div>
          <p className="form-hint">开始后固件会进入上报模式。页面优先看多帧稳定结果；原始门值只用于排查杂波和安装角度。</p>
        </div>

        <div className="project-ai-card">
          <div className="ai-chat-head compact-head">
            <h3>串口诊断</h3>
            <StatusPill state={status?.ready ? "ok" : "offline"}>{status?.ready ? "UART ready" : "no data"}</StatusPill>
          </div>
          <KeyValue title="原始计数" rows={[
            ["frame", `${status?.frameCount ?? 0}`],
            ["parse error", `${status?.parseErrorCount ?? 0}`],
            ["timeout", `${status?.timeoutCount ?? 0}`],
            ["last", status?.lastText || "--"],
            ["error", status?.lastError || "无"],
          ]} />
        </div>
      </div>

      <div className="mic-metrics-grid">
        <GaugeBlock label="模块上报" value={status ? radarPresenceText : "--"} state={status?.presence ? "warn" : status ? "ok" : "offline"} />
        <GaugeBlock label="目标状态" value={status ? targetLabel : "--"} state={targetState} />
        <GaugeBlock label="1米内人体" value={within1mText} state={status?.nearClutter || staticClutter ? "danger" : within1m ? "warn" : status ? "ok" : "offline"} />
        <GaugeBlock label="正在靠近" value={approachingText} state={approaching ? "warn" : status?.nearClutter || staticClutter ? "danger" : status ? "ok" : "offline"} />
        <GaugeBlock label="语义分类" value={semanticStateLabel} state={staticClutter || status?.nearClutter ? "danger" : stableTarget || humanCandidate ? "warn" : status ? "ok" : "offline"} />
        <GaugeBlock label="人体评分" value={status ? `${humanScore}%` : "--"} state={humanScore >= 45 ? "warn" : status ? "ok" : "offline"} />
        <GaugeBlock label="静态评分" value={status ? `${staticScore}%` : "--"} state={staticScore >= 70 ? "danger" : status ? "ok" : "offline"} />
        <GaugeBlock label="近场杂波" value={status ? (status.nearClutter ? "疑似" : "否") : "--"} state={status?.nearClutter ? "danger" : status ? "ok" : "offline"} />
        <GaugeBlock label="静态反射" value={status ? (staticClutter ? "疑似墙面" : "否") : "--"} state={staticClutter ? "danger" : status ? "ok" : "offline"} />
        <GaugeBlock label="稳定区域" value={status ? zoneLabel : "--"} state={stableTarget ? "warn" : status ? "ok" : "offline"} />
        <GaugeBlock label="可信度" value={status ? `${confidence}%` : "--"} state={confidence >= 70 ? "warn" : "ok"} />
        <GaugeBlock label="稳定度" value={status ? `${stability}%` : "--"} state={stability >= 60 ? "warn" : "ok"} />
        <GaugeBlock label="微动评分" value={status ? `${motionScore}%` : "--"} state={motionScore >= 28 ? "warn" : staticClutter ? "danger" : "ok"} />
        <GaugeBlock label="距离抖动" value={status ? `${status.distanceSpan ?? 0}` : "--"} state="ok" />
        <GaugeBlock label="能量变化" value={status ? `${status.energyChangeScore ?? 0}%` : "--"} state="ok" />
        <GaugeBlock label="靠近评分" value={status ? `${approachScore}%` : "--"} state={approaching ? "warn" : "ok"} />
        <GaugeBlock label="靠近帧/距离" value={status ? `${approachFrames} / ${approachDistanceDelta}` : "--"} state={approaching ? "warn" : "ok"} />
        <GaugeBlock label="厂家阈值" value={status ? (status.thresholdPresence ? `命中 ${thresholdScore}%` : "未命中") : "--"} state={status?.thresholdPresence ? "warn" : status ? "ok" : "offline"} />
        <GaugeBlock label="拒绝原因" value={status?.rejectionReason || "无"} state={status?.rejectionReason ? (staticClutter || status?.nearClutter ? "danger" : "ok") : "ok"} />
        <GaugeBlock label="阈值门" value={status ? `门 ${status.thresholdGate ?? "--"}` : "--"} state="ok" />
        <GaugeBlock label="保持帧数" value={status ? `${status.holdFramesRemaining ?? 0}` : "--"} state={(status?.holdFramesRemaining ?? 0) > 0 ? "warn" : "ok"} />
        <GaugeBlock label="稳定门" value={status ? `门 ${stableGate}` : "--"} state="ok" />
        <GaugeBlock label="估计门" value={status ? `门 ${estimatedGate}` : "--"} state="ok" />
        <GaugeBlock label="最强门/值" value={status ? `门 ${status.peakGate ?? peakIndex} / ${status.peakEnergy ?? peakValue}` : "--"} state="ok" />
        <GaugeBlock label="原始距离值" value={status ? `${status.distanceRaw}` : "--"} state="ok" />
        <GaugeBlock label="平滑距离值" value={status ? `${status.smoothedDistanceRaw ?? 0}` : "--"} state="ok" />
        <GaugeBlock label="OT2 接线" value="不接" state="offline" />
        <GaugeBlock label="更新时间" value={status ? `${status.updatedAtMs} ms` : "--"} state={status ? "ok" : "offline"} />
      </div>

      <div className="project-ai-card radar-distance-card">
        <div className="ai-chat-head compact-head">
          <h3>距离理解</h3>
          <span className="form-hint">按连续帧估算</span>
        </div>
        <div className="radar-summary-grid">
          <div className={`radar-summary-item ${distanceDelta <= -25 || approaching ? "warn" : distanceDelta >= 25 ? "ok" : ""}`}>
            <span>当前距离</span>
            <strong>{status ? latestRawDistanceText : "--"}</strong>
            <em>{status ? `平滑 ${latestSmoothedDistanceText}` : "--"}</em>
          </div>
          <div className={`radar-summary-item ${distanceDelta <= -25 || approaching ? "warn" : distanceDelta >= 25 ? "ok" : ""}`}>
            <span>距离趋势</span>
            <strong>{distanceTrendLabel}</strong>
            <em>{motionHintText}</em>
          </div>
          <div className="radar-summary-item">
            <span>门位判断</span>
            <strong>{status ? `估计 ${estimatedGate} / 稳定 ${stableGate}` : "--"}</strong>
            <em>{status ? `阈值 ${status.thresholdGate ?? "--"} · ${distanceBandLabel}` : "--"}</em>
          </div>
          <div className={`radar-summary-item ${staticClutter ? "danger" : ""}`}>
            <span>语义状态</span>
            <strong>{semanticStateLabel}</strong>
            <em>{status ? `人体 ${humanScore}% · 静态 ${staticScore}% · 微动 ${motionScore}%` : "--"}</em>
          </div>
        </div>
        <div className="radar-semantic-grid">
          <div className={within1m ? "active" : status?.nearClutter || staticClutter ? "blocked" : ""}>
            <strong>1 米内人体</strong>
            <span>{within1mText}</span>
          </div>
          <div className={approaching ? "active" : status?.nearClutter || staticClutter ? "blocked" : ""}>
            <strong>靠近趋势</strong>
            <span>{approachingText}</span>
          </div>
        </div>
        <div className="radar-distance-track" aria-label="雷达稳定距离区间">
          {[
            { key: "near", label: "近", range: "门 0-3 / 约 0-2.8 m" },
            { key: "mid", label: "中", range: "门 4-8 / 约 2.8-6.3 m" },
            { key: "far", label: "远", range: "门 9-12 / 约 6.3-9.1 m" },
          ].map((item) => (
            <span className={status?.stableZone === item.key && stableTarget ? "active" : ""} key={item.key}>
              <strong>{item.label}</strong>
              <em>{item.range}</em>
            </span>
          ))}
        </div>
        {recentSamples.length > 0 && (
          <div className="radar-distance-mini" aria-label="最近距离变化">
            {(() => {
              const maxDistance = Math.max(1, ...recentSamples.map((sample) => distanceOf(sample)));
              return recentSamples.map((sample, index) => {
                const distance = distanceOf(sample);
                const height = distance > 0 ? Math.max(10, Math.round((distance / maxDistance) * 100)) : 10;
                return (
                  <span
                    key={`${sample.frameCount}-${index}`}
                    style={{ height: `${height}%` }}
                    title={`raw=${sample.distanceRaw} smooth=${sample.smoothedDistanceRaw ?? 0} gate=${sample.stableGate ?? sample.estimatedGate ?? "--"}`}
                  />
                );
              });
            })()}
          </div>
        )}
        <p className="form-hint">已参考厂家上位机：每门按触发/保持双阈值判断，目标短暂丢失时保留约 3 秒保持态。手册说明一个距离门约 70cm；1 米内仍按“疑似”展示。</p>
      </div>

      <div className="project-ai-card">
        <div className="ai-chat-head compact-head">
          <h3>回波区域</h3>
          <span className="form-hint">近 / 中 / 远</span>
        </div>
        <div className="radar-zone-grid" aria-label="雷达回波区域">
          {[
            { label: "近区", gates: "0-3", value: nearEnergy, share: nearShare, state: (stableGate <= 3 && stableTarget ? "warn" : "ok") as "ok" | "warn" },
            { label: "中区", gates: "4-8", value: midEnergy, share: midShare, state: (stableGate >= 4 && stableGate <= 8 && stableTarget ? "warn" : "ok") as "ok" | "warn" },
            { label: "远区", gates: "9-15", value: farEnergy, share: farShare, state: (stableGate >= 9 && stableTarget ? "warn" : "ok") as "ok" | "warn" },
          ].map((band) => (
            <div className="radar-zone" key={band.label}>
              <div className="radar-zone-head">
                <strong>{band.label}</strong>
                <span>{band.gates}</span>
              </div>
              <div className="radar-zone-bar">
                <i style={{ width: `${Math.max(6, Math.round((band.value / totalEnergy) * 100))}%` }} />
              </div>
              <div className="radar-zone-foot">
                <b>{band.value}</b>
                <em>{band.share}%</em>
                <StatusDot state={band.state} />
              </div>
            </div>
          ))}
        </div>
      </div>

      <div className="project-ai-card">
        <div className="ai-chat-head compact-head">
          <h3>原始门值</h3>
          <span className="form-hint">0-15</span>
        </div>
        <div className="radar-raw-grid" aria-label="雷达原始距离门">
          {gateEnergy.map((value, index) => {
            const active = index === estimatedGate || index === stableGate;
            const height = Math.max(6, Math.round((value / maxEnergy) * 100));
            return (
              <span className={`radar-raw-cell ${active ? "active" : ""} ${index === peakIndex ? "peak" : ""}`} key={index}>
                <i style={{ height: `${height}%` }} />
                <b>{index}</b>
                <em>{value}</em>
              </span>
            );
          })}
        </div>
      </div>

      <div className="project-ai-card">
        <div className="ai-chat-head compact-head">
          <h3>存在时间线</h3>
          <span className="form-hint">最近 {timeline.length}/60 帧</span>
        </div>
        <div className="radar-timeline" aria-label="雷达存在时间线">
          {Array.from({ length: 60 }).map((_, index) => {
            const sample = timeline[index];
            return (
              <span
                className={sample?.staticClutter || sample?.nearClutter ? "blocked" : sample?.stablePresence ? "active stable" : sample?.humanCandidate ? "active candidate" : sample?.presence ? "active" : ""}
                key={index}
                title={sample ? `class=${sample.targetClass ?? "--"} raw=${sample.distanceRaw} gate=${sample.stableGate ?? sample.estimatedGate ?? "--"} human=${sample.humanScore ?? 0} static=${sample.staticScore ?? 0}` : "--"}
              />
            );
          })}
        </div>
        {latestTrend.length > 0 && (
          <div className="radar-trend-row">
            {latestTrend.map((sample, index) => (
              <span key={`${sample.frameCount}-${index}`}>
                <b>{sample.stableGate ?? sample.estimatedGate ?? "-"}</b>
                <em>{sample.confidence ?? 0}%</em>
              </span>
            ))}
          </div>
        )}
      </div>
    </div>
  );
}

function SensorsPanel({ sensors }: { sensors: SensorSnapshot | null }) {
  const light10 = sensors?.lightValue10bit ?? sensors?.lux;
  const lightRaw = sensors?.lightRaw12bit;
  const lightPercent = sensors?.lightPercent;
  const lightPolarityLabel = sensors?.lightPolarity === "raw_high_dark" ? "高=暗 / 低=亮" : "按设备上报";
  const radarLabel = !sensors?.radar
    ? "--"
    : sensors.radar.targetClass === "reliable_approaching"
      ? "可靠靠近人体"
      : sensors.radar.stablePresence
        ? "可靠人体"
        : sensors.radar.humanCandidate
          ? "人体候选"
          : sensors.radar.staticClutter
        ? "静态反射"
        : sensors.radar.nearClutter
          ? "近场杂波"
          : sensors.radar.presence
            ? "模块有目标"
            : "无目标";
  const radarState: "ok" | "warn" | "danger" | "offline" = !sensors?.radar
    ? "offline"
    : sensors.radar.staticClutter || sensors.radar.nearClutter
      ? "danger"
      : sensors.radar.stablePresence || sensors.radar.humanCandidate
        ? "warn"
        : "ok";
  const imuAddress = sensors?.imuAddress !== undefined ? `0x${sensors.imuAddress.toString(16).toUpperCase()}` : "--";
  const imuWhoAmI = sensors?.imuWhoAmI !== undefined ? `0x${sensors.imuWhoAmI.toString(16).toUpperCase()}` : "--";
  const accelMagnitude =
    sensors?.accelXG !== undefined && sensors?.accelYG !== undefined && sensors?.accelZG !== undefined
      ? Math.sqrt((sensors.accelXG * sensors.accelXG) + (sensors.accelYG * sensors.accelYG) + (sensors.accelZG * sensors.accelZG)).toFixed(3)
      : "--";
  const gyroMagnitude =
    sensors?.gyroXDps !== undefined && sensors?.gyroYDps !== undefined && sensors?.gyroZDps !== undefined
      ? Math.sqrt((sensors.gyroXDps * sensors.gyroXDps) + (sensors.gyroYDps * sensors.gyroYDps) + (sensors.gyroZDps * sensors.gyroZDps)).toFixed(2)
      : "--";
  return (
    <div className="section-flow">
      <div className="section-heading">
        <div>
          <p className="eyebrow">本地状态机输入</p>
          <h2>传感器快照</h2>
        </div>
        <StatusPill state={sensors ? "ok" : "offline"}>{sensors?.updatedAt ?? "无数据"}</StatusPill>
      </div>
      <div className="sensor-grid">
        <GaugeBlock label="亮度 0-1023" value={sensors ? `${light10}` : "--"} state="ok" />
        <GaugeBlock label="ADC 原始值" value={lightRaw !== undefined ? `${lightRaw}` : "--"} state="ok" />
        <GaugeBlock label="亮度百分比" value={lightPercent !== undefined ? `${lightPercent}%` : "--"} state="ok" />
        <GaugeBlock label="光敏极性" value={sensors ? lightPolarityLabel : "--"} state="ok" />
        <GaugeBlock label="PIR" value={sensors?.pir ? "触发" : "未触发"} state={sensors?.pir ? "warn" : "ok"} />
        <GaugeBlock label="亮度突变" value={sensors ? `${sensors.lightDelta}` : "--"} state="warn" />
        <GaugeBlock label="姿态变化" value={sensors ? `${sensors.angleDelta}°` : "--"} state="ok" />
        <GaugeBlock label="震动峰值" value={sensors ? `${sensors.vibrationPeak.toFixed(4)} g` : "--"} state="ok" />
        <GaugeBlock label="门状态" value={sensors?.doorState ?? "--"} state="ok" />
        <GaugeBlock label="雷达" value={radarLabel} state={radarState} />
      </div>
      <div className="sensor-grid">
        <GaugeBlock label="IMU 就绪" value={sensors?.imuReady ? "是" : "否"} state={sensors?.imuReady ? "ok" : "offline"} />
        <GaugeBlock label="IMU 地址" value={imuAddress} state="ok" />
        <GaugeBlock label="WHO_AM_I" value={imuWhoAmI} state="ok" />
        <GaugeBlock label="温度" value={sensors?.imuTemperatureC !== undefined ? `${sensors.imuTemperatureC.toFixed(2)} °C` : "--"} state="ok" />
        <GaugeBlock label="Pitch" value={sensors?.pitchDeg !== undefined ? `${sensors.pitchDeg.toFixed(2)}°` : "--"} state="ok" />
        <GaugeBlock label="Roll" value={sensors?.rollDeg !== undefined ? `${sensors.rollDeg.toFixed(2)}°` : "--"} state="ok" />
        <GaugeBlock label="加速度模长" value={accelMagnitude} state="ok" />
        <GaugeBlock label="陀螺仪模长" value={gyroMagnitude !== "--" ? `${gyroMagnitude} °/s` : "--"} state="ok" />
        <GaugeBlock label="I2C 错误" value={sensors?.imuError !== undefined ? `${sensors.imuError}` : "--"} state={sensors?.imuError ? "danger" : "ok"} />
      </div>
      <KeyValue title="MPU6050 原始读数" rows={[
        ["Accel X/Y/Z", sensors?.accelXG !== undefined ? `${sensors.accelXG.toFixed(4)} / ${sensors.accelYG?.toFixed(4)} / ${sensors.accelZG?.toFixed(4)} g` : "--"],
        ["Gyro X/Y/Z", sensors?.gyroXDps !== undefined ? `${sensors.gyroXDps.toFixed(3)} / ${sensors.gyroYDps?.toFixed(3)} / ${sensors.gyroZDps?.toFixed(3)} °/s` : "--"],
      ]} />
      <KeyValue title="外设状态" rows={[
        ["触摸", sensors?.touch ?? "--"],
        ["屏幕", sensors?.display ?? "--"],
        ["蜂鸣器", sensors?.buzzer ?? "--"],
      ]} />
    </div>
  );
}

function LogsPanel({
  logs,
  level,
  setLevel,
  searchTerm,
  setSearchTerm,
  clearLogs,
  exportLogs,
}: {
  logs: DeviceLog[];
  level: LogLevel | "all";
  setLevel: (level: LogLevel | "all") => void;
  searchTerm: string;
  setSearchTerm: (value: string) => void;
  clearLogs: () => void;
  exportLogs: () => void;
}) {
  return (
    <div className="section-flow">
      <div className="section-heading">
        <div>
          <p className="eyebrow">串口日志流</p>
          <h2>日志</h2>
        </div>
        <div className="button-row compact">
          <button className="icon-button" type="button" onClick={exportLogs} title="导出日志">
            <Download size={17} />
          </button>
          <button className="action-button secondary" type="button" onClick={clearLogs}>
            清空
          </button>
        </div>
      </div>
      <div className="log-toolbar">
        <label>
          <SlidersHorizontal size={16} />
          <select value={level} onChange={(event) => setLevel(event.target.value as LogLevel | "all")}>
            <option value="all">全部级别</option>
            <option value="debug">调试</option>
            <option value="info">信息</option>
            <option value="warn">警告</option>
            <option value="error">错误</option>
          </select>
        </label>
        <label>
          <Search size={16} />
          <input value={searchTerm} placeholder="搜索来源或内容" onChange={(event) => setSearchTerm(event.target.value)} />
        </label>
      </div>
      <div className="log-stream">
        {logs.map((log) => (
          <div className={`log-line ${log.level}`} key={log.id}>
            <time>{log.at}</time>
            <span>{levelLabel[log.level]}</span>
            <strong>{log.source}</strong>
            <p>{log.message}</p>
          </div>
        ))}
        {logs.length === 0 && <p className="empty-state">暂无日志。连接 Mock 或 USB 设备后会出现日志流。</p>}
      </div>
    </div>
  );
}

function DiagnosticsPanel({ diagnostics }: { diagnostics: DiagnosticSnapshot | null }) {
  return (
    <div className="section-flow">
      <div className="section-heading">
        <div>
          <p className="eyebrow">底层健康检查</p>
          <h2>诊断</h2>
        </div>
        <StatusPill state={diagnostics?.brownoutCount || diagnostics?.watchdogCount ? "danger" : diagnostics ? "ok" : "offline"}>
          {diagnostics ? "已读取" : "无数据"}
        </StatusPill>
      </div>
      <KeyValue title="系统诊断" rows={[
        ["PSRAM", diagnostics?.psram ?? "--"],
        ["Flash 分区", diagnostics?.flashPartition ?? "--"],
        ["LittleFS/cache", diagnostics?.littlefs ?? "--"],
        ["OTA", diagnostics?.otaSlot ?? "--"],
        ["Brownout", String(diagnostics?.brownoutCount ?? "--")],
        ["Watchdog", String(diagnostics?.watchdogCount ?? "--")],
        ["最近错误", diagnostics?.lastError ?? "--"],
      ]} />
    </div>
  );
}

function SettingsPanel({
  timeoutMs,
  setTimeoutMs,
  refreshSeconds,
  setRefreshSeconds,
  stateMachineConfig,
  setStateMachineConfig,
  stateMachineStatus,
  saveStateMachineConfig,
  connected,
}: {
  timeoutMs: number;
  setTimeoutMs: (value: number) => void;
  refreshSeconds: number;
  setRefreshSeconds: (value: number) => void;
  stateMachineConfig: StateMachineConfig | null;
  setStateMachineConfig: (value: StateMachineConfig) => void;
  stateMachineStatus: StateMachineStatus | null;
  saveStateMachineConfig: () => Promise<void>;
  connected: boolean;
}) {
  const config = stateMachineConfig ?? {
    nightLightThreshold: 250,
    dayLightThreshold: 450,
    radarTwoMeterRaw: 200,
    radarTwoMeterGate: 8,
    sleepEnabled: false,
    autoVoiceAfterClose: true,
    autoVoiceRecordSeconds: 6,
    closeStableMs: 2500,
  };
  const updateConfig = (patch: Partial<StateMachineConfig>) => {
    setStateMachineConfig({ ...config, ...patch });
  };

  return (
    <div className="section-flow">
      <div className="section-heading">
        <div>
          <p className="eyebrow">本地偏好</p>
          <h2>设置</h2>
        </div>
      </div>
      <div className="form-grid">
        <label>
          <span>协议超时 ms</span>
          <input type="number" min={800} max={45000} value={timeoutMs} onChange={(event) => setTimeoutMs(Number(event.target.value))} />
        </label>
        <label>
          <span>自动刷新秒数</span>
          <input type="number" min={10} max={120} value={refreshSeconds} onChange={(event) => setRefreshSeconds(Number(event.target.value))} />
        </label>
      </div>
      <div className="data-table">
        <h3>状态机阈值</h3>
        <div className="form-grid">
          <label>
            <span>夜间阈值</span>
            <input type="number" min={0} max={1023} value={config.nightLightThreshold} onChange={(event) => updateConfig({ nightLightThreshold: Number(event.target.value) })} />
          </label>
          <label>
            <span>白天阈值</span>
            <input type="number" min={0} max={1023} value={config.dayLightThreshold} onChange={(event) => updateConfig({ dayLightThreshold: Number(event.target.value) })} />
          </label>
          <label>
            <span>雷达 2m raw</span>
            <input type="number" min={1} max={5000} value={config.radarTwoMeterRaw} onChange={(event) => updateConfig({ radarTwoMeterRaw: Number(event.target.value) })} />
          </label>
          <label>
            <span>雷达 2m gate</span>
            <input type="number" min={0} max={15} value={config.radarTwoMeterGate} onChange={(event) => updateConfig({ radarTwoMeterGate: Number(event.target.value) })} />
          </label>
          <label>
            <span>关门后录音秒数</span>
            <input type="number" min={1} max={6} value={config.autoVoiceRecordSeconds ?? 6} onChange={(event) => updateConfig({ autoVoiceRecordSeconds: Number(event.target.value) })} />
          </label>
          <label>
            <span>关门稳定 ms</span>
            <input type="number" min={800} max={10000} value={config.closeStableMs ?? 2500} onChange={(event) => updateConfig({ closeStableMs: Number(event.target.value) })} />
          </label>
        </div>
        <label className="checkbox-line">
          <input type="checkbox" checked={config.sleepEnabled ?? false} onChange={(event) => updateConfig({ sleepEnabled: event.target.checked })} />
          <span>启用黑屏休眠省电</span>
        </label>
        <label className="checkbox-line">
          <input type="checkbox" checked={config.autoVoiceAfterClose} onChange={(event) => updateConfig({ autoVoiceAfterClose: event.target.checked })} />
          <span>关门稳定后自动开启语音对话</span>
        </label>
        <button className="action-button" type="button" disabled={!connected} onClick={() => void saveStateMachineConfig()}>
          <Check size={16} />
          保存状态机配置
        </button>
      </div>
      <KeyValue title="当前状态机" rows={[
        ["状态", stateMachineStatus?.state ?? "--"],
        ["门状态", stateMachineStatus?.doorState ?? "--"],
        ["昼夜", stateMachineStatus?.isNight ? "夜间" : "白天"],
        ["雷达 2m", stateMachineStatus?.radarWithin2m ? "是" : "否"],
        ["自动语音", stateMachineStatus?.autoVoiceState ?? "--"],
        ["原因", stateMachineStatus?.lastReason ?? "--"],
      ]} />
      <div className="warning-line">
        <Info size={16} />
        修改 Web Serial 超时只影响新建连接；已连接串口会保持当前参数。
      </div>
    </div>
  );
}

function WifiSignal({ strength }: { strength: number }) {
  const bars = strength >= 80 ? 4 : strength >= 60 ? 3 : strength >= 40 ? 2 : 1;
  return (
    <span className="wifi-signal" aria-label={`信号强度 ${strength}%`}>
      {[1, 2, 3, 4].map((bar) => (
        <i key={bar} className={bar <= bars ? "active" : ""} />
      ))}
    </span>
  );
}

function Metric({ label, value }: { label: string; value: string | number }) {
  return (
    <div className="metric">
      <span>{label}</span>
      <strong>{value}</strong>
    </div>
  );
}

function StatusDot({ state }: { state: "ok" | "warn" | "danger" | "offline" }) {
  return <i className={`status-dot ${state}`} aria-hidden="true" />;
}

function StatusPill({ state, children }: { state: "ok" | "warn" | "danger" | "offline"; children: React.ReactNode }) {
  return (
    <span className={`status-pill ${state}`}>
      <StatusDot state={state} />
      {children}
    </span>
  );
}

function SafetyBadge({ level }: { level: "safe" | "caution" | "danger" }) {
  const label = level === "safe" ? "安全" : level === "caution" ? "注意" : "危险";
  return <span className={`safety-badge ${level}`}>{label}</span>;
}

function DataTable({ title, headers, rows, empty }: { title: string; headers: string[]; rows: string[][]; empty: string }) {
  return (
    <div className="data-table">
      <h3>{title}</h3>
      <div className="table-wrap compact-table">
        <table>
          <thead>
            <tr>{headers.map((header) => <th key={header}>{header}</th>)}</tr>
          </thead>
          <tbody>{rows.map((row) => <tr key={row.join("-")}>{row.map((cell) => <td key={cell}>{cell}</td>)}</tr>)}</tbody>
        </table>
        {rows.length === 0 && <p className="empty-state">{empty}</p>}
      </div>
    </div>
  );
}

function KeyValue({ title, rows }: { title: string; rows: [string, string][] }) {
  return (
    <div className="key-value">
      <h3>{title}</h3>
      {rows.map(([key, value]) => (
        <div className="kv-row" key={key}>
          <span>{key}</span>
          <strong>{value}</strong>
        </div>
      ))}
    </div>
  );
}

function GaugeBlock({ label, value, state }: { label: string; value: string; state: "ok" | "warn" | "danger" | "offline" }) {
  return (
    <div className="gauge-block">
      <span>{label}</span>
      <strong>{value}</strong>
      <StatusDot state={state} />
    </div>
  );
}

function LogPreview({ logs }: { logs: DeviceLog[] }) {
  return (
    <div className="log-preview">
      <h3>最近日志</h3>
      {logs.map((log) => (
        <div className={`log-preview-row ${log.level}`} key={log.id}>
          <Terminal size={15} />
          <span>{log.source}</span>
          <p>{log.message}</p>
        </div>
      ))}
      {logs.length === 0 && <p className="empty-state">暂无日志。</p>}
    </div>
  );
}

export default App;
