# ESP32-S3-DevKitC-1 总排线方案（N8R8 安全修订版）

本文按 `ESP32-S3-DevKitC-1 N8R8` 重新修订。你刚才的启动日志已经确认开发板使用 `8MB Octal PSRAM`，因此本版明确避开 `GPIO35 / GPIO36 / GPIO37`，不再把它们接到 OV3660。

当前目标：一块开发板同时接上屏幕、触摸、OV3660 摄像头、INMP441 麦克风、MAX98357A 扬声器、人体感知雷达、光敏传感器、MPU6050/6 轴陀螺仪。

## 1. 关键结论

- `GPIO35 / GPIO36 / GPIO37`：**禁止接外设**。N8R8 上它们与 Octal Flash/PSRAM 相关，你遇到的 `MSPI Timing tuning fail + RTCWDT_RTC_RST` 循环重启，很可能就和这三个脚被摄像头占用有关。
- `GPIO0`：本版不再使用。它是启动绑带脚，接摄像头数据线风险太高。
- `GPIO48`：本版仍暂用作 OV3660 `D5`，但标为高风险，因为开发板板载 RGB LED 也接在 `GPIO48`。如果摄像头 DVP 不稳定，优先把它列为替换对象。
- `GPIO3 / GPIO45 / GPIO46`：本版仍用于 OV3660 数据线，属于启动敏感脚。不能只靠软件规避，需要硬件上避免摄像头在复位瞬间强拉固定电平。
- 雷达 `OT2` 本版先不接，释放 `GPIO19` 给 OV3660 `PCLK`。
- 触摸 `TP_RST` 本版先不接，释放 `GPIO16` 给 OV3660 `D7`。FT6336U 通常可以依赖上电复位，后续如触摸异常再补复位脚。
- 这是一份整机同时接线方案，不是“屏幕/摄像头二选一”。

## 2. 风险等级说明

| 等级 | 含义 | 处理方式 |
| --- | --- | --- |
| 低 | 常规 3.3V GPIO 或 ADC/I2C/I2S/UART 接线 | 按表接线即可，注意共地 |
| 中 | 共享总线、时钟线或默认功能被占用 | 接线可行，但要注意软件初始化顺序 |
| 高 | 启动敏感脚、板载器件共用脚、USB/JTAG 相关脚 | 可以试，但必须先断电复核；建议串联 `220Ω~1kΩ` 电阻 |
| 禁用 | N8R8 上不可外接或已导致启动风险 | 不接任何外设 |

## 3. 供电总表

| 部件 | 供电 | 风险 | 说明 |
| --- | --- | --- | --- |
| 屏幕 TS040HDS02CP-B1620A | VCC -> 5V | 中 | 屏幕主电源 5V；所有信号线仍为 3.3V 逻辑 |
| 触摸 FT6336U | VCC -> 3V3 | 低 | I2C 逻辑 3.3V |
| OV3660 摄像头 | VDD / DVDD1.5V -> 1.5V | 高 | 必须接 1.5V 稳压，不能误接 3.3V |
| OV3660 摄像头 | VDD2.8V / DVDD2.8V / AVDD / IOVDD -> 3V3 | 中 | 按你确认的模组参数可接 3.3V |
| INMP441 麦克风 | VDD -> 3V3 | 低 | I2S 逻辑 3.3V |
| MAX98357A 扬声器功放 | VIN -> 5V | 中 | BCLK/WS/DIN 仍为 3.3V 逻辑 |
| 人体感知雷达 | VCC -> 3V3 | 中 | 若模块只支持 5V 供电，必须确认 UART 输出仍是 3.3V |
| 光敏传感器 | VCC -> 3V3 | 低 | AO 不能超过 3.3V |
| MPU6050 / 6 轴 IMU | VCC -> 3V3 | 低 | I2C 逻辑 3.3V |

所有模块必须共地。

## 4. 开发板俯视接线图

说明：按你提供的开发板两列 GPIO 顺序绘制。USB 口在上方。

```text
                                      USB / Type-C
                          ┌────────────────────────────┐
                          │ ESP32-S3-DevKitC-1 N8R8 顶视 │
┌─────────────────────────┤                            ├──────────────────────────┐
│ 左侧排针，从上到下       │                            │ 右侧排针，从上到下        │
│                         │                            │                          │
│ GPIO4  ────────────────┼── I2C/SCCB SDA              │ GPIO1  ────────────────┼── 光敏 AO
│ GPIO5  ────────────────┼── I2C/SCCB SCL              │ GPIO2  ────────────────┼── OV3660 VSYNC
│ GPIO6  ────────────────┼── LCD WAIT#                 │ GPIO42 ────────────────┼── 麦克风 SD
│ GPIO7  ────────────────┼── LCD RESET#                │ GPIO41 ────────────────┼── I2S WS: 麦克风 WS / 扬声器 LRC
│ GPIO15 ────────────────┼── 触摸 TP_INT               │ GPIO40 ────────────────┼── I2S BCLK: 麦克风 SCK / 扬声器 BCLK
│ GPIO16 ────────────────┼── OV3660 D7                 │ GPIO39 ────────────────┼── 扬声器 DIN
│ GPIO17 ────────────────┼── OV3660 D8                 │ GPIO38 ────────────────┼── OV3660 HREF / HS
│ GPIO18 ────────────────┼── OV3660 D9                 │ GPIO37 ────────────────┼── 禁用：N8R8 Flash/PSRAM 相关
│ GPIO8  ────────────────┼── OV3660 D2                 │ GPIO36 ────────────────┼── 禁用：N8R8 Flash/PSRAM 相关
│ GPIO3  ────────────────┼── OV3660 D3（高风险）       │ GPIO35 ────────────────┼── 禁用：N8R8 Flash/PSRAM 相关
│ GPIO46 ────────────────┼── OV3660 D4（高风险）       │ GPIO0  ────────────────┼── 高风险预留，不接
│ GPIO9  ────────────────┼── LCD QSPI D3               │ GPIO45 ────────────────┼── OV3660 D6（高风险）
│ GPIO10 ────────────────┼── LCD QSPI CS#              │ GPIO48 ────────────────┼── OV3660 D5（高风险：板载 RGB LED）
│ GPIO11 ────────────────┼── LCD QSPI D0               │ GPIO47 ────────────────┼── OV3660 XCLK / MCLK
│ GPIO12 ────────────────┼── LCD QSPI SCLK             │ GPIO21 ────────────────┼── 雷达 TX -> ESP32 RX
│ GPIO13 ────────────────┼── LCD QSPI D1               │ GPIO20 ────────────────┼── ESP32 TX -> 雷达 RX
│ GPIO14 ────────────────┼── LCD QSPI D2               │ GPIO19 ────────────────┼── OV3660 PCLK
└─────────────────────────┴────────────────────────────┴──────────────────────────┘

电源排针另接：
3V3 -> 触摸 / OV3660 VDD2.8V、IOVDD、AVDD / 麦克风 / 雷达 / 光敏 / MPU6050
5V  -> 屏幕 VCC / MAX98357A VIN
GND -> 所有模块 GND 共地
1.5V 稳压输出 -> OV3660 VDD / DVDD1.5V
```

## 5. 开发板为主体的 GPIO 总表

| ESP32-S3 GPIO | 连接对象 | 信号 | 风险 | 说明 |
| --- | --- | --- | --- | --- |
| GPIO1 | 光敏传感器 | AO / ADC1_CH0 | 低 | 光敏模块必须 3.3V 供电 |
| GPIO2 | OV3660 | VSYNC / VS | 中 | 原生功能少，适合替代 GPIO35 |
| GPIO3 | OV3660 | D3 | 高 | 启动敏感脚，建议串 `220Ω~1kΩ` |
| GPIO4 | 触摸 / MPU6050 / OV3660 | I2C/SCCB SDA | 中 | 三个设备共用，总线上拉到 3.3V |
| GPIO5 | 触摸 / MPU6050 / OV3660 | I2C/SCCB SCL | 中 | 三个设备共用，总线上拉到 3.3V |
| GPIO6 | 屏幕 | LCD WAIT# | 低 | 屏幕忙信号输入 |
| GPIO7 | 屏幕 | LCD RESET# | 低 | 屏幕复位输出 |
| GPIO8 | OV3660 | D2 / esp32-camera pin_d0 | 中 | 摄像头 DVP 有效 8-bit 窗口最低位 |
| GPIO9 | 屏幕 | LCD QSPI D3 | 低 | 屏幕 QSPI |
| GPIO10 | 屏幕 | LCD QSPI CS# | 低 | 屏幕 QSPI |
| GPIO11 | 屏幕 | LCD QSPI D0 | 低 | 屏幕 QSPI |
| GPIO12 | 屏幕 | LCD QSPI SCLK | 低 | 屏幕 QSPI |
| GPIO13 | 屏幕 | LCD QSPI D1 | 低 | 屏幕 QSPI |
| GPIO14 | 屏幕 | LCD QSPI D2 | 低 | 屏幕 QSPI |
| GPIO15 | 触摸 | TP_INT | 低 | 触摸中断；若引脚不够可后续改为轮询 |
| GPIO16 | OV3660 | D7 / esp32-camera pin_d5 | 中 | 原触摸 TP_RST 让出；触摸复位不接 |
| GPIO17 | OV3660 | D8 / esp32-camera pin_d6 | 低 | 摄像头 DVP 有效 8-bit 窗口高位 |
| GPIO18 | OV3660 | D9 / esp32-camera pin_d7 | 低 | 摄像头 DVP 有效 8-bit 窗口最高位 |
| GPIO19 | OV3660 | PCLK | 高 | 会占用原生 USB D-；你使用 UART-USB 调试时可用 |
| GPIO20 | 人体雷达 | ESP32 TX -> 雷达 RX | 高 | 会占用原生 USB D+；你使用 UART-USB 调试时可用 |
| GPIO21 | 人体雷达 | 雷达 TX -> ESP32 RX | 低 | UART RX |
| GPIO35 | 禁用 | 不接 | 禁用 | N8R8 Flash/PSRAM 相关，不接外设 |
| GPIO36 | 禁用 | 不接 | 禁用 | N8R8 Flash/PSRAM 相关，不接外设 |
| GPIO37 | 禁用 | 不接 | 禁用 | N8R8 Flash/PSRAM 相关，不接外设 |
| GPIO38 | OV3660 | HREF / HS | 中 | 替代 GPIO36 |
| GPIO39 | MAX98357A | DIN | 中 | 与麦克风共享 BCLK/WS，但数据线独立 |
| GPIO40 | INMP441 / MAX98357A | I2S BCLK / SCK | 中 | 麦克风和扬声器共用时钟 |
| GPIO41 | INMP441 / MAX98357A | I2S WS / LRC | 中 | 麦克风和扬声器共用帧同步 |
| GPIO42 | INMP441 | SD / I2S RX data | 低 | 麦克风数据输入 |
| GPIO45 | OV3660 | D6 / esp32-camera pin_d4 | 高 | 启动敏感脚，建议串 `220Ω~1kΩ` |
| GPIO46 | OV3660 | D4 / esp32-camera pin_d2 | 高 | 启动敏感脚，建议串 `220Ω~1kΩ` |
| GPIO47 | OV3660 | XCLK / MCLK | 中 | 摄像头主时钟输出 |
| GPIO48 | OV3660 | D5 / esp32-camera pin_d3 | 高 | 板载 RGB LED 共用脚，DVP 不稳时优先替换 |
| GPIO0 | 预留 | 不接 | 高 | 启动绑带脚，本版不用于摄像头 |

## 6. 各部件接线明细

### 6.1 屏幕 TS040HDS02CP-B1620A（含触摸）

下表按图片中 20-pin FPC 表 B 的 Pin 1 -> Pin 20 顺序排列，便于直接对照排线方向。

| 屏幕 Pin | 屏幕 / 触摸引脚 | ESP32-S3 引脚 | 风险 | 说明 |
| --- | --- | --- | --- | --- |
| 1 | GND | GND | 低 | 屏幕地，必须与开发板、摄像头和传感器共地 |
| 2 | VCC | 5V | 中 | 屏幕主电源，不能接 3V3 替代；建议使用稳定 5V/2A 供电 |
| 3 | GND | GND | 低 | 屏幕地 |
| 4 | CS# | GPIO10 | 低 | TR230S QSPI 片选，低有效 |
| 5 | GND | GND | 低 | 屏幕地，建议就近接地降低 QSPI 干扰 |
| 6 | SDO1 | GPIO13 | 低 | QSPI D1 |
| 7 | SDO0 | GPIO11 | 低 | QSPI D0 |
| 8 | SCL / SCLK | GPIO12 | 低 | QSPI 时钟 |
| 9 | SDO3 | GPIO9 | 低 | QSPI D3 |
| 10 | SDO2 | GPIO14 | 低 | QSPI D2 |
| 11 | RESET# | GPIO7 | 低 | TR230S / LCD 低有效复位 |
| 12 | WAIT# | GPIO6 | 低 | TR230S 忙信号，1 表示允许继续发送命令 |
| 13 | QSPI-INT | 不接 | 低 | 本项目由 ESP32-S3 直接读 FT6336U，QSPI-INT 暂不使用 |
| 14 | D/C# | 不接 | 低 | QSPI-4SDA 模式不需要 D/C#；仅 SPI-4WIRE 调试模式才需要 |
| 15 | TP-INT | GPIO15 | 低 | FT6336U 触摸中断 |
| 16 | TP-SDA | GPIO4 | 中 | FT6336U I2C SDA，与 MPU6050、OV3660 SCCB 共用；上拉只能到 3.3V |
| 17 | TP-SCL | GPIO5 | 中 | FT6336U I2C SCL，与 MPU6050、OV3660 SCCB 共用；上拉只能到 3.3V |
| 18 | TP-RST | 不接 | 中 | 本版让出 GPIO16 给摄像头 D7；触摸依赖上电复位 |
| 19 | TP-VCC | 3V3 | 中 | 触摸侧 3.3V 供电；若转接板实物已标 NC/共用供电，以实物丝印和供应商说明为准，禁止接 5V 到触摸逻辑 |
| 20 | TP-GND | GND | 低 | 触摸地，与系统共地 |

### 6.2 OV3660 摄像头

| 摄像头 Pin | 摄像头信号 | ESP32-S3 / 电源连接 | 风险 | 说明 |
| --- | --- | --- | --- | --- |
| 1 | NC | 不接 | 低 | 空脚 |
| 2 | AGND | GND | 低 | 模拟地 |
| 3 | SDA / SIOD | GPIO4 | 中 | SCCB 数据，和 I2C 总线共用 |
| 4 | AVDD / VDD2.8V | 3V3 | 中 | 按你确认接 3.3V |
| 5 | SCL / SIOC | GPIO5 | 中 | SCCB 时钟，和 I2C 总线共用 |
| 6 | RESET | 不接 | 中 | 当前不占 GPIO；若模组不起振再补 |
| 7 | VS / VSYNC | GPIO2 | 中 | 从 GPIO35 改到 GPIO2，避开 PSRAM |
| 8 | PWDN | 不接 | 中 | 当前不占 GPIO；若模组默认掉电再补 |
| 9 | HS / HREF | GPIO38 | 中 | 从 GPIO36 改到 GPIO38，避开 PSRAM |
| 10 | DVDD1.5V / VDD | 1.5V | 高 | 核心电源，必须接 1.5V 稳压输出 |
| 11 | DVDD2.8V / VDD2.8V | 3V3 | 中 | 按你确认可接 3.3V |
| 12 | D9 | GPIO18 | 低 | esp32-camera pin_d7，8-bit DVP 有效窗口最高位 |
| 13 | MCLK / XCLK | GPIO47 | 中 | 主时钟 |
| 14 | D8 | GPIO17 | 低 | esp32-camera pin_d6，8-bit DVP 有效窗口高位 |
| 15 | DGND | GND | 低 | 数字地 |
| 16 | D7 | GPIO16 | 中 | esp32-camera pin_d5，从 GPIO0 改到 GPIO16 |
| 17 | PCLK | GPIO19 | 高 | 从 GPIO37 改到 GPIO19；占用原生 USB D- |
| 18 | D6 | GPIO45 | 高 | esp32-camera pin_d4，启动敏感脚，建议串联电阻 |
| 19 | D2 | GPIO8 | 中 | esp32-camera pin_d0，8-bit DVP 有效窗口最低位 |
| 20 | D5 | GPIO48 | 高 | esp32-camera pin_d3，板载 RGB LED 共用脚，线尽量短 |
| 21 | D3 | GPIO3 | 高 | esp32-camera pin_d1，启动敏感脚，建议串联电阻 |
| 22 | D4 | GPIO46 | 高 | esp32-camera pin_d2，启动敏感脚，建议串联电阻 |
| 23 | D1 | 不接 | 低 | 10-bit 输出低位，当前 8-bit DVP 不接 |
| 24 | D0 | 不接 | 低 | 10-bit 输出低位，当前 8-bit DVP 不接 |
| LED | LED | 不接 | 低 | 按你的要求 LED 不处理 |

### 6.3 INMP441 麦克风

| 麦克风引脚 | ESP32-S3 引脚 | 风险 | 说明 |
| --- | --- | --- | --- |
| VDD | 3V3 | 低 | 3.3V 供电 |
| GND | GND | 低 | 共地 |
| SCK | GPIO40 | 中 | I2S BCLK，与扬声器 BCLK 共用 |
| WS | GPIO41 | 中 | I2S WS，与扬声器 LRC 共用 |
| SD | GPIO42 | 低 | 麦克风数据输出到 ESP32 |
| L/R | GND | 低 | 左声道 |

### 6.4 MAX98357A 扬声器功放

| 扬声器引脚 | ESP32-S3 引脚 | 风险 | 说明 |
| --- | --- | --- | --- |
| VIN / VCC | 5V | 中 | 功放电源 |
| GND | GND | 低 | 共地 |
| BCLK | GPIO40 | 中 | I2S BCLK，与麦克风 SCK 共用 |
| LRC / WS | GPIO41 | 中 | I2S WS，与麦克风 WS 共用 |
| DIN | GPIO39 | 中 | I2S 播放数据 |
| SD / EN | 3V3 或悬空 | 中 | 建议固定使能 |
| GAIN | 悬空 | 低 | 默认增益 |

注意：当前代码仍是麦克风 RX 与扬声器 TX 两套 I2S 测试组件。硬件时钟线可以并接，但首轮调试建议不要一边录音一边播放。

### 6.5 人体感知雷达

| 雷达引脚 | ESP32-S3 引脚 | 风险 | 说明 |
| --- | --- | --- | --- |
| VCC | 3V3 | 中 | 按当前方案使用 3.3V |
| GND | GND | 低 | 共地 |
| TX | GPIO21 | 低 | 雷达 TX -> ESP32 UART RX |
| RX | GPIO20 | 高 | ESP32 UART TX -> 雷达 RX；占用原生 USB D+ |
| OT2 | 不接 | 中 | 本版释放 GPIO19 给摄像头 PCLK |

### 6.6 光敏传感器

| 光敏引脚 | ESP32-S3 引脚 | 风险 | 说明 |
| --- | --- | --- | --- |
| VCC | 3V3 | 低 | 仅 3.3V 供电 |
| GND | GND | 低 | 共地 |
| AO | GPIO1 | 低 | ADC1_CH0 |

### 6.7 6 轴陀螺仪 MPU6050

| MPU6050 引脚 | ESP32-S3 引脚 | 风险 | 说明 |
| --- | --- | --- | --- |
| VCC | 3V3 | 低 | 仅 3.3V 供电 |
| GND | GND | 低 | 共地 |
| SDA | GPIO4 | 中 | I2C SDA，与触摸、OV3660 SCCB 共用 |
| SCL | GPIO5 | 中 | I2C SCL，与触摸、OV3660 SCCB 共用 |
| AD0 | GND 或悬空 | 低 | 默认地址 `0x68` |
| INT | 不接 | 低 | 当前代码未使用中断 |

## 7. GPIO 风险清单

| GPIO | 当前用途 | 风险 | 建议 |
| --- | --- | --- | --- |
| GPIO35/36/37 | 不接 | 禁用 | N8R8 上不要接任何外设；如果已经接了摄像头，先断电拔掉 |
| GPIO0 | 不接 | 高 | 启动绑带脚，本版不用；避免再次分配给摄像头 DVP |
| GPIO3 | OV3660 D3 | 高 | 摄像头线串 `220Ω~1kΩ`，观察是否影响启动 |
| GPIO45 | OV3660 D6 | 高 | 摄像头线串 `220Ω~1kΩ`，避免复位瞬间强拉 |
| GPIO46 | OV3660 D4 | 高 | 摄像头线串 `220Ω~1kΩ`，避免复位瞬间强拉 |
| GPIO48 | OV3660 D5 | 高 | 板载 RGB LED 共用脚；若拍照花屏或初始化异常，优先换脚或移除板载 LED 影响 |
| GPIO19/20 | OV3660 PCLK / 雷达 TX | 高 | 占用原生 USB D-/D+；你用 UART-USB 调试时可接受，不要再同时使用原生 USB Serial/JTAG |
| GPIO40/41 | I2S 共用时钟 | 中 | 首轮不要同时录音和播放 |
| GPIO4/5 | I2C/SCCB 共用 | 中 | 总线上拉只到 3.3V，线尽量短 |

## 8. 上电检查顺序

1. **断电拔掉 GPIO35/36/37 上所有线**，确认它们完全悬空。
2. 只接开发板、屏幕、传感器、麦克风、扬声器、雷达 UART，不接 OV3660 DVP，先确认 Web `get_status` 正常。
3. 接 OV3660 电源但不接 DVP：确认 `1.5V`、`3.3V`、GND 没有短路，摄像头模块不发热。
4. 接 OV3660 SCCB：`GPIO4/GPIO5`，确认 I2C/SCCB 不拖死触摸和 MPU6050。
5. 接 OV3660 `XCLK=GPIO47`，再接同步脚：`VSYNC=GPIO2`、`HREF=GPIO38`、`PCLK=GPIO19`。
6. 最后接 D2-D9 数据线：`D2->GPIO8`、`D3->GPIO3`、`D4->GPIO46`、`D5->GPIO48`、`D6->GPIO45`、`D7->GPIO16`、`D8->GPIO17`、`D9->GPIO18`；`D0/D1` 悬空。
7. 如果再次出现 `MSPI Timing tuning fail` 或 `RTCWDT_RTC_RST` 循环，优先断开摄像头高风险脚：`GPIO48`、`GPIO45`、`GPIO46`、`GPIO3`。

## 9. 当前文档与代码同步提醒

本文档已经给出 N8R8 安全修订版排线。若固件代码仍是上一版映射，需要同步修改摄像头和雷达相关 GPIO：

```text
OV3660 VSYNC: GPIO35 -> GPIO2
OV3660 HREF : GPIO36 -> GPIO38
OV3660 PCLK : GPIO37 -> GPIO19
OV3660 D7   : GPIO0  -> GPIO16
雷达 OT2    : GPIO19 -> 不接
触摸 TP_RST : GPIO16 -> 不接
```

在代码同步前，不要按本文档接完摄像头后直接点拍照；否则固件仍会按旧 GPIO 读摄像头。
