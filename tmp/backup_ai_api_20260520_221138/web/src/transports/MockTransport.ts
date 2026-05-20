import {
  createMockDiagnostics,
  createMockLogs,
  createMockNetwork,
  createMockPins,
  createMockSensors,
  createMockStatus,
  createMockWifiNetworks,
} from "../data/mockData";
import type { DeviceCommand, DeviceResponse, NetworkConfig } from "../types";
import { BaseTransport } from "./DeviceTransport";

// Mock 传输层：无硬件时模拟 ESP32-S3 返回，便于比赛展示和前端开发。
export class MockTransport extends BaseTransport {
  private timer: number | undefined;
  private connected = false;
  private network = createMockNetwork();

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
