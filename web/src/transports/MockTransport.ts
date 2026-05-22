import {
  createMockAIConfig,
  createMockASRConfig,
  createMockDiagnostics,
  createMockLogs,
  createMockNetwork,
  createMockPins,
  createMockSensors,
  createMockStatus,
  createMockWifiNetworks,
} from "../data/mockData";
import type {
  AIAssistantChatRequest,
  AIAssistantChatResponse,
  AIChatResponse,
  AIConfig,
  AIContextPreview,
  AIProfilesResponse,
  ASRConfig,
  DeviceChatHistory,
  DeviceCommand,
  DeviceResponse,
  MemorySummary,
  NetworkConfig,
  ProjectAITaskRequest,
  ProjectAITaskResponse,
  VoiceChatResponse,
  VoiceChatStatus,
} from "../types";
import { BaseTransport } from "./DeviceTransport";

export class MockTransport extends BaseTransport {
  private timer: number | undefined;
  private connected = false;
  private network = createMockNetwork();
  private aiConfig = createMockAIConfig();
  private aiProfiles: AIConfig[] = [this.aiConfig];
  private asrConfig: ASRConfig = createMockASRConfig();
  private voiceStatus: VoiceChatStatus = {
    state: "idle",
    durationMs: 0,
    pcmBytes: 0,
    rms: 0,
    error: "",
  };
  private memorySummary: MemorySummary = {
    schema_version: 1,
    memory_policy: "硬件测试记忆：只保存结构化摘要，不保存完整聊天记录",
    family_size: 2,
    taste: ["清淡", "少油"],
    avoid: ["香菜"],
    allergies: [],
    recent_summary: ["用户希望优先处理临期食材", "早餐偏快手，晚餐偏家常"],
  };
  private chatHistory: DeviceChatHistory = {
    schemaVersion: 1,
    updatedAt: Math.floor(Date.now() / 1000),
    ttlSeconds: 48 * 60 * 60,
    maxMessages: 15,
    timeReady: true,
    count: 0,
    prunedCount: 0,
    messages: [],
  };

  private withVoiceDiagnostics(status: VoiceChatStatus): VoiceChatStatus {
    const sampleCount = Math.floor(status.pcmBytes / 2);
    const peakAbs = Math.max(status.rms * 3, status.rms ? 500 : 0);
    const clipCount = peakAbs > 32000 ? 4 : 0;
    return {
      ...status,
      sampleCount,
      peakAbs,
      minSample: -peakAbs,
      maxSample: peakAbs,
      meanSample: Math.round(status.rms / 18),
      clipCount,
      timeoutCount: 0,
      qualityHint: sampleCount < 8000 ? "too_short" : clipCount > 0 ? "clipping" : status.rms < 80 ? "silent" : "ok",
    };
  }

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
          message: "PIR hold=0 brightness_delta=12.4 angle_delta=0.8",
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

  async sendCommand<TPayload = unknown>(command: DeviceCommand, payload?: unknown): Promise<DeviceResponse<TPayload>> {
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
        this.aiProfiles = this.aiProfiles
          .filter((item) => item.profileId !== profileId)
          .concat(this.aiConfig)
          .sort((a, b) => (a.profileId ?? 0) - (b.profileId ?? 0));
        this.emitLog("info", "AI API 配置已写入 Mock 设备缓存，Key 不会回显。", "ai");
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
      case "get_asr_config":
        responsePayload = this.asrConfig;
        break;
      case "set_asr_config": {
        const update = payload as Partial<ASRConfig> & { apiKey?: string };
        const apiKey = update.apiKey?.trim();
        this.asrConfig = {
          ...this.asrConfig,
          ...update,
          apiKey: undefined,
          apiBaseUrl: update.apiBaseUrl?.trim() || this.asrConfig.apiBaseUrl,
          model: update.model?.trim() || this.asrConfig.model,
          hasApiKey: apiKey ? true : this.asrConfig.hasApiKey,
          apiKeyPreview: apiKey ? `${apiKey.slice(0, 3)}...${apiKey.slice(-4)}` : this.asrConfig.apiKeyPreview,
          ready: Boolean((update.apiBaseUrl ?? this.asrConfig.apiBaseUrl) && (update.model ?? this.asrConfig.model) && (apiKey || this.asrConfig.hasApiKey)),
          lastError: "",
        };
        this.emitLog("info", "Mock ASR 配置已保存，Key 不会回显。", "asr");
        responsePayload = this.asrConfig;
        break;
      }
      case "clear_asr_key":
        this.asrConfig = {
          ...this.asrConfig,
          hasApiKey: false,
          apiKeyPreview: "",
          ready: false,
          lastError: "ASR Key 已清除",
        };
        responsePayload = this.asrConfig;
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
      case "ai_assistant_chat": {
        const request = this.normalizeAssistantRequest(payload);
        if (!this.aiConfig.hasApiKey) {
          throw new Error("Mock AI 缺少 API Key，请先保存一个测试 Key。");
        }
        const reply = this.createAssistantReply(request);
        const historyCount = this.chatHistory.messages.length;
        const historyPrunedCount = this.appendMockChatHistory(request, reply);
        responsePayload = {
          reply,
          model: this.aiConfig.model,
          latencyMs: 260,
          status: "mock_ok",
          httpStatus: 200,
          taskType: request.taskType,
          contextInjected: request.includeInventory || request.includeMemory || request.includeReminders || request.includePreferences,
          localSnapshotVersion: 1,
          needsConfirmation: request.taskType !== "chat_assist",
          historyInjected: historyCount > 0,
          historyCount: Math.min(historyCount, this.chatHistory.maxMessages),
          historyPersisted: true,
          historyPrunedCount,
        } satisfies AIAssistantChatResponse;
        this.emitLog("info", `Mock AI 助手已注入上下文并完成：${request.taskType}`, "ai");
        break;
      }
      case "mic_record_start":
      case "voice_chat_start":
        this.voiceStatus = this.withVoiceDiagnostics({
          state: "recording",
          durationMs: 0,
          pcmBytes: 0,
          rms: 0,
          error: "",
        });
        this.emitLog("info", "Mock voice recording started.", "asr");
        responsePayload = this.voiceStatus;
        break;
      case "mic_record_status":
      case "voice_chat_status":
        if (this.voiceStatus.state === "recording") {
          this.voiceStatus = this.withVoiceDiagnostics({
            ...this.voiceStatus,
            durationMs: Math.min(this.voiceStatus.durationMs + 500, 6000),
            pcmBytes: this.voiceStatus.pcmBytes + 16000,
            rms: Math.round(300 + Math.random() * 1200),
          });
        }
        responsePayload = this.voiceStatus;
        break;
      case "mic_record_stop":
        this.voiceStatus = this.withVoiceDiagnostics({
          ...this.voiceStatus,
          state: "ready",
          durationMs: Math.max(this.voiceStatus.durationMs, 500),
        });
        this.emitLog("info", "Mock microphone recording stopped without ASR.", "asr");
        responsePayload = this.voiceStatus;
        break;
      case "voice_chat_stop": {
        if (!this.asrConfig.hasApiKey) {
          throw new Error("Mock ASR 缺少 API Key，请先保存 ASR Key。");
        }
        if (!this.aiConfig.hasApiKey) {
          throw new Error("Mock AI 缺少 API Key，请先保存 AI Key。");
        }
        this.voiceStatus = this.withVoiceDiagnostics({
          ...this.voiceStatus,
          state: "ready",
          durationMs: Math.max(this.voiceStatus.durationMs, 1800),
          pcmBytes: Math.max(this.voiceStatus.pcmBytes, 57600),
          rms: Math.max(this.voiceStatus.rms, 620),
        });
        const transcript = "帮我看看今晚可以吃什么";
        const request: AIAssistantChatRequest = {
          ...this.normalizeProjectAiRequest({ taskType: "voice_intent_parse", userText: transcript }),
          taskType: "voice_intent_parse",
          message: transcript,
          userText: transcript,
        };
        const reply = this.createAssistantReply(request);
        this.appendMockChatHistory(request, reply);
        responsePayload = {
          transcript,
          reply,
          asrModel: this.asrConfig.model,
          aiModel: this.aiConfig.model,
          asrLatencyMs: 680,
          aiLatencyMs: 930,
          asrHttpStatus: 200,
          aiHttpStatus: 200,
          audioBytes: this.voiceStatus.pcmBytes + 44,
          historyPrunedCount: 0,
        } satisfies VoiceChatResponse;
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
      case "set_memory_summary": {
        const update = payload as { memory?: MemorySummary } | MemorySummary | undefined;
        const next = update && "memory" in update ? update.memory : update;
        this.memorySummary = {
          schema_version: 1,
          memory_policy: "硬件测试记忆：只保存结构化摘要，不保存完整聊天记录",
          family_size: 2,
          taste: [],
          avoid: [],
          allergies: [],
          recent_summary: [],
          ...(next as MemorySummary),
        };
        responsePayload = this.memorySummary;
        this.emitLog("info", "Mock 硬件测试记忆已写入。", "ai_context");
        break;
      }
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
      case "get_chat_history":
        responsePayload = this.chatHistory;
        break;
      case "clear_chat_history":
        this.chatHistory = {
          ...this.chatHistory,
          updatedAt: Math.floor(Date.now() / 1000),
          count: 0,
          prunedCount: 0,
          messages: [],
        };
        responsePayload = this.chatHistory;
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

  private normalizeAssistantRequest(payload: unknown): AIAssistantChatRequest {
    const request = payload as Partial<AIAssistantChatRequest> | undefined;
    const base = this.normalizeProjectAiRequest(payload);
    return {
      ...base,
      message: request?.message ?? request?.userText ?? "",
      userText: request?.userText ?? request?.message ?? "",
    };
  }

  private appendMockChatHistory(request: AIAssistantChatRequest, reply: string) {
    const now = Math.floor(Date.now() / 1000);
    const cutoff = now - this.chatHistory.ttlSeconds;
    const merged = [
      ...this.chatHistory.messages,
      {
        id: crypto.randomUUID(),
        role: "user" as const,
        content: request.message.slice(0, 512),
        taskType: request.taskType,
        createdAt: now,
      },
      {
        id: crypto.randomUUID(),
        role: "assistant" as const,
        content: reply.slice(0, 2048),
        taskType: request.taskType,
        createdAt: now,
      },
    ]
      .filter((item) => item.createdAt >= cutoff)
      .slice(-this.chatHistory.maxMessages);
    const prunedCount = Math.max(0, this.chatHistory.messages.length + 2 - merged.length);
    this.chatHistory = {
      ...this.chatHistory,
      updatedAt: now,
      count: merged.length,
      prunedCount,
      messages: merged,
    };
    return prunedCount;
  }

  private createAssistantReply(request: AIAssistantChatRequest) {
    const contextParts = [
      request.includeInventory ? "库存" : "",
      request.includeReminders ? "临期提醒" : "",
      request.includePreferences ? "偏好" : "",
      request.includeMemory ? "测试记忆" : "",
    ].filter(Boolean);
    if (request.taskType === "recipe_generate") {
      return `Mock 真实助手：我已注入${contextParts.join("、") || "无额外上下文"}。按当前测试库存，建议先处理牛奶和番茄，可以做番茄鸡蛋汤；如果牛奶有异味、胀包或冷链异常，请不要食用。`;
    }
    if (request.taskType === "shopping_list_generate") {
      return `Mock 真实助手：根据已注入的${contextParts.join("、") || "上下文"}，建议购买绿叶菜和主食，葱花、酸奶属于可选补充，避免一次买太多。`;
    }
    return `Mock 真实助手：已收到“${request.message.slice(0, 80)}”。我会优先依据项目上下文回答，不会编造库存或把结果直接入库。`;
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
          conversation_history: this.chatHistory,
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
      safetyNote: "Mock 结果仅验证上下文注入和结构化输出，不会直接写入库存。",
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
