import {
  Activity,
  Bot,
  Cable,
  Check,
  Cpu,
  Download,
  Info,
  KeyRound,
  LayoutDashboard,
  LockKeyhole,
  Play,
  Power,
  Radio,
  RotateCw,
  Search,
  Send,
  Settings,
  ShieldAlert,
  SlidersHorizontal,
  Terminal,
  Trash2,
  Unplug,
  Wifi,
} from "lucide-react";
import { FormEvent, useCallback, useEffect, useMemo, useRef, useState } from "react";
import { MockTransport } from "./transports/MockTransport";
import { WebSerialTransport } from "./transports/WebSerialTransport";
import type {
  AIChatMessage,
  AIChatResponse,
  AIConfig,
  ConnectionState,
  DeviceLog,
  DeviceStatus,
  DiagnosticSnapshot,
  LogLevel,
  NetworkConfig,
  PinInfo,
  SectionDefinition,
  SensorSnapshot,
  TransportMode,
  WifiNetwork,
} from "./types";

const sections: SectionDefinition[] = [
  { id: "overview", label: "总览", icon: LayoutDashboard },
  { id: "usb", label: "USB 连接", icon: Cable },
  { id: "network", label: "网络配置", icon: Wifi },
  { id: "ai", label: "AI API", icon: Bot },
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
  apiBaseUrl: "https://api.openai.com/v1",
  model: "gpt-4o-mini",
  systemPrompt: "你是冰箱小精灵的开发测试助手，请用简短中文回答。",
  timeoutMs: 30000,
  hasApiKey: false,
  apiKeyPreview: "",
  lastError: "",
  ready: false,
};

const nowTime = () =>
  new Intl.DateTimeFormat("zh-CN", {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  }).format(new Date());

function App() {
  const [activeSection, setActiveSection] = useState<SectionDefinition["id"]>("overview");
  const [transportMode, setTransportMode] = useState<TransportMode>("mock");
  const [connection, setConnection] = useState<ConnectionState>("disconnected");
  const [timeoutMs, setTimeoutMs] = useState(45000);
  const [refreshSeconds, setRefreshSeconds] = useState(5);
  const [status, setStatus] = useState<DeviceStatus | null>(null);
  const [network, setNetwork] = useState<NetworkConfig | null>(null);
  const [aiConfig, setAiConfig] = useState<AIConfig | null>(null);
  const [aiKeyInput, setAiKeyInput] = useState("");
  const [aiChatDraft, setAiChatDraft] = useState("");
  const [aiMessages, setAiMessages] = useState<AIChatMessage[]>([]);
  const [aiBusy, setAiBusy] = useState(false);
  const [wifiNetworks, setWifiNetworks] = useState<WifiNetwork[]>([]);
  const [wifiScanState, setWifiScanState] = useState<"idle" | "scanning" | "done" | "error">("idle");
  const [pins, setPins] = useState<PinInfo[]>([]);
  const [sensors, setSensors] = useState<SensorSnapshot | null>(null);
  const [diagnostics, setDiagnostics] = useState<DiagnosticSnapshot | null>(null);
  const [logs, setLogs] = useState<DeviceLog[]>([]);
  const [logFilter, setLogFilter] = useState<LogLevel | "all">("all");
  const [searchTerm, setSearchTerm] = useState("");
  const [busy, setBusy] = useState(false);
  const transportRef = useRef<MockTransport | WebSerialTransport | null>(null);

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

  const createTransport = useCallback(() => {
    return transportMode === "mock" ? new MockTransport() : new WebSerialTransport(timeoutMs);
  }, [timeoutMs, transportMode]);

  const clearFrontendData = useCallback(() => {
    setStatus(null);
    setNetwork(null);
    setAiConfig(null);
    setAiKeyInput("");
    setAiChatDraft("");
    setAiMessages([]);
    setAiBusy(false);
    setWifiNetworks([]);
    setWifiScanState("idle");
    setPins([]);
    setSensors(null);
    setDiagnostics(null);
    setLogs([]);
    setBusy(false);
    setSearchTerm("");
    setLogFilter("all");
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

  const refreshAll = useCallback(async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      return;
    }
    setBusy(true);
    try {
      const [statusRes, networkRes, aiRes, pinsRes, sensorsRes, diagnosticsRes, logsRes] =
        await Promise.all([
          transport.sendCommand<DeviceStatus>("get_status"),
          transport.sendCommand<NetworkConfig>("get_network"),
          transport.sendCommand<AIConfig>("get_ai_config"),
          transport.sendCommand<PinInfo[]>("get_pins"),
          transport.sendCommand<SensorSnapshot>("get_sensors"),
          transport.sendCommand<DiagnosticSnapshot>("get_diagnostics"),
          transport.sendCommand<DeviceLog[]>("get_logs"),
        ]);
      setStatus(statusRes.payload);
      setNetwork(networkRes.payload);
      setAiConfig(aiRes.payload);
      setPins(pinsRes.payload);
      setSensors(sensorsRes.payload);
      setDiagnostics(diagnosticsRes.payload);
      setLogs((current) => [...logsRes.payload, ...current].slice(0, 180));
    } catch (error) {
      appendLog("error", String(error), "web");
    } finally {
      setBusy(false);
    }
  }, [appendLog, connection]);

  const connect = async () => {
    if (connection !== "disconnected") {
      return;
    }
    setConnection("connecting");
    const transport = createTransport();
    transport.onLog(appendLog);
    transportRef.current = transport;

    try {
      await transport.connect();
      setConnection("connected");
      appendLog("info", transportMode === "mock" ? "已进入 Mock 运维模式。" : "已连接 USB 串口。");
    } catch (error) {
      transportRef.current = null;
      setConnection("disconnected");
      appendLog("error", String(error), "web");
    }
  };

  const disconnect = async () => {
    await transportRef.current?.disconnect();
    transportRef.current = null;
    setConnection("disconnected");
  };

  const saveNetwork = async (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    if (!network || !transportRef.current) {
      return;
    }
    try {
      const response = await transportRef.current.sendCommand<NetworkConfig>("set_network", {
        ...network,
        save: true,
      });
      setNetwork(response.payload);
      appendLog("info", "网络配置已发送，设备将保存 Wi-Fi 凭据并尝试联网。", "network");
      await refreshAll();
    } catch (error) {
      appendLog("error", String(error), "network");
    }
  };

  const scanWifi = async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接 Mock 或 USB 设备，再扫描 Wi-Fi。", "network");
      return;
    }
    setWifiScanState("scanning");
    try {
      const response = await transport.sendCommand<WifiNetwork[]>("scan_wifi");
      setWifiNetworks(response.payload);
      setWifiScanState("done");
      appendLog("info", `扫描完成，发现 ${response.payload.length} 个 Wi-Fi。`, "network");
    } catch (error) {
      setWifiNetworks([]);
      setWifiScanState("error");
      appendLog("error", String(error), "network");
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

    try {
      const response = await transport.sendCommand<AIConfig>("set_ai_config", {
        apiBaseUrl: current.apiBaseUrl.trim(),
        apiKey: aiKeyInput.trim(),
        model: current.model.trim(),
        systemPrompt: current.systemPrompt.trim(),
        timeoutMs: current.timeoutMs,
      });
      setAiConfig(response.payload);
      setAiKeyInput("");
      appendLog("info", "AI API 配置已保存，API Key 不会从设备回显。", "ai");
    } catch (error) {
      appendLog("error", String(error), "ai");
    }
  };

  const clearAiKey = async () => {
    const transport = transportRef.current;
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接 Mock 或 USB 设备，再清除 API Key。", "ai");
      return;
    }

    try {
      const response = await transport.sendCommand<AIConfig>("clear_ai_key");
      setAiConfig(response.payload);
      setAiKeyInput("");
      appendLog("warn", "设备本地 API Key 已清除。", "ai");
    } catch (error) {
      appendLog("error", String(error), "ai");
    }
  };

  const sendAiChat = async (event?: FormEvent<HTMLFormElement>) => {
    event?.preventDefault();
    const transport = transportRef.current;
    const message = aiChatDraft.trim().slice(0, 360);
    if (!message) {
      return;
    }
    if (!transport || connection !== "connected") {
      appendLog("warn", "请先连接 Mock 或 USB 设备，再测试 AI 聊天。", "ai");
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
      content: "等待设备请求 AI API...",
      at: nowTime(),
      status: "pending",
    };

    setAiMessages((current) => [...current, userMessage, pendingMessage].slice(-18));
    setAiChatDraft("");
    setAiBusy(true);
    try {
      const response = await transport.sendCommand<AIChatResponse>("test_ai_chat", { message });
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
      appendLog("info", `AI 聊天测试完成，模型 ${response.payload.model}，耗时 ${response.payload.latencyMs} ms。`, "ai");
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
      setAiBusy(false);
    }
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
      refreshAll();
    }
  }, [connection, refreshAll]);

  useEffect(() => {
    if (connection !== "connected") {
      return undefined;
    }
    const timer = window.setInterval(refreshAll, refreshSeconds * 1000);
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
            <button className="icon-button" onClick={refreshAll} disabled={connection !== "connected" || busy} title="刷新">
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

        <div className="content-grid">
          <section className="primary-panel">{renderSection()}</section>
          <aside className="context-panel">
            <h2>硬件风险</h2>
            <p>{status?.powerNote ?? "连接设备后显示供电、GPIO、启动绑带脚与日志风险。"}</p>
            <div className="risk-list">
              {(diagnostics?.riskItems ?? [
                "屏幕 VCC 为 5V，GPIO 逻辑为 3.3V。",
                "GPIO0、GPIO35-37、GPIO45/46 不作为普通外设脚使用。",
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
            wifiNetworks={wifiNetworks}
            wifiScanState={wifiScanState}
            setNetwork={setNetwork}
            saveNetwork={saveNetwork}
            scanWifi={scanWifi}
          />
        );
      case "ai":
        return (
          <AiPanel
            connection={connection}
            aiConfig={aiConfig}
            setAiConfig={setAiConfig}
            apiKeyInput={aiKeyInput}
            setApiKeyInput={setAiKeyInput}
            messages={aiMessages}
            draft={aiChatDraft}
            setDraft={setAiChatDraft}
            busy={aiBusy}
            saveAiConfig={saveAiConfig}
            clearAiKey={clearAiKey}
            sendAiChat={sendAiChat}
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
          ["光照", sensors ? `${sensors.lux} lux` : "--"],
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
  onConnect,
  onDisconnect,
}: {
  mode: TransportMode;
  connection: ConnectionState;
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
  wifiNetworks,
  wifiScanState,
  setNetwork,
  saveNetwork,
  scanWifi,
}: {
  mode: TransportMode;
  connection: ConnectionState;
  network: NetworkConfig | null;
  wifiNetworks: WifiNetwork[];
  wifiScanState: "idle" | "scanning" | "done" | "error";
  setNetwork: (network: NetworkConfig) => void;
  saveNetwork: (event: FormEvent<HTMLFormElement>) => void;
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

  return (
    <form className="section-flow" onSubmit={saveNetwork}>
      <div className="section-heading">
        <div>
          <p className="eyebrow">Wi-Fi 快速连接</p>
          <h2>选择网络并连接</h2>
        </div>
        <div className="button-row compact">
          <button className="action-button secondary" type="button" onClick={() => void scanWifi()} disabled={connection !== "connected" || scanBusy}>
            <RotateCw size={17} className={scanBusy ? "spin" : ""} />
            扫描
          </button>
          <button className="action-button" type="submit">
            <Wifi size={17} />
            连接
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
        像手机联网一样先选 Wi-Fi、输密码、点连接。AI API 地址和 Key 请到 AI API 页面单独配置。
      </div>
    </form>
  );
}

function AiPanel({
  connection,
  aiConfig,
  setAiConfig,
  apiKeyInput,
  setApiKeyInput,
  messages,
  draft,
  setDraft,
  busy,
  saveAiConfig,
  clearAiKey,
  sendAiChat,
}: {
  connection: ConnectionState;
  aiConfig: AIConfig | null;
  setAiConfig: (config: AIConfig) => void;
  apiKeyInput: string;
  setApiKeyInput: (value: string) => void;
  messages: AIChatMessage[];
  draft: string;
  setDraft: (value: string) => void;
  busy: boolean;
  saveAiConfig: (event: FormEvent<HTMLFormElement>) => void;
  clearAiKey: () => Promise<void>;
  sendAiChat: (event?: FormEvent<HTMLFormElement>) => Promise<void>;
}) {
  const current = aiConfig ?? defaultAiConfig;
  const update = (patch: Partial<AIConfig>) => {
    setAiConfig({ ...current, ...patch });
  };
  const connected = connection === "connected";

  return (
    <div className="section-flow">
      <div className="section-heading">
        <div>
          <p className="eyebrow">OpenAI-compatible</p>
          <h2>AI API 配置与测试</h2>
        </div>
        <StatusPill state={current.ready ? "ok" : current.hasApiKey ? "warn" : "offline"}>
          {current.ready ? "可测试" : current.hasApiKey ? "待补配置" : "未保存 Key"}
        </StatusPill>
      </div>

      <form className="ai-config-grid" onSubmit={saveAiConfig}>
        <div className="ai-config-main">
          <div className="form-grid">
            <label>
              <span>API Base URL</span>
              <input
                value={current.apiBaseUrl}
                placeholder="https://api.openai.com/v1"
                inputMode="url"
                required
                onChange={(event) => update({ apiBaseUrl: event.target.value })}
              />
            </label>
            <label>
              <span>模型名</span>
              <input
                value={current.model}
                placeholder="gpt-4o-mini"
                required
                onChange={(event) => update({ model: event.target.value })}
              />
            </label>
            <label>
              <span>API Key</span>
              <input
                type="password"
                value={apiKeyInput}
                placeholder={current.hasApiKey ? `保持现有 Key：${current.apiKeyPreview || "已保存"}` : "保存到开发板 NVS"}
                maxLength={256}
                autoComplete="new-password"
                onChange={(event) => setApiKeyInput(event.target.value)}
              />
            </label>
            <label>
              <span>请求超时 ms</span>
              <input
                type="number"
                min={5000}
                max={45000}
                value={current.timeoutMs}
                onChange={(event) => update({ timeoutMs: Number(event.target.value) })}
              />
            </label>
          </div>
          <label>
            <span>系统提示词</span>
            <textarea
              value={current.systemPrompt}
              maxLength={240}
              rows={3}
              onChange={(event) => update({ systemPrompt: event.target.value })}
            />
          </label>
          <div className="button-row">
            <button className="action-button" type="submit" disabled={!connected}>
              <KeyRound size={17} />
              保存 AI 配置
            </button>
            <button className="action-button secondary" type="button" disabled={!connected || !current.hasApiKey} onClick={() => void clearAiKey()}>
              <Trash2 size={17} />
              清除 Key
            </button>
          </div>
        </div>

        <aside className="ai-config-side">
          <KeyValue title="设备端状态" rows={[
            ["连接", connected ? "已连接" : "未连接"],
            ["Key", current.hasApiKey ? current.apiKeyPreview || "已保存" : "未保存"],
            ["接口", current.apiBaseUrl || "--"],
            ["最近错误", current.lastError || "无"],
          ]} />
          <div className="warning-line">
            <ShieldAlert size={16} />
            开发模式会把 API Key 写入开发板 NVS；串口响应、日志和导出日志不会回显明文。
          </div>
        </aside>
      </form>

      <form className="ai-chat-panel" onSubmit={(event) => void sendAiChat(event)}>
        <div className="ai-chat-head">
          <div>
            <h3>模拟聊天测试</h3>
            <p>发送一条短消息，设备会调用 OpenAI-compatible /chat/completions。</p>
          </div>
          <StatusPill state={busy ? "warn" : current.ready ? "ok" : "offline"}>{busy ? "请求中" : current.ready ? "就绪" : "待配置"}</StatusPill>
        </div>
        <div className="ai-chat-stream">
          {messages.map((message) => (
            <div className={`ai-message ${message.role} ${message.status ?? "ok"}`} key={message.id}>
              <span>{message.role === "user" ? "我" : "AI"}</span>
              <p>{message.content}</p>
              <small>{message.at}{message.latencyMs ? ` / ${message.latencyMs} ms` : ""}</small>
            </div>
          ))}
          {messages.length === 0 && (
            <div className="ai-chat-empty">
              <Bot size={22} />
              <strong>还没有测试消息</strong>
              <span>保存配置后可以发一句“请用一句话介绍冰箱小精灵”。</span>
            </div>
          )}
        </div>
        <div className="ai-chat-input">
          <textarea
            value={draft}
            maxLength={360}
            rows={2}
            placeholder="输入一条测试消息，最多 360 字符"
            onChange={(event) => setDraft(event.target.value)}
          />
          <button className="action-button" type="submit" disabled={!connected || busy || draft.trim().length === 0}>
            <Send size={17} />
            发送
          </button>
        </div>
      </form>
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

function SensorsPanel({ sensors }: { sensors: SensorSnapshot | null }) {
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
        <GaugeBlock label="PIR" value={sensors?.pir ? "触发" : "未触发"} state={sensors?.pir ? "warn" : "ok"} />
        <GaugeBlock label="光照" value={sensors ? `${sensors.lux} lux` : "--"} state="ok" />
        <GaugeBlock label="光照突变" value={sensors ? `${sensors.lightDelta}` : "--"} state="warn" />
        <GaugeBlock label="姿态变化" value={sensors ? `${sensors.angleDelta}°` : "--"} state="ok" />
        <GaugeBlock label="震动峰值" value={sensors ? `${sensors.vibrationPeak} g` : "--"} state="ok" />
        <GaugeBlock label="门状态" value={sensors?.doorState ?? "--"} state="ok" />
      </div>
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
}: {
  timeoutMs: number;
  setTimeoutMs: (value: number) => void;
  refreshSeconds: number;
  setRefreshSeconds: (value: number) => void;
}) {
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
          <input type="number" min={2} max={60} value={refreshSeconds} onChange={(event) => setRefreshSeconds(Number(event.target.value))} />
        </label>
      </div>
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
