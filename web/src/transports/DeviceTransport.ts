import type {
  DeviceCommand,
  DeviceEvent,
  DeviceMessage,
  DeviceResponse,
  LogLevel,
} from "../types";

export type MessageHandler = (message: DeviceMessage) => void;
export type LogHandler = (level: LogLevel, message: string, source?: string) => void;

export interface DeviceTransport {
  connect(): Promise<void>;
  disconnect(): Promise<void>;
  sendCommand<TPayload = unknown>(
    command: DeviceCommand,
    payload?: unknown,
  ): Promise<DeviceResponse<TPayload>>;
  onMessage(callback: MessageHandler): () => void;
  onLog(callback: LogHandler): () => void;
}

// 轻量事件基类：两种传输层都需要消息和日志订阅。
export abstract class BaseTransport implements DeviceTransport {
  protected messageHandlers = new Set<MessageHandler>();
  protected logHandlers = new Set<LogHandler>();

  abstract connect(): Promise<void>;
  abstract disconnect(): Promise<void>;
  abstract sendCommand<TPayload = unknown>(
    command: DeviceCommand,
    payload?: unknown,
  ): Promise<DeviceResponse<TPayload>>;

  onMessage(callback: MessageHandler) {
    this.messageHandlers.add(callback);
    return () => this.messageHandlers.delete(callback);
  }

  onLog(callback: LogHandler) {
    this.logHandlers.add(callback);
    return () => this.logHandlers.delete(callback);
  }

  protected emitMessage(message: DeviceMessage) {
    for (const handler of this.messageHandlers) {
      try {
        handler(message);
      } catch (error) {
        this.emitLog("warn", `消息处理器异常：${error instanceof Error ? error.message : String(error)}`, "transport");
      }
    }
    if (message.type === "event" && message.event === "log") {
      const payload = message.payload as DeviceEvent["payload"] & {
        level?: LogLevel;
        message?: string;
        source?: string;
      };
      this.emitLog(payload.level ?? "info", payload.message ?? "", payload.source);
    }
  }

  protected emitLog(level: LogLevel, message: string, source = "transport") {
    this.logHandlers.forEach((handler) => handler(level, message, source));
  }
}
