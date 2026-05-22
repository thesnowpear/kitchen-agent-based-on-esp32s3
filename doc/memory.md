# 项目记忆与踩坑记录

本文记录“冰箱小精灵”项目中已经验证过的环境、硬件、串口、编码和联调经验。后续 AI 或协作者在开始工作前应先阅读本文件，避免重复踩坑。

----

## 2026-05-21 光照传感器与 INMP441 麦克风接入

### 接线与硬件安全

- 光照传感器当前按模拟量接入：`VCC -> 3V3`，`GND -> GND`，`IO/AO -> GPIO1`。
- `GPIO1` 对应 `ADC1_CH0`，固件用 ADC oneshot 读取 12-bit 原始值，再换算成 Web 面板展示的 `0-1023`。
- 当前光照模块实测为反向模拟量：读值高表示暗，读值低表示亮；固件保留 `lightRaw12bit` 原始值，同时把 `lightValue10bit`、`lightPercent` 和 `lightDelta` 换算成“数值越高越亮”的业务语义。
- 光照模块必须使用 `3.3V` 供电，避免 `IO/AO` 输出高于 ESP32-S3 ADC 输入范围。
- INMP441 当前按 I2S 接入：`VDD -> 3V3`，`GND -> GND`，`SCK -> GPIO40`，`WS -> GPIO41`，`SD -> GPIO42`，`L/R -> GND`。
- `L/R -> GND` 表示先固定左声道；如果后续 I2S 数据异常或转写为空，可尝试改接 `3V3` 测试右声道。
- 录音测试前先确认共地、线尽量短、杜邦线接触可靠；I2S 对接触不良比普通 GPIO 更敏感。

### 固件实现记忆

- 已新增 `components/sensors`，对外提供 `fridge_sensors_get_snapshot()`，不要在 USB 协议或业务逻辑里直接读 ADC。
- 已新增 `components/audio`，INMP441 采样参数为 `16 kHz / 16-bit PCM / mono`，录音缓存优先放 PSRAM。
- INMP441 常见输出是 24-bit 数据左对齐到 32-bit I2S slot，当前实现会把 32-bit 原始采样右移压成 16-bit PCM。
- 已新增 `components/asr`，ASR 配置独立于 AI 配置，NVS namespace 为 `fridge_asr`。
- 默认 ASR 配置为硅基流动：`https://api.siliconflow.cn/v1/audio/transcriptions`，模型 `TeleAI/TeleSpeechASR`。
- ASR 上传体包含 WAV 头和 PCM 数据，6 秒音频接近 192KB，multipart body 必须优先放 PSRAM，避免与 TLS/HTTP 内部缓冲争抢内部 SRAM。
- `voice_chat_stop` 会执行录音停止、WAV 封装、ASR HTTPS 请求和 AI HTTPS 请求，Web Serial 超时必须按长请求处理，不能使用普通短命令超时。

### USB/Web Serial 调试记忆

- 固件 USB 协议仍是 `115200` 波特率、JSON Lines，一行一个请求，一行一个响应。
- `get_sensors` 已返回真实光照字段：`lightRaw12bit`、`lightValue10bit`、`lightPercent`、`lightDelta`、`lightPolarity`，同时保留 `pir`、`lux`、`angleDelta`、`vibrationPeak`、`doorState` 等旧字段；其中 `lux` 只是兼容旧 Web 面板的亮度 0-1023，不是真实物理 lux。
- `get_asr_config` 会返回 ASR Base URL、模型、超时、`hasApiKey`、`apiKeyPreview` 和 `ready`，不会回显明文 Key。
- 已新增 USB 命令：`get_asr_config`、`set_asr_config`、`clear_asr_key`、`voice_chat_start`、`voice_chat_stop`、`voice_chat_status`。
- ASR Key 缺失时，`voice_chat_stop` 的预期错误是 `missing ASR API Key`，这是正常可读错误，不代表录音任务失败。
- COM16 可能被 `idf.py monitor`、Web Serial、VS Code Serial Monitor 或其他串口助手占用。如果出现 `Access to the port 'COM16' is denied`，先查是否有残留 `idf_monitor` 进程。
- `idf.py monitor` 在被外部超时强杀时可能留下多层 Python 子进程继续占用 COM16；只应停止命令行明确包含 `COM16` 且为 `idf_monitor` 或 `idf.py -p COM16 monitor` 的进程，不要乱杀其它 Python 服务。

### 2026-05-21 真机验证结果

- `idf.py build` 通过，固件镜像约 `0x116140`，最小 app 分区 `0x240000`，剩余约 52%。
- `npm run build` 通过。
- `idf.py -p COM16 flash` 成功，烧录设备识别为 ESP32-S3 rev v0.2，8MB Flash，8MB PSRAM。
- 启动日志确认 PSRAM 初始化和 memory test OK。
- 启动日志确认 `fridge_sensors` 初始化 GPIO1/ADC1_CH0 成功。
- 启动日志确认 `fridge_audio` 初始化 INMP441 I2S 成功。
- 串口验证 `get_sensors` 可返回实时光照值，遮挡/照射应继续用 Web 面板做实物复测。
- 串口验证 `voice_chat_start` 后 `pcmBytes` 和 `rms` 会增长，说明录音任务在跑。
- 测试中出现过连续 `i2s read failed: ESP_ERR_TIMEOUT`，已改为限频日志 `i2s read timeout, count=N`，避免刷爆 USB 日志。
- I2S timeout 如果持续出现，优先检查麦克风是否真实接线、`SD/WS/SCK` 是否接反、`L/R` 声道选择、GND 是否可靠、线是否太长。

### 文档与编码记忆

- 本项目涉及中文文档，编辑前必须确认目标文件是否为有效 UTF-8。
- `doc/progress.md` 在 2026-05-21 检查时存在非 UTF-8 字节，`apply_patch` 无法安全编辑；不要用二进制方式强行追加中文，避免扩大乱码。
- 如果需要更新 `doc/progress.md`，应先单独处理文件编码或让用户确认是否允许做一次编码修复。
- 编辑前按项目要求保留 `.bak-YYYYMMDD-HHMMSS` 备份；本次相关备份已生成在对应文件旁。

----

## 2026-05-21 麦克风采集优化参考结论

- 已参考 `example/microphone_reference/atomic14-esp32-i2s-mic-test`、`atomic14-esp32_audio` 和 Espressif 官方 `examples/peripherals/i2s`。
- 当前项目继续使用 ESP-IDF 新 I2S STD 驱动，不引入 Arduino/PlatformIO 依赖。
- I2S 通道应在初始化时配置，在录音开始时 enable，在录音任务结束时 disable，避免空闲时 DMA 长时间运行。
- INMP441 仍按 `16 kHz / 32-bit slot / mono / left slot` 采集，`L/R -> GND` 对应左声道。
- 当前 32-bit 原始采样转 16-bit PCM 的默认位移为 `raw >> 14`；如果声音过小可尝试 `>> 12` 或 `>> 11`，如果削顶严重可尝试 `>> 16`。
- `voice_chat_status` 新增诊断字段：`sampleCount`、`peakAbs`、`minSample`、`maxSample`、`meanSample`、`clipCount`、`timeoutCount`、`qualityHint`。
- `qualityHint=ok` 表示当前采样质量基本可用；`silent` 优先检查麦克风供电、SD、L/R 声道和说话距离；`clipping` 表示 PCM 可能削顶；`i2s_timeout` 优先检查 SCK/WS/SD/GND、线长和接触；`too_short` 表示录音太短或无样本。

----

## 2026-05-22 麦克风测试独立页面

- Web 面板已新增侧边栏页面“麦克风测试”，用于单独验证 INMP441 硬件采集、音量、I2S 稳定性和 ASR 识别效果。
- 新增 USB 命令：`mic_record_start`、`mic_record_status`、`mic_record_stop`。这三条命令只做麦克风采集诊断，其中 `mic_record_stop` 不会上传 ASR，也不会调用 AI。
- `voice_chat_start`、`voice_chat_status`、`voice_chat_stop` 保持原有 AI 助手语音对话链路；`voice_chat_stop` 仍会执行“停止录音 -> ASR -> AI 回复”。
- Web 的“硬件录音测试”会每 `500 ms` 轮询一次 `mic_record_status`，显示 `rms`、`peakAbs`、`min/max`、`clipCount`、`timeoutCount`、`qualityHint` 和最近 30 次轻量音量柱状图。
- Web 的“ASR 识别测试”复用 `voice_chat_start/voice_chat_stop`，用于完整验证“录音 -> 转文字 -> AI 回复”，并显示转写文本、AI 回复、ASR/AI 耗时和 HTTP 状态。
- AI 助手页仍保留语音按钮，但硬件录音测试进行中时不会允许它误触发 `voice_chat_stop`；如需停止硬件录音，应回到“麦克风测试”页点击停止。
- `qualityHint` 排查建议：
  - `ok`：采样基本可用，可以继续测试 ASR。
  - `silent`：检查 VDD/GND/SD、L/R 声道选择和说话距离。
  - `clipping`：PCM 可能削顶，后续可调大 `AUDIO_PCM_SHIFT` 或降低输入增益。
  - `i2s_timeout`：优先检查 SCK/WS/SD/GND、线长和杜邦线接触。
  - `too_short`：至少录 1-2 秒再判断，确认 `pcmBytes` 是否按约 `32000 bytes/s` 增长。

----

## 2026-05-22 语音链路 AI HTTP 400 排查

- 用户实测麦克风硬件测试、ASR 转写和 AI 回复整体可用，但连续说几次后偶发 `AI HTTP 状态异常：400`。
- `example/INMP441麦克风模块` 官方例程使用 Arduino 旧 I2S 驱动，配置为 `16-bit / ONLY_LEFT / I2S_MSB / 44100Hz`；当前工程继续使用 ESP-IDF 新 I2S STD 驱动和 `32-bit slot -> 16-bit PCM`，因为现有麦克风与 ASR 已验证正常，暂不因该例程改采样链路。
- 已增强 AI HTTP 400 错误显示：如果服务端响应中没有 `message/error_msg/detail`，固件会把响应 JSON 摘要带回 Web，避免只看到“状态异常：400”。
- 已给语音链路增加 ASR 空文本保护：ASR 返回空文本或极短文本时不再继续请求 AI，而是提示重新录音，避免兼容 API 因空 user content 返回 400。
- 语音链路和文字对话应保持一致：ASR 转写文本只作为另一种输入方式，进入 AI 后同样注入项目上下文、结构化记忆和设备侧短期会话历史。
- 语音链路已复用文字对话的设备历史裁剪逻辑 `convert_storage_history_to_ai_history()`，避免 user/assistant 顺序异常导致兼容 API 返回 400。
- 已确认一次真实 400 根因为：`messages[0].content: invalid unicode code point`，通常是固定长度缓冲或 `strlcpy` 按字节截断中文 UTF-8，导致半个汉字进入 AI 请求 JSON。
- AI 客户端 `json_escape_alloc()` 已增加 UTF-8 校验与清洗，所有 system/context/history/user 文本在出网前都会把非法 UTF-8 字节替换为 `?`，避免 OpenAI-compatible 服务拒绝解析请求体。
- 如果后续仍出现 400，优先查看 Web/串口返回的服务端错误摘要；常见方向是模型名不兼容、Base URL 不匹配、服务不支持 `max_tokens/temperature` 参数、或单次上下文仍然过长。
