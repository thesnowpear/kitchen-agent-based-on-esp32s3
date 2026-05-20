#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 启动 USB/串口 JSON Lines 协议任务。
// 注意：该组件只读写控制台串口，不控制 GPIO；Web Serial 波特率建议 115200。
esp_err_t fridge_usb_protocol_start(void);

#ifdef __cplusplus
}
#endif
