# 24G 人体雷达组件说明

本文是 `components/radar/` 的快速入口，给后续 AI/协作者用来快速理解当前雷达实现。

## 1. 组件职责

这个组件负责把 24GHz UART 雷达模块的原始文本帧、二进制上报帧，统一整理成一个可给 Web、USB 和后续状态机复用的快照。

它不负责“识别门是否打开”，也不直接负责开门状态机；它只提供“目标是否存在、是否更像静态反射、是否更像人体、是否更像靠近”的雷达语义。

## 2. 硬件接线

当前已验证接线：

- 雷达 `VCC -> 3V3`
- 雷达 `GND -> GND`
- 雷达 `TX -> ESP32-S3 GPIO21`，作为 UART1 RX
- 雷达 `RX -> ESP32-S3 GPIO20`，作为 UART1 TX
- 雷达 `OT2` 不接，GPIO19 让给 OV3660 PCLK

注意事项：

- 只接受 3.3V 逻辑。
- 不要把 5V TTL 直接接到 ESP32-S3。
- 当前方案默认使用 UART1。
- 雷达结果只能作为靠近/有人上下文，不可单独当成开门依据。

## 3. 协议模式

组件支持两种模式：

- `normal`：保留文本输出，便于看厂商原始信息。
- `report`：切换到二进制上报帧，供 Web 雷达页和诊断命令使用。

常用 USB 命令：

- `radar_test_start`
- `radar_test_status`
- `radar_test_stop`

## 4. 快照字段语义

`fridge_radar_snapshot_t` 是当前组件对外的核心数据结构。

重要字段：

- `presence`：模块原始上报是否有目标。
- `threshold_presence`：是否命中厂商阈值链。
- `near_clutter`：太近的杂波/近场强反射。
- `static_clutter`：更像墙面、柜体、门板的静态反射。
- `human_candidate`：可能是人体，但还没升格成可靠人体。
- `stable_presence`：当前代码里表示“可靠人体级别”的结果。
- `within_1m`：更像 1 米内人体的结果。
- `approaching`：更像正在靠近的人。
- `distance_raw`：模块原始距离值。
- `gate_energy[]`：16 个距离门的能量数组。
- `motion_score`：多帧微动评分。
- `static_score`：静态反射评分。
- `human_score`：人体候选评分。
- `target_class`：语义分类字符串。
- `rejection_reason`：没升格的原因。

## 5. 当前判定链

当前逻辑大致是：

1. 先看模块是否上报目标。
2. 再看是否命中厂商阈值。
3. 再用 7 帧左右的历史观察门位、距离和能量变化。
4. 如果稳定且微动不足，优先判成静态反射。
5. 如果静态反射不成立，再看人体候选分数。
6. 只有人体候选、连续靠近证据、门位变化和距离变化都足够时，才升到可靠人体。

设计目的：

- 墙面不要轻易被判成人。
- 真有人靠近时不要只看单帧，要看连续趋势。
- 保留原始目标和静态反射，方便现场调参。

## 6. 调参位置

固件主调参点在 `components/radar/fridge_radar.c`，常见要看这些宏：

- `FRIDGE_RADAR_MOTION_MIN_SCORE`
- `FRIDGE_RADAR_HUMAN_CANDIDATE_MIN_SCORE`
- `FRIDGE_RADAR_RELIABLE_HUMAN_MIN_SCORE`
- `FRIDGE_RADAR_STATIC_CLUTTER_MOTION_MAX`
- `FRIDGE_RADAR_STATIC_CLUTTER_STABILITY_MIN`
- `FRIDGE_RADAR_APPROACH_MIN_FRAMES`
- `FRIDGE_RADAR_APPROACH_MIN_DISTANCE_DELTA`

如果墙面误报偏多，优先提高：

- `static_clutter` 相关门槛
- `approaching` 所需的连续下降证据

如果人体被压得太狠，优先放宽：

- `human_candidate` 的门槛
- `stable_presence` 的人体分数

## 7. Web 面板对应

Web 雷达页已经把这些状态拆开显示：

- 模块上报
- 目标状态
- 1 米内人体
- 正在靠近
- 近场杂波
- 静态反射
- 人体候选
- 语义分类
- 静态评分 / 人体评分 / 微动评分

因此后续改算法时，要同步保证 USB JSON 字段和 Web 侧字段名一致。

## 8. 已知坑

- `presence=true` 只能说明模块看见了什么，不代表是人。
- 静态墙面在门位稳定、能量强时，最容易骗过单一阈值。
- `approach_score` 不能只看门位变化，必须结合连续帧距离下降。
- 远门偶发跳变不应该直接升格成可靠人体。

## 9. 相关源码

- `components/radar/fridge_radar.c`
- `components/radar/include/fridge_radar.h`
- `components/usb_protocol/fridge_usb_protocol.c`
- `web/src/App.tsx`
- `web/src/types.ts`
