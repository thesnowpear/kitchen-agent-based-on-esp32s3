// 冰箱小精灵 FT6336U 触摸驱动公共接口。
// 当前硬件排线中 TP_RST 不接，GPIO16 已用于 OV3660 D7；触摸依赖上电复位和 I2C0 读取。

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_TOUCH_I2C_ADDR 0x48
#define FRIDGE_TOUCH_INT_GPIO 15

typedef struct {
    bool pressed;
    uint16_t x;
    uint16_t y;
    uint8_t points;
} fridge_touch_point_t;

// 初始化 FT6336U 最小读点驱动。I2C0 由 sensors 组件初始化，SDA/SCL 为 GPIO4/GPIO5。
esp_err_t fridge_touch_init(void);

// 读取最近触摸坐标；该函数只做一次 I2C 寄存器读取，不阻塞等待中断。
esp_err_t fridge_touch_read(fridge_touch_point_t *out);

#ifdef __cplusplus
}
#endif
