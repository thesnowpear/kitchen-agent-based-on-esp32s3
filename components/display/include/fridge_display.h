// 冰箱小精灵 TR230S 屏幕驱动公共接口。
// 本组件只封装 TS040HDS02CP-B1620A 的 QSPI 刷新和 TR230S 亮度寄存器，不初始化 LVGL 页面。

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_DISPLAY_WIDTH 720
#define FRIDGE_DISPLAY_HEIGHT 720

// 当前 N8R8 安全排线：LCD RESET# 固定为 GPIO7，GPIO8 已让给 OV3660 D2，不能再按旧计划接 LCD 复位。
esp_err_t fridge_display_init(void);

// 刷新一个闭区间矩形区域，像素输入为 LVGL/主机端常见 little-endian RGB565。
// 注意：TR230S 写显存阶段需要 WAIT# 允许后再发命令，调用方应避免跨任务并发刷新。
esp_err_t fridge_display_flush_area(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, const uint16_t *rgb565_pixels);

// 设置屏幕亮度百分比并保存到 NVS。屏幕没有独立 BL GPIO，亮度通过 TR230S 0x20 寄存器控制。
esp_err_t fridge_display_set_brightness(uint8_t percent);

// 读取当前缓存的亮度百分比；未初始化时返回默认值。
uint8_t fridge_display_get_brightness(void);

// 返回显示驱动是否已经完成 QSPI 和 TR230S 初始化。
bool fridge_display_is_ready(void);

#ifdef __cplusplus
}
#endif
