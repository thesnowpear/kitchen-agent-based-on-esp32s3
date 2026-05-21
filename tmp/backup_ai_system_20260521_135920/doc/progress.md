# 冰箱小精灵项目进度

说明：

- 本文档记录已经完成的阶段性进展。
- 较大进展使用 `----` 分隔，并写明完成时间。
- 小修改和小修复默认追加到最近相关进展下，不单独新开分隔块。

----
完成时间：2026-05-17 21:08:38 +08:00

- 完成项目架构文档 `doc/项目架构与工作流设计.md`。
- 明确了主控 `ESP32-S3`、独立 `ESP32-CAM`、云端 AI Adapter、小程序/移动端的整体分层架构。
- 固化了硬件拓扑、推荐组件划分、状态机、MQTT/HTTP 接口、数据模型、分区建议和测试验收路径。

----
完成时间：2026-05-17 22:25:58 +08:00

- 完成项目级 `AGENTS.md` 初版约束整理。
- 明确了硬件安全规则、中文注释要求、组件化建议、实现顺序和测试要求。
- 为后续协作者统一了硬件风险检查和代码组织基线。

----
完成时间：2026-05-18 00:28:46 +08:00

- 完成主控工程向 `ESP32-S3-DevKitC-1 N8R8` 规划配置的基础收敛。
- 已新增 `partitions.csv` 与 `sdkconfig.defaults`，规划为 `8MB Flash + 8MB PSRAM + 双 OTA + 本地数据分区`。
- 分区中已预留 `assets`、`cache`、`coredump`，为后续资源缓存、离线队列和诊断留出空间。

----
完成时间：2026-05-18 20:16:14 +08:00

- 完成主固件基础组件骨架搭建。
- `components/` 下已建立 `diagnostics`、`network`、`usb_protocol`、`display_test` 等组件。
- `main/main.c` 已接入主控启动编排，支持 `NVS`、诊断、网络、USB 配网控制台，以及独立屏幕测试模式切换。
- 当前主控工程已经脱离空白模板，进入“可继续迭代的基础框架”阶段。

----
完成时间：2026-05-18 20:29:21 +08:00

- 完成项目协作文档补充。
- 在 `AGENTS.md` 中补充了当前使用的 `ESP-IDF 6.0.1`、主固件与前端的基础技术栈、编程约定和进度记录规则。
- 新建 `doc/progress.md`，用于后续自动累计项目里程碑与阶段性完成情况。

----
完成时间：2026-05-18 21:47:00 +08:00

- 完成项目硬件方案基线修订。
- 将摄像头方案从独立 `ESP32-CAM + OV2640 + TF 卡` 调整为 `OV3660` 直连 `ESP32-S3` 主控。
- 将人体靠近/存在检测从 `PIR` 调整为 `HLK-LD2410C/LD2410B` 或同类 `24GHz/25GHz` 人体存在毫米波雷达模块。
- 明确首版不新增 `TF/SD` 卡，离线时只保留拍照任务状态、错误码和必要元数据，不承诺保存图片原图。

----
���ʱ�䣺2026-05-18 22:34:00 +08:00

- ��� `display_test` ����С������ǿ��������Ļ����ʹ�ܽ� `GPIO7`���ϵ�������߱����ٽ��� QSPI ���ԡ�
- ����������ͼ˳��Ϊ�׵����ȣ��������֡�����δ�����͡�ͼ��δд�롱�������⡣
- ���¹���ͨ���������µ� `fridge_spirit.bin`����ǰ��Ļ���������ƫ��Ӳ������/����·���������ǹ̼�δ���С�

----
完成时间：2026-05-20 21:52:38 +08:00

- 完成两份屏幕相关 PDF 的 Markdown 整理，便于后续固件和硬件接线时快速检索。
- 新增 `doc/屏幕模组_TS040HDS02CP-B1620A_规格整理.md`，覆盖模组规格、连接器引脚、电气/背光参数、SPI/QSPI 时序、光学参数、可靠性测试、机械图和使用注意事项。
- 新增 `doc/TR230S_DATASHEET_V2.3_整理.md`，覆盖 TR230S 总体能力、DBU 引脚、接口配置、SPI/QSPI/8080 时序、核心寄存器、电气特性和驱动接入建议。
- 新增 `doc/screen_assets/`，保存时序图、机械图、封装图和 RGB 映射图等 Markdown 难以纯文本表达的关键页面截图。
----
完成时间：2026-05-20 22:31:47 +08:00

- 完成 Web 配网与 AI API 测试链路的模块化接入。
- 默认关闭 `CONFIG_FRIDGE_SCREEN_TEST`，正常主控路径会启动 NVS、Wi-Fi、AI 配置组件和 USB JSON Lines 运维面板；屏幕测试代码仍保留，后续可手动重新启用。
- 新增 `components/ai_client`，支持在开发板 NVS 中保存 OpenAI-compatible API Base URL、模型、系统提示词和 API Key，并通过 HTTPS `/chat/completions` 执行文本聊天测试。
- 扩展 Web Serial 协议命令：`get_ai_config`、`set_ai_config`、`clear_ai_key`、`test_ai_chat`；响应中不回传 API Key 明文，只返回保存状态和脱敏预览。
- Web 面板新增 `AI API` 页面，支持配置保存、清除 Key 和模拟聊天测试；Mock 模式同步支持无硬件演示。
- 安全边界：当前为 NVS 开发模式保存 Key，适合今天快速联调；后续可替换为云端 AI Adapter 或 NVS 加密方案。
----

## 2026-05-20 23:26:33 USB 协议稳定与 Wi-Fi 扫描恢复

- 修复 Web Serial 连接后初始化并发请求过多的问题：Web 面板改为串行读取设备状态，并避免刷新任务重入，旧固件缺少 AI 命令时不会拖垮网络页面。
- 修复固件 USB JSON Lines 读取：不再用 gets(stdin) 假设一次读完整行，改为逐字节累积到换行后解析，避免半条 JSON 被误判为 command is required。
- 增强固件协议兼容性：忽略非 JSON 串口噪声，并兼容早期 cmd 字段。
- 修复诊断任务快照中 Wi-Fi heartbeat 指向局部变量导致乱码的问题。
- 已重新构建并刷写 ESP32-S3；串口探针验证 get_status、get_network、get_ai_config 和 scan_wifi 均能按 request_id 返回。
- 实测 scan_wifi 能扫描到多个 2.4GHz AP，包括 igo_DC991F、SEU-ISP、SEU-WLAN、SEU-GUEST。
补充：为降低 Web 面板长期打开时的电脑负载，默认自动刷新间隔从 5 秒调整为 15 秒，后台标签页暂停刷新，周期刷新只读取动态状态；Wi-Fi 扫描、联网和 AI 测试期间跳过自动刷新，串口 TX 调试日志默认关闭。
----

## 2026-05-21 01:02:21 Wi-Fi 连接诊断与串口响应稳定性修复

- 按用户要求将 Espressif 官方 ESP-IDF Wi-Fi station/scan 示例以 sparse checkout 方式拉取到 `example/esp-idf-wifi-reference/`，用于对照成熟连接流程。
- 对照官方 station 示例，将 Wi-Fi 连接等待时间从 15 秒放宽到 35 秒，避免还在重试阶段就被 Web 端判断为超时。
- 增强 `components/network` 的断线 reason 诊断：认证失败、安全模式不兼容、找不到 AP、信号丢失、握手超时等场景会写入更明确的中文错误说明，并回传到 Web 面板。
- Wi-Fi 密码只在成功拿到 IP 后写入 NVS，避免错误密码连接失败后污染已保存配置。
- USB JSON Lines 响应输出时锁定 `stdout`，降低 ESP_LOG 日志插入响应中间导致 Web 解析失败或超时的概率。
- AI API 测试在网络已连接但未完成 SNTP 校时时会主动再尝试一次校时，减少“Wi-Fi 已连但 AI 测试被 internet_ready 拦住”的情况。
- 固件已重新构建通过；下一步需要把新固件烧录到 ESP32-S3 后，再观察实际断线 reason 来区分密码、热点兼容性、信号和供电问题。
----

## 2026-05-21 01:23:00 AI API 测试崩溃定位与修复

- 通过 COM16 直接复现 `test_ai_chat`，确认原失败不是 API 服务错误，而是 `usb_protocol` 任务在执行 AI HTTPS 测试时发生栈溢出并重启。
- 将 USB JSON Lines 行缓冲改为静态存储，并把 AI 测试消息与结果结构改为堆分配，降低 USB 任务栈压力。
- 将 USB 协议任务栈提升到 32KB，给 `esp_http_client`、TLS 和 JSON 响应留出更稳定的运行空间。
- AI HTTP 非 2xx 响应现在会优先透传服务端 `error.message`，方便区分 Key 错误、模型错误、限流和 Base URL 错误。
- SNTP 校时增加备用服务器尝试，降低单个 NTP 节点不可达导致 HTTPS/TLS 前置检查失败的概率。
- 已重新构建并刷写 ESP32-S3；实测 `get_network` 在线、`get_ai_config` 配置存在，`test_ai_chat` 返回 HTTP 200 和 `Pong！`，AI API 链路闭环成功。
----
## 2026-05-21 01:51:28 AI 连接参考与运行压力优化完成

- 新增 example/ai_reference/，以参考方式保存 ESP-IDF esp_http_client、ESP32_AI_Connect、espai、ESP32_ChatGPT，不接入 CMake/固件依赖。
- AI API 测试拆到一次性 i_chat_worker，USB 协议任务常驻栈回落到 12KB，worker 使用临时 32KB 栈，完成后释放。
- AI 测试请求增加 max_tokens=128，HTTP 响应缓冲从 8KB 降为 4KB，并显式使用 Accept-Encoding: identity。
- Wi-Fi 获取 IP 后改为后台 SNTP；AI 测试前仍做有界校时确认，SNTP 使用互斥避免并发重入。
- Web 周期刷新不再每轮读取 AI 配置列表，仅进入 AI 页面或配置变更后刷新，降低串口和浏览器压力。
- 已完成固件构建、Web 构建、刷写和真机回归；连续 10 次 	est_ai_chat 均 HTTP 200，无 stack overflow、无重启、无串口 JSON 破碎。
- 验证记录：AI worker 栈水位约 27344-27568 words，USB 协议任务 AI 后栈水位约 8944 words，Wi-Fi 获取到 IP 192.168.0.106，API 配置仍保存在 NVS。
- 2026-05-21 02:05:48 补充：放宽 AI API 系统提示词配置。固件 FRIDGE_AI_MAX_SYSTEM_PROMPT_LEN 从 256B 提升到 2048B，Web 输入框取消 240 字硬限制并改为 UTF-8 字节计数提示；USB 侧相关 AI 配置结构改为堆分配，避免长提示词增加常驻任务栈压力。已通过 
pm run build 和 idf.py build。- 2026-05-21 02:06:48 补充：USB JSON Lines 单行缓冲从 4096B 提升到 6144B，用于容纳长系统提示词转义后的请求；复跑 idf.py build 通过。- 2026-05-21 02:22:55 补充：系统提示词长文本支持升级到 8192B。固件改为使用 NVS blob 保存 systemPrompt，兼容旧版短字符串读取；USB JSON Lines 单行缓冲提升到 16KB，AI 配置/测试大结构改为堆分配。已刷写 COM16，并用当前厨房助手完整提示词真机校验：3779B 写入、3779B 读回、内容完全一致。