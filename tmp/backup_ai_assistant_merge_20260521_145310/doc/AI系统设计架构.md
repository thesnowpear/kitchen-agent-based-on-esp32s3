# 冰箱小精灵 AI 系统设计架构

## 1. 目标与边界

本文档固定“冰箱小精灵”从开发期 AI 连通性测试走向正式 AI 业务闭环的架构。当前 `components/ai_client` 保留为开发调试探针，用于验证 OpenAI-compatible API 地址、Key、模型和基础聊天是否可用；正式项目 AI 以“端侧采集与缓存 + 云端 AI Adapter + 结构化业务数据注入 + 用户确认闭环”为主。

核心边界：

- ESP32-S3 不长期保存完整聊天记录、图片原图或第三方大模型 API Key。
- 端侧只保存必要配置、库存快照、提醒队列、用户偏好、结构化记忆摘要和离线任务元数据。
- AI 结果不能直接入库，必须经过规则校验和用户确认。
- 开门提醒、离线库存摘要和临期提醒走本地快路径，不等待云端模型。

## 2. 分层架构

```text
Web 运维面板 / 屏幕 / 语音
  |
  | USB JSON Lines / UI 事件 / 语音文本
  v
ESP32-S3 端侧
  - ai_client：开发期 API 连通性测试
  - storage：库存、提醒、偏好、记忆摘要、离线队列
  - ai_context：按任务生成最小上下文包和 Mock 结构化结果
  - 后续 ai_orchestrator：创建云端 AI Job、接收结果、进入确认流程
  |
  | HTTPS / MQTT
  v
云端 AI Adapter
  - Mock Provider
  - OpenAI-compatible Provider
  - 规则校验与 JSON Schema 校验
  - Prompt/模型版本记录
  |
  v
库存服务 / 小程序确认页 / 购物清单 / 菜谱推荐
```

端侧职责是稳定、可解释、可离线；云端职责是真实 AI 推理、多模态识别、跨设备同步和长期数据存储。

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

- NVS：保存小型结构化记忆摘要，以及已有 Wi-Fi/开发期 AI 配置。
- LittleFS `cache`：后续保存库存快照、提醒队列、离线任务、事件日志。
- LittleFS `assets`：后续保存默认提示词模板、UI 资源和少量固定语音资源。
- PSRAM：运行时临时上下文、HTTP 请求体、AI 响应和图片帧缓冲。

首版先使用种子 JSON 数据打通接口，后续在 `storage` 组件内部替换为 LittleFS 文件读写，外部调用方不需要改接口。

## 6. USB/Web 调试接口

保留已有开发期命令：

- `get_ai_config`
- `set_ai_config`
- `test_ai_chat`
- `clear_ai_key`

新增项目 AI 调试命令：

- `get_ai_context_preview`：输入任务类型和上下文开关，返回本次会注入的上下文包。
- `test_ai_task`：返回本地 Mock 结构化结果，不调用第三方模型，不写库存。
- `get_memory_summary`：读取结构化记忆摘要。
- `clear_memory_summary`：清空结构化记忆摘要，库存和提醒不受影响。

Web 面板中 `AI API` 页面定位为开发调试；`项目 AI` 页面用于验证正式 AI 任务、上下文注入、记忆摘要和确认边界。

## 7. 云端 AI Adapter

云端 AI Adapter 是设备端和第三方模型之间的边界，设备端不直接感知供应商差异。最小接口：

- `POST /api/v1/ai/jobs`
- 请求包含：`task_type`、`request_id`、`device_id`、`context_refs`、`local_snapshot_version`、`input`
- 响应包含：`job_id`、`status`、`result_json`、`confidence`、`needs_confirmation`

Provider 策略：

- `mock`：比赛现场或离线演示兜底。
- `openai_compatible`：后续接入真实模型。
- 其他供应商通过同一 Adapter 封装，不进入设备端固件。

所有 AI 请求必须记录 `prompt_version`、`model_provider`、`model_name`、`request_id`，但日志不得保存 API Key、Wi-Fi 密码、家庭 ID 等敏感信息。

## 8. 安全与确认规则

- AI 识别结果低置信度、图片模糊、遮挡或包装文字不清时必须要求用户确认或重新拍照。
- 不得自动删除、消耗、移动或修改库存。
- 临期和过期食品建议必须保守；异味、霉变、胀包、冷链异常时提醒谨慎食用或丢弃。
- 不声称能看到实时画面、传感器或真实库存，除非系统输入明确提供。
- 对儿童饮食、疾病饮食和过敏问题保持谨慎，必要时建议咨询专业人士。

## 9. 当前落地状态

- 已新增 `storage` 组件，提供库存快照、提醒队列、偏好、记忆摘要和离线队列读取接口。
- 已新增 `ai_context` 组件，支持按 AI 任务生成上下文预览和本地 Mock 结构化结果。
- 已扩展 USB JSON Lines 命令，Web 面板可直接测试项目 AI 任务。
- 已新增 Web `项目 AI` 页面，用于查看上下文注入、结构化结果和记忆摘要。
- 已新增 `cloud/ai_adapter_mock`，作为云端 AI Adapter 的最小 Mock 参考实现。

## 10. 后续迭代

1. 在 `storage` 组件内接入 LittleFS `cache` 和 `assets` 分区。
2. 新增 `ai_orchestrator`，负责创建云端 AI Job、重试、离线 pending 和结果确认状态。
3. 搭建真实云端服务，替换 Mock Provider。
4. 接入摄像头拍照任务和图片上传链路。
5. 将 AI 结果接入小程序/屏幕确认页，确认后再写库存事件。
