# AI 运行逻辑与记忆策略

本文记录冰箱小精灵固件当前的 AI 上下文、cache 数据源和结构化记忆写入规则。后续修改 `components/ai_client`、`components/ai_context`、`components/storage`、`components/usb_protocol` 或屏幕 AI 页面前，应先阅读本文。

## 1. 总体原则

- AI 可以参与判断“是否需要记住一件事”，但不能直接任意覆盖设备存储。
- 系统提示词负责给 AI 说明何时写记忆、输出什么指令。
- 固件负责解析、校验和执行记忆指令，只允许白名单动作和字段。
- 用户可见回复、屏幕显示和 TTS 播报中不得出现固件内部的记忆指令。
- 库存、提醒、偏好、离线队列和结构化记忆都以 `cache` LittleFS 分区为生产数据源。
- API Key 仍由对应组件按配置保存，串口响应和日志不得回显明文。

## 2. 本地 cache 数据文件

当前 `components/storage/fridge_storage.c` 挂载 `cache` LittleFS 到 `/cache`。

| 文件 | 用途 | 初始化策略 |
| --- | --- | --- |
| `/cache/ui_inventory.json` | UI 与 AI 共用的库存快照 | 文件不存在时写入默认 UI 库存种子；存在时只读取，不覆盖 |
| `/cache/memory_summary.json` | 长期结构化记忆摘要 | 文件不存在时优先从旧 NVS `fridge_store/memory` 迁移；没有旧值才写默认模板 |
| `/cache/reminder_queue.json` | AI 上下文和开门提醒使用的提醒队列 | 文件不存在时写入默认内容 |
| `/cache/user_preferences.json` | 菜谱、购物清单和语音语义使用的偏好 | 文件不存在时写入默认内容 |
| `/cache/pending_queue.json` | 离线队列摘要 | 文件不存在时写入默认内容 |
| `/cache/ai_history.json` | 48 小时 / 15 轮短期会话历史 | 文件不存在或损坏时重建为空历史 |

注意：`/cache/ui_inventory.json` 是库存单一来源。AI 上下文里的库存和屏幕 UI 读取同一个文件，避免出现“屏幕库存”和“AI 库存”分叉。

## 3. AI 上下文读取链路

`fridge_ai_context_build_preview()` 会按任务类型读取本地上下文：

- 库存：`fridge_storage_get_inventory_snapshot()` -> `/cache/ui_inventory.json`
- 提醒：`fridge_storage_get_reminder_queue()` -> `/cache/reminder_queue.json`
- 偏好：`fridge_storage_get_user_preferences()` -> `/cache/user_preferences.json`
- 长期结构化记忆：`fridge_storage_get_memory_summary()` -> `/cache/memory_summary.json`
- 离线队列：`fridge_storage_get_offline_queue_summary()` -> `/cache/pending_queue.json`
- 短期历史：`fridge_storage_get_chat_history()` -> `/cache/ai_history.json`

短期历史用于连续对话上下文，长期结构化记忆用于偏好、忌口、过敏、家庭人数、常用工具和用户明确要求记住的稳定事实。

## 4. 结构化记忆写入策略

### 4.1 何时允许写入

AI 只有在满足以下条件之一时，才应输出记忆写入指令：

- 用户明确说“记住”“以后都按这个来”“我不吃某某”“我过敏某某”等长期偏好。
- 用户表达稳定家庭信息，例如家庭人数、常用厨具、长期饮食习惯。
- 用户明确要求删除或忘记某项偏好。
- 对话中产生了后续确实有价值的长期摘要，并且不是单次临时任务。

### 4.2 不应写入的内容

以下内容不要写入长期结构化记忆：

- 闲聊。
- 单次菜谱请求。
- 临时库存状态。
- 未确认的图片识别结果。
- 传感器实时状态。
- 一次性的定时器、秒表、闹钟指令。
- 含糊、不确定或用户没有确认的推测。
- API Key、Wi-Fi 密码、令牌、个人敏感明文。

## 5. MEMORY_OP 指令

AI 如果决定写入记忆，必须在回复最后另起一行输出隐藏指令：

```text
MEMORY_OP:{"action":"append|replace|clear","key":"taste|avoid|allergies|family_size|kitchen_tools|recent_summary","value":"不超过80字的中文摘要","reason":"简短原因"}
```

如果不需要写记忆，不输出 `MEMORY_OP`。

示例：

```text
好的，以后给你推荐菜谱时我会避开香菜。
MEMORY_OP:{"action":"append","key":"avoid","value":"不吃香菜","reason":"用户明确表达长期忌口"}
```

```text
我已经记住家里是 3 个人吃饭。
MEMORY_OP:{"action":"replace","key":"family_size","value":"3","reason":"用户明确更新家庭人数"}
```

```text
好的，我会忘记之前记录的过敏信息。
MEMORY_OP:{"action":"clear","key":"allergies","value":"","reason":"用户要求清除过敏记录"}
```

## 6. 固件执行边界

固件通过 `fridge_storage_apply_memory_directive()` 执行记忆指令。

固件会做以下校验：

- 只识别回复中的 `MEMORY_OP:` 后的 JSON 对象。
- 只允许 `append`、`replace`、`clear` 三种动作。
- 只允许 `taste`、`avoid`、`allergies`、`family_size`、`kitchen_tools`、`recent_summary` 六类字段。
- `family_size` 必须是 `0..12` 的数字。
- 字符串值会按 UTF-8 安全前缀裁剪到约 80 字节。
- 数组类字段会裁剪数量，避免文件无限增长。
- 整个 `memory_summary.json` 不得超过 `FRIDGE_STORAGE_MAX_MEMORY_LEN`。
- 指令解析失败、字段非法或超限时，只忽略记忆写入，不影响用户可见回复。

固件会把 `MEMORY_OP` 从回复中剥离：

- USB `ai_assistant_chat` 返回给 Web 前会剥离。
- 屏幕 AI 语音页显示和 TTS 播报前会剥离。
- 短期会话历史保存前会尽量保存剥离后的助手回复。

## 7. 覆盖与追加规则

- `append`：向数组字段追加一条记忆，例如 `avoid`、`taste`、`recent_summary`。
- `replace`：覆盖某个字段。数组字段会变成只包含新值的数组；`family_size` 会写成数字。
- `clear`：清空某个字段。数组字段重建为空数组；`family_size` 删除后不自动补默认值。

生产建议：

- 长期偏好优先使用 `append`。
- 用户明确纠正旧信息时使用 `replace`。
- 用户明确要求忘记时使用 `clear`。

## 8. AI_ACTION 与设备动作权限

AI 可以在用户明确要求时输出紧凑 JSON 指令，由固件 `components/ai_actions` 统一解析和白名单执行。第一版只允许低风险页面切换和已有厨房计时工具，不开放亮度、音量、Wi-Fi 连接、拍照、库存写入、OTA 或 GPIO 控制。

页面切换格式：

```text
{"tool":"ui","action":"switch_page","page":"home|standby|zone|door|recipe|nutrition|shopping|settings|wifi|more|offline|ai|timer|stopwatch|alarm"}
```

厨房工具格式沿用现有 JSON：

```text
{"tool":"timer","action":"start","duration_seconds":480,"label":"煮蛋"}
{"tool":"stopwatch","action":"start"}
{"tool":"alarm","action":"set","hour":7,"minute":0,"label":"拿牛奶"}
```

执行边界：

- 只允许白名单页面；`camera` 和 `camera_result` 暂不允许 AI 自动进入，因为拍照页会启动 OV3660 预览，属于硬件副作用。
- 页面切换不会直接在 AI worker 或 USB worker 里操作 LVGL；`ui` 组件通过 `fridge_ui_set_page_async()` 投递到 UI 线程执行。
- JSON 指令包含未知字段、未知页面或未知动作时，固件拒绝执行并返回需要确认的提示。
- `MEMORY_OP` 和设备动作可同时出现在一次回复中；固件先剥离并处理记忆，再执行 AI 动作，最后保存/显示用户可见文本或动作反馈。

## 9. 代码位置

- `components/ai_client/fridge_ai_client.c`
  - `build_assistant_request()`：系统提示词中声明 AI 自主记忆判断、`MEMORY_OP` 输出格式和白名单 UI 控制 JSON。
- `components/ai_actions/fridge_ai_actions.c`
  - `fridge_ai_actions_execute_json()`：统一解析 UI 页面和厨房工具 JSON。
  - `fridge_ai_actions_strip_directives()`：执行动作后剥离紧凑 JSON，避免屏幕/TTS/历史中残留内部指令。
- `components/ui/fridge_ui_task.c`
  - `fridge_ui_set_page_async()`：把页面切换投递到 LVGL UI 线程。
  - `fridge_ui_page_from_key()`：把 AI 白名单页面 key 映射到 `fridge_ui_page_t`。
- `components/storage/fridge_storage.c`
  - `fridge_storage_apply_memory_directive()`：解析、校验、执行记忆指令并返回剥离后的回复。
  - `fridge_storage_get_memory_summary()`：读取 `/cache/memory_summary.json`。
  - `fridge_storage_set_memory_summary()`：手动写入 `/cache/memory_summary.json`。
- `components/usb_protocol/fridge_usb_protocol.c`
  - `handle_ai_assistant_chat()`：USB AI 回复返回前执行并剥离 `MEMORY_OP`。
  - `voice_chat_worker_task()`：语音链路保存和返回前处理 `MEMORY_OP`。
- `components/ui/pages/page_ai.c`
  - `ai_voice_task()`：屏幕语音回复显示和 TTS 播报前剥离 `MEMORY_OP`，并执行白名单 AI 动作。

## 10. 测试建议

基础验证：

1. 清空结构化记忆：发送 `clear_memory_summary`。
2. 对 AI 说“我不吃香菜，以后推荐菜谱避开它”。
3. 再发送 `get_memory_summary`。
4. 确认 `/cache/memory_summary.json` 中 `avoid` 或 `recent_summary` 出现对应摘要。
5. 确认 Web/屏幕/TTS 中没有出现 `MEMORY_OP` 文本。

负向验证：

1. 对 AI 闲聊或问一次性菜谱。
2. 检查 `get_memory_summary`，不应新增无关长期记忆。
3. 模拟 AI 输出非法 key 或非法 action，固件应忽略写入但正常返回回复。

AI 动作验证：

1. 对 AI 说“打开菜谱页”，应输出并执行 `{"tool":"ui","action":"switch_page","page":"recipe"}`，屏幕切到菜谱页。
2. 对 AI 说“回主页”“进入待机页”“打开 Wi-Fi 设置”，应切到对应页面。
3. 对 AI 说“打开拍照页”，固件应拒绝自动进入并提示需要用户手动确认。
4. 对 AI 说“设置 8 分钟煮蛋定时器”，应保持现有定时器执行结果。
5. 模拟未知页面、未知 action 或额外字段，固件应拒绝执行，不改变当前页面。

硬件注意：

- 记忆写入会写 Flash LittleFS，不应在高频循环里触发。
- `cache` 当前挂载策略仍包含 `format_if_mount_failed = true`，生产前需要确认数据丢失策略。
- 烧录主固件时不要把大主固件写入 `ota_1` 小 recovery 分区。
