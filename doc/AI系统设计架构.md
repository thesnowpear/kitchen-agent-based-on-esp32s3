# 冰箱小精灵 AI 系统设计架构

## 1. 目标与边界

本文档固定“冰箱小精灵”首版 AI 业务闭环的架构。当前方案采用 ESP32-S3 设备直连 OpenAI-compatible API：`components/ai_client` 保存 API Base URL、模型、系统提示词和 API Key，并通过 HTTPS 直接发起 AI 请求；`components/ai_context` 负责为不同任务生成结构化上下文和输出约束。

核心边界：

- ESP32-S3 不长期保存完整聊天记录或图片原图；API Key 在 Demo 阶段允许保存到 NVS，但不得在串口响应、日志或仓库中回显明文。
- 端侧只保存必要配置、库存快照、提醒队列、用户偏好、结构化记忆摘要和离线任务元数据。
- AI 结果不能直接入库，必须经过规则校验和用户确认。
- 开门提醒、离线库存摘要和临期提醒走本地快路径，不等待 AI API。

## 2. 分层架构

```text
Web 运维面板 / 屏幕 / 语音
  |
  | USB JSON Lines / UI 事件 / 语音文本
  v
ESP32-S3 端侧
  - ai_client：设备直连 AI API
  - storage：库存、提醒、偏好、记忆摘要、离线队列
  - ai_context：按任务生成最小上下文包和 Mock 结构化结果
  - 后续 ai_orchestrator：排队、重试、解析结果、进入确认流程
  |
  | HTTPS
  v
OpenAI-compatible API
  - /chat/completions 文本与结构化任务
  - 后续多模态图片识别
  - 模型按 Web 面板配置切换
  |
  v
库存服务 / 小程序确认页 / 购物清单 / 菜谱推荐
```

端侧职责是稳定、可解释、可离线，并负责直接访问 AI API。后续如需要跨设备同步或远程日志，应作为独立运维能力设计，不改变 AI 直连主链路。

## 3. AI 任务类型

| 任务类型 | 用途 | 输出要求 |
| --- | --- | --- |
| `chat_assist` | 普通厨房助手问答 | 简短中文回复，不能编造库存 |
| `recognize_ingredients` | 拍照识别食材候选 | 名称、数量估计、置信度、疑点、确认字段 |
| `inventory_parse` | 解析“买了/吃掉/放进去了”等表达 | 库存变更建议和待确认字段 |
| `recipe_generate` | 根据库存和偏好推荐菜谱 | 菜名、可用库存、缺少食材、耗时、步骤 |
| `shopping_list_generate` | 生成购物清单 | 建议购买、可选补充、依据 |
| `reminder_explain` | 解释临期/过期提醒 | 食材、剩余天数、位置、保守处理建议 |
| `voice_intent_parse` | 语音意图解析 | 意图、槽位、需要确认的信息 |

## 4. 上下文注入策略

系统提示词拆成三层：

- 固定人格模板：回答风格、安全边界、确认原则。
- 任务模板：不同任务的输出 schema 和业务规则。
- 动态上下文：按需注入库存、提醒、偏好、记忆摘要、离线队列和设备状态。

默认只注入本次任务需要的最小数据。不要把完整数据库、完整聊天记录或大量历史事件放入 prompt；长期记忆只保存结构化摘要，例如家庭人数、口味、忌口、过敏、常用食材、最近操作摘要。

## 5. 本地存储设计

当前实现新增 `components/storage` 作为存储门面：

- NVS：保存小型结构化记忆摘要，以及已有 Wi-Fi/设备直连 AI 配置。
- LittleFS `cache`：后续保存库存快照、提醒队列、离线任务、事件日志。
- LittleFS `assets`：后续保存默认提示词模板、UI 资源和少量固定语音资源。
- PSRAM：运行时临时上下文、HTTP 请求体、AI 响应和图片帧缓冲。

首版先使用种子 JSON 数据打通接口，后续在 `storage` 组件内部替换为 LittleFS 文件读写，外部调用方不需要改接口。

## 6. USB/Web 调试接口

保留已有设备直连 AI 命令：

- `get_ai_config`
- `set_ai_config`
- `test_ai_chat`
- `clear_ai_key`

当前 Web 面板合并为 `AI 助手` 页面，主对话默认使用真实项目上下文命令：

- `ai_assistant_chat`：输入任务类型、用户消息、最近历史和上下文开关，设备先生成最小上下文包，再直连 OpenAI-compatible `/chat/completions`。
- `get_ai_context_preview`：输入任务类型和上下文开关，返回本次会注入的上下文包。
- `test_ai_task`：返回本地 Mock 结构化结果，不调用第三方模型，不写库存。
- `get_memory_summary`：读取结构化记忆摘要。
- `set_memory_summary`：写入用户明确确认的硬件测试记忆摘要，不保存完整聊天记录。
- `clear_memory_summary`：清空结构化记忆摘要，库存和提醒不受影响。

`test_ai_chat` 仍作为隐藏/次要的基础连通性探针保留；`AI 助手` 页面同时提供 AI 设置、任务模式、上下文预览、真实对话和硬件测试记忆维护。

## 7. 设备直连 AI API

设备端直接调用 OpenAI-compatible 接口。首版最小接口：

- `POST {api_base_url}/chat/completions`
- 请求包含：`model`、`messages`、`temperature`、`max_tokens`、`stream=false`
- `messages` 中包含：固定人格模板、任务模板、动态上下文和用户输入
- 响应解析：优先读取 `choices[0].message.content`，并按任务要求解析为结构化 JSON 或简短中文回复

设备端策略：

- 本地 Mock：比赛现场、离线演示或额度异常时兜底，不作为云端服务。
- OpenAI-compatible：正式首版真实模型接入方式。
- 多配置槽：允许保存多个 Base URL/模型配置，便于在不同兼容服务之间切换。
- 后续多模态：摄像头接入后，按所选服务商的兼容格式扩展图片输入。

所有 AI 请求应记录 `prompt_version`、`model_name`、`request_id` 和耗时，但日志不得保存 API Key、Wi-Fi 密码、家庭 ID 等敏感信息。

## 8. 安全与确认规则

- AI 识别结果低置信度、图片模糊、遮挡或包装文字不清时必须要求用户确认或重新拍照。
- 不得自动删除、消耗、移动或修改库存。
- 临期和过期食品建议必须保守；异味、霉变、胀包、冷链异常时提醒谨慎食用或丢弃。
- 不声称能看到实时画面、传感器或真实库存，除非系统输入明确提供。
- 对儿童饮食、疾病饮食和过敏问题保持谨慎，必要时建议咨询专业人士。

## 9. 当前落地状态

- 已新增 `storage` 组件，提供库存快照、提醒队列、偏好、记忆摘要和离线队列读取接口。
- 已新增 `ai_context` 组件，支持按 AI 任务生成上下文预览和本地 Mock 结构化结果。
- 已扩展 USB JSON Lines 命令，Web 面板可通过 `ai_assistant_chat` 进行真实上下文对话。
- 已将 Web `AI API` 与 `项目 AI` 合并为 `AI 助手` 页面，用于配置 API、选择任务、预览上下文、写入测试记忆和真实调用模型。
- 已移除后端参考实现，避免后续实现误回到中转服务路线。

## 10. 后续迭代

1. 在 `storage` 组件内接入 LittleFS `cache` 和 `assets` 分区。
2. 新增 `ai_orchestrator`，负责设备端 AI 请求排队、重试、超时、结构化解析和结果确认状态。
3. 将 `test_ai_task` 从本地 Mock 升级为真实 `ai_client` 调用，并保留 Mock 开关。
4. 接入摄像头拍照任务和多模态/图片输入链路。
5. 将 AI 结果接入 Web/屏幕确认页，确认后再写库存事件。
