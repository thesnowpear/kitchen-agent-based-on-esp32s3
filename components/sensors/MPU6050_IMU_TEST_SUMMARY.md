# MPU6050 陀螺仪模块测试总结

更新时间：2026-05-23 16:05:09 +08:00

本文给后续 AI 和协作者快速读取，记录本项目 MPU6050/IMU 陀螺仪模块的接线、固件入口、Web 面板测试路径、已验证结果和排查经验。

## 一句话结论

MPU6050 已在 ESP32-S3-DevKitC-1 N8R8 上通过真实硬件测试。设备端 `get_sensors` 能稳定返回 `imuReady=true`、`imuError=0`、`WHO_AM_I=0x68`，Web 面板已能在“传感器”页以约 `300 ms` 间隔刷新并展示 `Gyro X/Y/Z`、`Accel X/Y/Z` 和姿态角。

## 硬件接线

当前 MPU6050 使用 ESP32-S3 的 I2C0：

| MPU6050 模块 | ESP32-S3-DevKitC-1 N8R8 | 说明 |
| --- | --- | --- |
| VCC | 3V3 | 使用 3.3 V 供电，避免 5 V IO 风险 |
| GND | GND | 必须共地 |
| SDA | GPIO4 | `FRIDGE_IMU_I2C_SDA_GPIO` |
| SCL | GPIO5 | `FRIDGE_IMU_I2C_SCL_GPIO` |
| AD0 | GND 或悬空为低 | 当前地址 `0x68` |

硬件注意：

- I2C 上拉应到 3.3 V，不能上拉到 5 V。
- 当前 I2C 时钟为 `100 kHz`，先以稳定为主，后续确认无误再考虑提速。
- 避免把 MPU6050 接到启动绑带脚或 Flash/PSRAM 相关 GPIO。

## 固件位置

主要文件：

- `components/sensors/fridge_sensors.c`
- `components/sensors/include/fridge_sensors.h`
- `components/usb_protocol/fridge_usb_protocol.c`
- `web/src/App.tsx`
- `web/src/types.ts`

关键配置：

- I2C 端口：`FRIDGE_IMU_I2C_PORT = I2C_NUM_0`
- SDA：`FRIDGE_IMU_I2C_SDA_GPIO = 4`
- SCL：`FRIDGE_IMU_I2C_SCL_GPIO = 5`
- 默认地址：`FRIDGE_IMU_DEFAULT_ADDR = 0x68`
- 期望 ID：`IMU_EXPECTED_WHO_AM_I = 0x68`
- 采样任务周期：`IMU_SAMPLE_PERIOD_MS = 20`

初始化流程：

1. `fridge_sensors_init()` 初始化光敏 ADC 和 MPU6050 I2C。
2. `imu_init()` 配置 I2C0，读取 `WHO_AM_I`，确认 `0x68`。
3. 复位 MPU6050，退出休眠。
4. 设置低量程：
   - 加速度：`±2 g`，换算系数 `raw / 16384.0`
   - 陀螺仪：`±250 °/s`，换算系数 `raw / 131.0`
5. `sensor_task` 后台周期采样，并更新 `fridge_sensor_snapshot_t` 快照。

## USB/Web Serial 协议

Web 面板和调试脚本通过 JSON Lines 发送：

```json
{"type":"request","request_id":"gyro-test-1","command":"get_sensors"}
```

关键返回字段：

```json
{
  "imuReady": true,
  "imuAddress": 104,
  "imuWhoAmI": 104,
  "imuError": 0,
  "accelXG": -0.0040,
  "accelYG": -0.0900,
  "accelZG": 1.1200,
  "gyroXDps": -1.2595,
  "gyroYDps": 1.0000,
  "gyroZDps": -0.0916,
  "imuTemperatureC": 28.0,
  "pitchDeg": -4.59,
  "rollDeg": 0.21,
  "angleDelta": 4.80,
  "vibrationPeak": 1.1200
}
```

说明：

- `imuAddress` 和 `imuWhoAmI` 用十进制返回，`104` 对应十六进制 `0x68`。
- `gyroXDps/Y/ZDps` 单位为 `°/s`。
- `accelXG/Y/ZG` 单位为 `g`。
- `pitchDeg`、`rollDeg` 由加速度估算，适合静态姿态参考，不是完整姿态融合算法。
- `fridge_sensors_get_snapshot()` 只读最近快照，不直接访问硬件，适合 Web 高频轮询。

## Web 面板测试路径

1. 启动 Web 面板：`web/` 下执行 `npm run dev`。
2. 打开 `http://127.0.0.1:5173/`。
3. 选择 `USB` 模式并连接 `COM16`。
4. 进入“传感器”页面。
5. 轻晃或旋转 MPU6050，观察：
   - `Gyro X/Y/Z`
   - `陀螺仪模长`
   - `Accel X/Y/Z`
   - `Pitch`
   - `Roll`
   - `IMU 就绪`
   - `WHO_AM_I`

已修复的 Web 问题：

- 之前全局刷新约 `15 s`，短时间晃动时看不到陀螺仪变化。
- 之前传感器页主要显示 `angleDelta`、`vibrationPeak` 等聚合值，没有直接展示原始 `Gyro X/Y/Z`。
- 现在传感器页激活时单独以约 `300 ms` 轮询 `get_sensors`，并使用独立 in-flight 标记避免串口请求堆积。

## 已验证结果

直接通过 COM16 连续发送 `get_sensors`，已确认：

- 串口：`COM16`
- 波特率：`115200`
- 协议：JSON Lines
- `imuReady=true`
- `imuError=0`
- `imuWhoAmI=104`，即 `0x68`
- 三轴加速度和三轴陀螺仪均有返回值
- Web 构建：`npm run build` 通过
- Web 页面：“传感器”页可展示 `MPU6050 原始读数`、`Gyro X/Y/Z`、`陀螺仪模长`

一次实测样例：

```text
accel = [-0.004, -0.090, 1.120] g
gyro  = [-1.2595, 1.0000, -0.0916] °/s
pitch = -4.59°
roll  = 0.21°
```

静止状态下 Gyro 可能有小偏置，这是 MPU6050 常见现象；后续如果要做门体动作识别，应增加零偏校准和滤波。

## 快速排查表

| 现象 | 优先检查 |
| --- | --- |
| Web 面板完全超时 | 固件是否为正常 USB 运维模式，`CONFIG_FRIDGE_SCREEN_TEST` 应为 `n` |
| `imuReady=false` | I2C 接线、3.3 V 供电、GND、SDA/SCL 是否接反 |
| `imuWhoAmI` 不是 `0x68` | AD0 电平、模块型号是否为 MPU6050、I2C 地址是否变化 |
| `imuError` 非 0 | 对照 ESP-IDF `esp_err_t`，优先排查 I2C ACK、总线占用、线太长 |
| Web 看起来不变化 | 确认在“传感器”页，观察 `Gyro X/Y/Z`，不要只看聚合字段 |
| 静止时 Gyro 不为 0 | 正常零偏；需要校准，不代表读数失败 |
| 动作很大但变化小 | 确认是否真的旋转模块；加速度变化和陀螺仪变化物理含义不同 |

## 后续建议

- 增加 MPU6050 零偏校准：上电静止采样 1-3 秒，计算 `gyro_x/y/z` bias。
- 增加简单低通滤波和动作阈值，避免冰箱压缩机振动造成误判。
- 将门体开合判断做成状态机：光照突变为主，IMU 姿态/震动为辅，毫米波雷达只作靠近/有人上下文。
- 若后续改用 ICM-42688，不要复用 MPU6050 寄存器表，应单独建立驱动分支或抽象接口。

