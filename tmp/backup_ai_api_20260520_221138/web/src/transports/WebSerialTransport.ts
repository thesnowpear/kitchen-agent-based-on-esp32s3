import type { DeviceCommand, DeviceRequest, DeviceResponse } from "../types";
import { BaseTransport } from "./DeviceTransport";

interface PendingRequest {
  resolve: (value: DeviceResponse) => void;
  reject: (reason?: unknown) => void;
  timeout: number;
}

// Web Serial 传输层：通过 USB CDC/JTAG 串口收发 JSON Lines 协议。
export class WebSerialTransport extends BaseTransport {
  private port: SerialPort | null = null;
  private reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
  private writer: WritableStreamDefaultWriter<Uint8Array> | null = null;
  private pending = new Map<string, PendingRequest>();
  private decoder = new TextDecoder();
  private encoder = new TextEncoder();
  private readBuffer = "";
  private readLoopActive = false;

  constructor(private timeoutMs = 30000) {
    super();
  }

  async connect() {
    if (!navigator.serial) {
      throw new Error("当前浏览器不支持 Web Serial，请使用 Chrome 或 Edge 的 localhost 页面。");
    }
    this.port = await navigator.serial.requestPort();
    await this.port.open({ baudRate: 115200 });

    if (!this.port.readable || !this.port.writable) {
      throw new Error("串口未提供可读写流。");
    }

    this.reader = this.port.readable.getReader();
    this.writer = this.port.writable.getWriter();
    this.readLoopActive = true;
    this.readLoop();
    this.emitLog("info", "USB 串口已连接，波特率 115200。", "serial");
  }

  async disconnect() {
    this.readLoopActive = false;
    this.pending.forEach((pending) => {
      window.clearTimeout(pending.timeout);
      pending.reject(new Error("串口已断开"));
    });
    this.pending.clear();

    await this.reader?.cancel().catch(() => undefined);
    this.reader?.releaseLock();
    this.reader = null;

    this.writer?.releaseLock();
    this.writer = null;

    await this.port?.close().catch(() => undefined);
    this.port = null;
    this.emitLog("info", "USB 串口已断开。", "serial");
  }

  async sendCommand<TPayload = unknown>(
    command: DeviceCommand,
    payload?: unknown,
  ): Promise<DeviceResponse<TPayload>> {
    if (!this.writer) {
      throw new Error("串口未连接");
    }

    const request: DeviceRequest = {
      type: "request",
      request_id: crypto.randomUUID(),
      command,
      payload,
    };
    const line = `${JSON.stringify(request)}\n`;

    const promise = new Promise<DeviceResponse<TPayload>>((resolve, reject) => {
      const timeout = window.setTimeout(() => {
        this.pending.delete(request.request_id);
        reject(new Error(`${command} 请求超时`));
      }, this.timeoutMs);
      this.pending.set(request.request_id, {
        resolve: resolve as (value: DeviceResponse) => void,
        reject,
        timeout,
      });
    });

    await this.writer.write(this.encoder.encode(line));
    this.emitLog("debug", `TX ${command} ${request.request_id}`, "serial");
    return promise;
  }

  private async readLoop() {
    while (this.readLoopActive && this.reader) {
      try {
        const { value, done } = await this.reader.read();
        if (done) {
          break;
        }
        if (value) {
          this.handleChunk(value);
        }
      } catch (error) {
        if (this.readLoopActive) {
          this.emitLog("error", String(error), "serial");
        }
        break;
      }
    }
  }

  private handleChunk(value: Uint8Array) {
    this.readBuffer += this.decoder.decode(value, { stream: true });
    const lines = this.readBuffer.split(/\r?\n/);
    this.readBuffer = lines.pop() ?? "";

    lines
      .map((line) => line.trim())
      .filter(Boolean)
      .forEach((line) => this.handleLine(line));
  }

  private handleLine(line: string) {
    try {
      const message = JSON.parse(line);
      this.emitMessage(message);
      if (message.type === "response" && message.request_id) {
        const pending = this.pending.get(message.request_id);
        if (pending) {
          window.clearTimeout(pending.timeout);
          this.pending.delete(message.request_id);
          pending.resolve(message);
        }
      }
    } catch {
      this.emitLog("info", line, "serial-log");
    }
  }
}

