// 冰箱小精灵 LVGL UI 公共接口。
// UI 只在正常主控模式启动，独立屏幕测试和摄像头测试模式不加载本组件。

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FRIDGE_UI_PAGE_STANDBY = 0,
    FRIDGE_UI_PAGE_HOME,
    FRIDGE_UI_PAGE_ZONE,
    FRIDGE_UI_PAGE_EDIT_FOOD,
    FRIDGE_UI_PAGE_DOOR,
    FRIDGE_UI_PAGE_CAMERA,
    FRIDGE_UI_PAGE_CAMERA_RESULT,
    FRIDGE_UI_PAGE_RECIPE,
    FRIDGE_UI_PAGE_SHOPPING,
    FRIDGE_UI_PAGE_SETTINGS,
    FRIDGE_UI_PAGE_WIFI,
    FRIDGE_UI_PAGE_MORE,
    FRIDGE_UI_PAGE_OFFLINE,
    FRIDGE_UI_PAGE_COUNT,
} fridge_ui_page_t;

esp_err_t fridge_ui_init(void);
esp_err_t fridge_ui_set_page(fridge_ui_page_t page);
esp_err_t fridge_ui_set_brightness(uint8_t percent);
void fridge_ui_request_standby(void);

#ifdef __cplusplus
}
#endif
