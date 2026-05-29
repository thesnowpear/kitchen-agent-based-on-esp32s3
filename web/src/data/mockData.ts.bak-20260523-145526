import type {
  AIConfig,
  ASRConfig,
  DeviceLog,
  DeviceStatus,
  DiagnosticSnapshot,
  NetworkConfig,
  PinInfo,
  SensorSnapshot,
  WifiNetwork,
} from "../types";

const now = () =>
  new Intl.DateTimeFormat("zh-CN", {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  }).format(new Date());

// Mock 状态数据：贴近当前硬件架构，避免前端展示脱离真实约束。
export function createMockStatus(): DeviceStatus {
  return {
    model: "冰箱小精灵 DevKit",
    chip: "ESP32-S3-DevKitC-1 N8R8",
    firmware: "s3-alpha.0.1.0",
    uptime: "02:17:43",
    flash: "8 MB planned / sdkconfig 待修正",
    psram: "8 MB detected",
    freeHeapKb: 184,
    minHeapKb: 132,
    freePsramKb: 6420,
    temperatureC: null,
    wifi: "ok",
    mqtt: "warn",
    usb: "ok",
    ota: "warn",
    page: "IDLE / local summary",
    powerNote: "屏幕 5V 供电与 ESP32-S3 3.3V GPIO 必须隔离确认。",
    tasks: [
      { name: "gui_task", priority: "高", state: "running", heartbeat: "120 ms" },
      { name: "sensor_task", priority: "中高", state: "running", heartbeat: "42 ms" },
      { name: "net_task", priority: "中", state: "reconnect", heartbeat: "3.2 s" },
      { name: "storage_task", priority: "中低", state: "idle", heartbeat: "1.0 s" },
    ],
  };
}

export function createMockNetwork(): NetworkConfig {
  return {
    ssid: "KitchenLab-2.4G",
    wifiPassword: "",
    mqttHost: "mqtt://192.168.1.20:1883",
    apiBaseUrl: "https://api.fridge-spirit.local",
    ntpServer: "ntp.aliyun.com",
    save: true,
    saveAiKey: false,
    connected: true,
    saved: true,
    internet: true,
    status: "mock_online",
    ip: "192.168.1.88",
    rssi: -48,
    lastError: "",
  };
}

export function createMockAIConfig(): AIConfig {
  return {
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
}

export function createMockASRConfig(): ASRConfig {
  return {
    apiBaseUrl: "https://api.siliconflow.cn/v1/audio/transcriptions",
    model: "TeleAI/TeleSpeechASR",
    timeoutMs: 45000,
    hasApiKey: false,
    apiKeyPreview: "",
    lastError: "",
    ready: false,
  };
}

export function createMockWifiNetworks(): WifiNetwork[] {
  return [
    { ssid: "KitchenLab-2.4G", band: "2.4G", signal: 92, rssi: -54, channel: 6, secured: true, authmode: "WPA2", note: "Mock 扫描结果，ESP32 兼容" },
    { ssid: "Home-IoT", band: "2.4G", signal: 78, rssi: -61, channel: 1, secured: true, authmode: "WPA2/WPA3", note: "Mock 扫描结果" },
    { ssid: "Phone-Hotspot", band: "2.4G", signal: 64, rssi: -68, channel: 11, secured: true, authmode: "WPA2", note: "Mock 热点" },
  ];
}

export function createMockPins(): PinInfo[] {
  return [
    { gpio: "GPIO10", signal: "LCD_CS", usage: "屏幕片选", level: "safe", note: "低有效，QSPI 屏幕专用。", readonly: true },
    { gpio: "GPIO12", signal: "LCD_SCLK", usage: "QSPI 时钟", level: "caution", note: "调试阶段从低时钟起步，不直接冲 100MHz。", readonly: true },
    { gpio: "GPIO11", signal: "LCD_D0", usage: "QSPI DATA0", level: "safe", note: "3.3V 逻辑，不接 5V 信号。", readonly: true },
    { gpio: "GPIO13", signal: "LCD_D1", usage: "QSPI DATA1", level: "safe", note: "屏幕数据线方向需核对。", readonly: true },
    { gpio: "GPIO14", signal: "LCD_D2", usage: "QSPI DATA2", level: "safe", note: "屏幕数据线方向需核对。", readonly: true },
    { gpio: "GPIO9", signal: "LCD_D3", usage: "QSPI DATA3", level: "safe", note: "屏幕数据线方向需核对。", readonly: true },
    { gpio: "GPIO7", signal: "LCD_BL", usage: "背光 PWM", level: "caution", note: "确认背光驱动，不让 GPIO 承担超额电流。", readonly: true },
    { gpio: "GPIO4", signal: "I2C_SDA", usage: "触摸/光照/IMU", level: "caution", note: "SDA 上拉到 3.3V，不能上拉到 5V。", readonly: true },
    { gpio: "GPIO5", signal: "I2C_SCL", usage: "触摸/光照/IMU", level: "caution", note: "SCL 上拉到 3.3V，避免总线过长。", readonly: true },
    { gpio: "GPIO1", signal: "LIGHT_AO", usage: "光敏 ADC", level: "safe", note: "AO 模拟输出，只允许 3.3V 供电后接入 ADC1_CH0。", readonly: true },
    { gpio: "GPIO40", signal: "MIC_BCLK", usage: "I2S 麦克风", level: "safe", note: "I2S 时钟线，避免与屏幕高速线过近。", readonly: true },
    { gpio: "GPIO41", signal: "MIC_WS", usage: "I2S 麦克风", level: "safe", note: "INMP441 字选择线，L/R 接 GND 时使用左声道。", readonly: true },
    { gpio: "GPIO42", signal: "MIC_SD", usage: "I2S 麦克风", level: "safe", note: "INMP441 数据输出到 ESP32-S3。", readonly: true },
    { gpio: "GPIO47", signal: "BUZZER", usage: "蜂鸣器 PWM", level: "caution", note: "蜂鸣器建议使用驱动管或限流方案。", readonly: true },
    { gpio: "GPIO0", signal: "BOOT", usage: "启动绑带脚", level: "danger", note: "禁止随意外接，会影响下载/启动模式。", readonly: true },
    { gpio: "GPIO35-37", signal: "PSRAM/Flash", usage: "保留", level: "danger", note: "可能被 Flash/PSRAM 占用，首版不使用。", readonly: true },
    { gpio: "GPIO45/46", signal: "STRAP", usage: "启动敏感", level: "danger", note: "启动绑带相关，未经核对不要连接外设。", readonly: true },
  ];
}

export function createMockSensors(): SensorSnapshot {
  const raw = Math.round(900 + Math.random() * 2400);
  const brightness10 = Math.round(((4095 - raw) * 1023) / 4095);
  return {
    pir: Math.random() > 0.65,
    lux: brightness10,
    lightRaw12bit: raw,
    lightValue10bit: brightness10,
    lightPercent: Math.round((brightness10 * 100) / 1023),
    lightPolarity: "raw_high_dark",
    lightDelta: Number((8 + Math.random() * 22).toFixed(1)),
    angleDelta: Number((1.2 + Math.random() * 3).toFixed(1)),
    vibrationPeak: Number((0.02 + Math.random() * 0.08).toFixed(2)),
    touch: "FT6336U idle / addr 0x38",
    display: "TR230S QSPI planned / line buffer",
    buzzer: "LEDC idle",
    doorState: "IDLE",
    updatedAt: now(),
  };
}

export function createMockDiagnostics(): DiagnosticSnapshot {
  return {
    psram: "OK / 8MB / alloc tested",
    flashPartition: "当前 sdkconfig 仍需调整为 8MB + OTA + LittleFS",
    littlefs: "planned: assets + cache",
    otaSlot: "未启用，等待分区表修正",
    brownoutCount: 0,
    watchdogCount: 0,
    lastError: "无硬件错误，当前为 Mock 数据",
    riskItems: [
      "上电前核对屏幕 VCC 5V 与 GPIO 3.3V 逻辑隔离。",
      "I2C 上拉只允许到 3.3V。",
      "Wi-Fi 峰值、屏幕背光、ESP32-CAM 补光建议使用稳定 5V/2A 电源。",
    ],
  };
}

export function createMockLogs(): DeviceLog[] {
  return [
    { id: crypto.randomUUID(), at: now(), level: "info", source: "boot", message: "Fridge Spirit console mock boot complete" },
    { id: crypto.randomUUID(), at: now(), level: "warn", source: "partition", message: "sdkconfig still reports single app partition" },
    { id: crypto.randomUUID(), at: now(), level: "info", source: "sensor_task", message: "brightness=642 delta=12 angle_delta=2.1 pir=0" },
    { id: crypto.randomUUID(), at: now(), level: "debug", source: "mqtt", message: "heartbeat queued because broker is not connected" },
  ];
}
