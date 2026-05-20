import {
  createMockAIConfig,
  createMockDiagnostics,
  createMockLogs,
  createMockNetwork,
  createMockPins,
  createMockSensors,
  createMockStatus,
  createMockWifiNetworks,
} from "../data/mockData";
import type { AIChatResponse, AIConfig, DeviceCommand, DeviceResponse, NetworkConfig } from "../types";
import { BaseTransport } from "./DeviceTransport";

// Mock 传输层：无硬件时模拟 ESP32-S3 返回，便于比赛展示和前端开发。
export class MockTransport extends BaseTransport {
  private timer: number | undefined;
  private connected = false;
  private network = createMockNetwork();
  private aiConfig = createMockAIConfig();

  async connect() {
    this.connected = true;
    this.emitLog("info", "Mock 设备已连接，当前使用模拟数据。", "mock");
    this.timer = window.setInterval(() => {
      this.emitMessage({
        type: "event",
        event: "log",
        payload: {
          level: "debug",
          source: "sensor_task",
          message: "PIR hold=0 lux_delta=12.4 angle_delta=0.8",
        },
      });
    }, 6500);
  }

  async disconnect() {
    this.connected = false;
    if (this.timer) {
      window.clearInterval(this.timer);
    }
    this.emitLog("info", "Mock 设备已断开。", "mock");
  }

  async sendCommand<TPayload = unknown>(
    command: DeviceCommand,
    payload?: unknown,
  ): Promise<DeviceResponse<TPayload>> {
    if (!this.connected) {
      throw new Error("Mock 设备未连接");
    }

    await new Promise((resolve) => window.setTimeout(resolve, 180));
    const requestId = crypto.randomUUID();
    let responsePayload: unknown;

    switch (command) {
      case "get_status":
        responsePayload = createMockStatus();
        break;
      case "get_network":
        responsePayload = this.network;
        break;
      case "scan_wifi":
        responsePayload = createMockWifiNetworks();
        break;
      case "set_network":
        this.network = {
          ...(payload as NetworkConfig),
          connected: true,
          saved: true,
          internet: true,
          status: "mock_online",
          ip: "192.168.1.88",
          rssi: -48,
          lastError: "",
          saveAiKey: false,
        };
        this.emitLog("info", "网络配置已写入 Mock 设备缓存。", "network");
        responsePayload = this.network;
        break;
      case "get_ai_config":
        responsePayload = this.aiConfig;
        break;
      case "set_ai_config": {
        const update = payload as Partial<AIConfig> & { apiKey?: string };
        const apiKey = update.apiKey?.trim();
        this.aiConfig = {
          ...this.aiConfig,
          ...update,
          apiKey: undefined,
          hasApiKey: apiKey ? true : this.aiConfig.hasApiKey,
          apiKeyPreview: apiKey ? `${apiKey.slice(0, 3)}...${apiKey.slice(-4)}` : this.aiConfig.apiKeyPreview,
          ready: Boolean((update.apiBaseUrl ?? this.aiConfig.apiBaseUrl) && (update.model ?? this.aiConfig.model) && (apiKey || this.aiConfig.hasApiKey)),
          lastError: "",
        };
        this.emitLog("info", "AI API 配置已写入 Mock 设备缓存，Key 未回显。", "ai");
        responsePayload = this.aiConfig;
        break;
      }
      case "clear_ai_key":
        this.aiConfig = {
          ...this.aiConfig,
          hasApiKey: false,
          apiKeyPreview: "",
          ready: false,
          lastError: "API Key 已清除",
        };
        this.emitLog("warn", "Mock AI API Key 已清除。", "ai");
        responsePayload = this.aiConfig;
        break;
      case "test_ai_chat": {
        const message = (payload as { message?: string })?.message ?? "";
        if (!this.aiConfig.hasApiKey) {
          throw new Error("Mock AI 缺少 API Key，请先保存一个测试 Key。");
        }
        responsePayload = {
          reply: `Mock 回复：已收到“${message.slice(0, 80)}”。真实设备会通过 OpenAI-compatible /chat/completions 请求。`,
          model: this.aiConfig.model,
          latencyMs: 180,
          status: "mock_ok",
          httpStatus: 200,
        } satisfies AIChatResponse;
        break;
      }
      case "get_pins":
        responsePayload = createMockPins();
        break;
      case "get_sensors":
        responsePayload = createMockSensors();
        break;
      case "get_diagnostics":
        responsePayload = createMockDiagnostics();
        break;
      case "get_logs":
        responsePayload = createMockLogs();
        break;
      default:
        responsePayload = {};
    }

    const response: DeviceResponse<TPayload> = {
      type: "response",
      request_id: requestId,
      ok: true,
      command,
      payload: responsePayload as TPayload,
    };
    this.emitMessage(response);
    return response;
  }
}
