# 扬声器与云端 TTS 测试记录

本文记录 MAX98357A I2S 扬声器和云端 TTS 在本项目中的已验证结论。后续 AI 或协作者修改 `components/speaker`、USB 协议或 Web 扬声器测试页前，应先阅读本文，避免重复踩坑。

## 硬件接线

当前固件默认使用 ESP32-S3 I2S TX 驱动 MAX98357A：

| ESP32-S3 DevKitC-1 | MAX98357A | 说明 |
| --- | --- | --- |
| 5V / VBUS | VIN / VCC | 功放供电可用 5V，调试时确认 USB/5V 电源稳定。 |
| GND | GND | 必须与 ESP32-S3 共地。 |
| GPIO40 | BCLK | I2S bit clock，与 INMP441 SCK 共用，3.3V 逻辑。 |
| GPIO41 | LRC / LRCLK / WS | I2S word select，与 INMP441 WS 共用，3.3V 逻辑。 |
| GPIO39 | DIN | I2S 数据输出，3.3V 逻辑。 |
| 3V3 或悬空 | SD / EN | 不要接 GND，否则 MAX98357A 会静音。若开头爆音明显，优先把 SD/EN 固定到 3V3。 |
| 悬空 | GAIN | 默认增益，后续如音量不合适再调整。 |

硬件安全注意：

- MAX98357A 的输入脚只允许接 ESP32-S3 的 3.3V GPIO，不得接入 5V 信号。
- 扬声器播放、Wi-Fi 发射和屏幕背光同时工作时电流峰值会升高，建议使用稳定 5V/2A 供电。
- 当前总排线让扬声器与 INMP441 共用 BCLK/WS；首轮调试不要同时录音和播放，后续全双工再统一音频驱动。
- 如果只听到很短的爆音或完全静音，优先检查 SD/EN 是否被拉低、GND 是否可靠、BCLK/LRC/DIN 是否接反。

## 固件与 Web 实现

- 固件组件：`components/speaker`。
- Web 页面：`web/src/App.tsx` 中的“扬声器测试”页。
- USB JSON Lines 命令：
  - `get_tts_config`
  - `set_tts_config`
  - `clear_tts_key`
  - `tts_play`
  - `tts_status`
  - `tts_stop`
- TTS Key 保存到设备 NVS，串口和 Web 响应只返回 `hasApiKey` 与 `apiKeyPreview`，不得回显明文 Key。
- 设备播放使用云端返回的 `16-bit PCM / mono / 24000Hz`，再通过 I2S 写入 MAX98357A。

## 2026-05-24 实测结论

已执行并通过：

- `npm run build`
- `idf.py build`
- `idf.py -p COM16 flash`
- COM16 串口读取 `get_tts_config` 和 `tts_status`

烧录结果：

- 设备识别为 ESP32-S3 rev v0.2。
- Flash 为 8MB，PSRAM 为 8MB。
- 固件写入和校验成功。

串口确认：

- `get_tts_config` 可正常返回 TTS 配置。
- TTS 配置已规范化为：
  - URL：`https://api.siliconflow.cn/v1/audio/speech`
  - model：`fnlp/MOSS-TTSD-v0.5`
  - voice：`fnlp/MOSS-TTSD-v0.5:alex`
  - sample rate：`24000`
- `hasApiKey=true` 时只显示 Key 预览，不回显明文。

## TTS HTTP 400 根因与处理

用户实测曾出现：

```text
TTS HTTP 状态异常：400
```

当时配置为 SiliconFlow 的 `fnlp/MOSS-TTSD-v0.5`，但音色仍是 OpenAI 风格的 `alloy`。该组合不兼容，容易触发服务端 400。

当前处理：

- 默认 TTS URL 改为 SiliconFlow `/v1/audio/speech`。
- 默认模型改为 `fnlp/MOSS-TTSD-v0.5`。
- 默认音色改为 `fnlp/MOSS-TTSD-v0.5:alex`。
- 固件会把 MOSS 模型下残留的 `alloy` 自动回退为 `fnlp/MOSS-TTSD-v0.5:alex`。
- 固件会把 MOSS 模型下的短音色名如 `alex` 自动补全为 `fnlp/MOSS-TTSD-v0.5:alex`。
- 非 2xx HTTP 响应会截取一小段服务端响应摘要放入错误信息，方便继续排查。

## 开头杂音/音乐/无用发音判断

用户完成测试后反馈：文字前面会出现较多杂音、音乐或无用发音。

当前判断：

- 如果浏览器试听和设备播放都有相似的前摇、音乐、人声或无用发音，优先判断为 TTS 模型/音色输出问题。
- 如果浏览器试听干净，只有 MAX98357A 设备播放开头有爆音或刺啦声，优先判断为设备链路问题。
- 硬件或 PCM 格式问题通常表现为爆音、刺啦声、速度不对、音调异常；模型问题更容易表现为“像正常音频一样的音乐、废话、前置人声”。

建议排查顺序：

1. 在 Web 扬声器测试页先用“浏览器试听”听同一段文本。
2. 如果浏览器也有前摇，换 TTS 模型或音色，不要优先改 I2S。
3. 如果浏览器正常而设备异常，检查 SD/EN、GND、BCLK/LRC/DIN，并考虑在设备播放前写入 100-200ms 静音 PCM。
4. 如果只在短文本中明显，尝试把测试句改成完整自然句，减少“测试、测试”这类极短提示。

## TTS 时长明显长于文字的判断

用户后续反馈：TTS 播放长度经常比文字预期更长。

当前判断优先级：

- 如果浏览器 MP3 试听也明显偏长，优先判断为 TTS 模型/音色生成问题。常见表现是模型在正文前后生成停顿、音乐、无意义人声或拖长尾音。
- 如果浏览器试听时长正常，但设备播放时长偏长，优先检查 PCM 播放参数是否不匹配。例如云端实际返回采样率不是 24000Hz，而设备仍按 24000Hz 播放，会造成速度和时长异常。
- 当前设备端时长计算为 `audioBytes / (24000 * sizeof(int16_t))`，默认假设云端返回 `16-bit mono PCM 24000Hz`。如果切换其他 TTS 服务或模型，必须重新确认返回 PCM 的采样率、声道数和位宽。
- 如果听感像“语速正常但多了前后无用内容”，更偏模型问题；如果听感像“整体变慢、音调变低”，更偏采样率/格式问题。

建议后续优化：

1. Web 浏览器试听仍作为模型输出对照样本。
2. 设备端可增加返回音频的前 32 字节检测，避免把 WAV/MP3 误当裸 PCM 播放。
3. 如果服务端支持更稳定的音色或模型，优先更换 TTS 模型，而不是在设备端裁剪音频。

## 后续改动提醒

- 修改 `FRIDGE_TTS_SAMPLE_RATE` 时必须同时确认云端 `sample_rate` 和 I2S STD clock 配置一致。
- 不要把 `response_format` 改成 `mp3` 后直接送给 MAX98357A；设备播放路径当前不解码 MP3，只播放 PCM。
- 如果需要支持更多 TTS 服务，应为不同服务增加明确的请求格式适配，不要假设所有 `/audio/speech` 都兼容同一套字段。
- 如果要消除设备开头 pop 音，优先做小改动：播放前 enable I2S 后写一段全 0 静音 PCM，再写正文音频。
