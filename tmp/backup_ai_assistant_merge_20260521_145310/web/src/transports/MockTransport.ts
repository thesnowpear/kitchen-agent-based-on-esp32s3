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
import type {
  AIChatResponse,
  AIConfig,
  AIContextPreview,
  AIProfilesResponse,
  DeviceCommand,
  DeviceResponse,
  MemorySummary,
  NetworkConfig,
  ProjectAITaskRequest,
  ProjectAITaskResponse,
} from "../types";
import { BaseTransport } from "./DeviceTransport";

// Mock 传输层：无硬件时模拟 ESP32-S3 返回，便于比赛展示和前端开发。
export class MockTransport extends BaseTransport {
  private timer: number | undefined;
  private connected = false;
  private network = createMockNetwork();
  private aiConfig = createMockAIConfig();
  private aiProfiles: AIConfig[] = [this.aiConfig];
  private memorySummary: MemorySummary = {
    schema_version: 1,
    memory_policy: "只保存结构化摘要，不保存完整聊天记录",
    family_size: 2,
    taste: ["清淡", "少油"],
    avoid: ["香菜"],
    allergies: [],
    recent_summary: ["用户希望优先处理临期食材", "早餐偏快手，晚餐偏家常"],
  };

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
      case "get_ai_profiles":
        responsePayload = this.getAiProfilesPayload();
        break;
      case "set_ai_config": {
        const update = payload as Partial<AIConfig> & { apiKey?: string };
        const apiKey = update.apiKey?.trim();
        const profileId = update.profileId ?? this.aiConfig.profileId ?? 0;
        this.aiConfig = {
          ...this.aiConfig,
          ...update,
          profileId,
          profileName: update.profileName?.trim() || this.aiConfig.profileName || "默认配置",
          apiKey: undefined,
          hasApiKey: apiKey ? true : this.aiConfig.hasApiKey,
          apiKeyPreview: apiKey ? `${apiKey.slice(0, 3)}...${apiKey.slice(-4)}` : this.aiConfig.apiKeyPreview,
          ready: Boolean((update.apiBaseUrl ?? this.aiConfig.apiBaseUrl) && (update.model ?? this.aiConfig.model) && (apiKey || this.aiConfig.hasApiKey)),
          lastError: "",
        };
        this.aiProfiles = this.aiProfiles.filter((item) => item.profileId !== profileId).concat(this.aiConfig).sort((a, b) => (a.profileId ?? 0) - (b.profileId ?? 0));
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
        this.aiProfiles = this.aiProfiles.map((item) => (item.profileId === this.aiConfig.profileId ? this.aiConfig : item));
        this.emitLog("warn", "Mock AI API Key 已清除。", "ai");
        responsePayload = this.aiConfig;
        break;
      case "create_ai_profile": {
        const nextId = Math.max(...this.aiProfiles.map((item) => item.profileId ?? 0)) + 1;
        this.aiConfig = {
          ...createMockAIConfig(),
          profileId: nextId,
          profileName: (payload as { profileName?: string })?.profileName || `备用配置 ${nextId}`,
          apiBaseUrl: "",
          hasApiKey: false,
          apiKeyPreview: "",
          ready: false,
        };
        this.aiProfiles = [...this.aiProfiles, this.aiConfig];
        responsePayload = this.aiConfig;
        break;
      }
      case "select_ai_profile": {
        const profileId = (payload as { profileId?: number })?.profileId ?? 0;
        const found = this.aiProfiles.find((item) => item.profileId === profileId);
        if (!found) {
          throw new Error("Mock AI 配置不存在");
        }
        this.aiConfig = found;
        responsePayload = this.aiConfig;
        break;
      }
      case "delete_ai_profile": {
        const profileId = (payload as { profileId?: number })?.profileId ?? 0;
        if (profileId === 0) {
          throw new Error("默认配置不能删除");
        }
        this.aiProfiles = this.aiProfiles.filter((item) => item.profileId !== profileId);
        this.aiConfig = this.aiProfiles[0] ?? createMockAIConfig();
        responsePayload = this.aiConfig;
        break;
      }
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
      case "get_ai_context_preview": {
        const request = this.normalizeProjectAiRequest(payload);
        responsePayload = this.createContextPreview(request);
        break;
      }
      case "test_ai_task": {
        const request = this.normalizeProjectAiRequest(payload);
        responsePayload = this.createProjectAiResult(request);
        this.emitLog("info", `Mock 项目 AI 任务完成：${request.taskType}`, "ai_context");
        break;
      }
      case "get_memory_summary":
        responsePayload = this.memorySummary;
        break;
      case "clear_memory_summary":
        this.memorySummary = {
          schema_version: 1,
          memory_policy: "已清空结构化记忆摘要，不保存完整聊天记录",
          family_size: 0,
          taste: [],
          avoid: [],
          allergies: [],
          recent_summary: [],
        };
        responsePayload = this.memorySummary;
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

  private getAiProfilesPayload(): AIProfilesResponse {
    return {
      activeProfileId: this.aiConfig.profileId ?? 0,
      profiles: this.aiProfiles,
    };
  }

  private normalizeProjectAiRequest(payload: unknown): ProjectAITaskRequest {
    const request = payload as Partial<ProjectAITaskRequest> | undefined;
    return {
      taskType: request?.taskType ?? "chat_assist",
      userText: request?.userText ?? "",
      includeInventory: request?.includeInventory ?? true,
      includeMemory: request?.includeMemory ?? true,
      includeReminders: request?.includeReminders ?? true,
      includePreferences: request?.includePreferences ?? true,
    };
  }

  private createContextPreview(request: ProjectAITaskRequest): AIContextPreview {
    return {
      taskType: request.taskType,
      localSnapshotVersion: 1,
      needsConfirmation: request.taskType !== "chat_assist",
      context: {
        schema_version: 1,
        task_type: request.taskType,
        request_id: "mock_preview",
        persona_template: "你是冰箱小精灵的智能厨房助手。回答要实用、保守、可确认、少打扰。",
        task_template: "按任务类型输出结构化 JSON，AI 结果不能直接入库。",
        user_text: request.userText,
        dynamic_context: {
          inventory: request.includeInventory ? { items: [{ name: "牛奶", days_left: 1, location: "门架中层" }, { name: "番茄", days_left: 2, location: "冷藏主仓 B2" }] } : null,
          reminders: request.includeReminders ? { reminders: [{ name: "牛奶", suggestion: "今天优先处理" }] } : null,
          preferences: request.includePreferences ? { people: 2, taste: ["清淡", "少油"], avoid: ["香菜"] } : null,
          memory_summary: request.includeMemory ? this.memorySummary : null,
          offline_queue: { pending_count: 0 },
        },
        output_policy: {
          ai_result_must_be_confirmed: true,
          do_not_fabricate_inventory: true,
          do_not_store_full_chat: true,
        },
      },
    };
  }

  private createProjectAiResult(request: ProjectAITaskRequest): ProjectAITaskResponse {
    const base = {
      taskType: request.taskType,
      confidence: 82,
      needsConfirmation: request.taskType !== "chat_assist",
      safetyNote: "Mock 结果只验证上下文注入和结构化输出，不会直接写入库存。",
    };
    if (request.taskType === "recipe_generate") {
      return {
        ...base,
        needsConfirmation: false,
        result: {
          schema_version: 1,
          type: "recipe_generate",
          recipe: {
            name: "番茄鸡蛋汤",
            use_inventory: ["番茄", "鸡蛋"],
            missing: ["葱花，可选"],
            time_minutes: 15,
            steps: ["番茄切块，鸡蛋打散", "少油炒番茄出汁", "加水煮开后淋入蛋液", "按口味少量加盐"],
          },
        },
      };
    }
    if (request.taskType === "shopping_list_generate") {
      return {
        ...base,
        result: {
          schema_version: 1,
          type: "shopping_list_generate",
          suggested: ["绿叶菜", "面条"],
          optional: ["葱花", "低脂酸奶"],
          basis: "根据当前库存和快手晚餐偏好生成，避免过量购买",
        },
      };
    }
    if (request.taskType === "recognize_ingredients") {
      return {
        ...base,
        result: {
          schema_version: 1,
          type: "recognize_ingredients",
          candidates: [{ name: "番茄", quantity: "约2-3个", confidence: 0.82, doubt: "Mock 未接入真实图片" }],
          needs_confirmation: true,
          confirm_fields: ["名称", "数量", "保质期", "位置"],
        },
      };
    }
    return {
      ...base,
      needsConfirmation: request.taskType !== "chat_assist",
      result: {
        schema_version: 1,
        type: request.taskType,
        reply: "我会优先依据当前库存、临期提醒和你的偏好给出建议；信息不足时会请你确认，不会编造库存。",
      },
    };
  }
}
