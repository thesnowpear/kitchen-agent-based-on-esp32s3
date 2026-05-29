// 冰箱小精灵 AI 动作执行公共接口。
// 本组件只解析和执行白名单低风险动作；页面切换通过 UI 注册回调异步投递，避免后台任务直接触碰 LVGL。

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_AI_ACTION_MAX_TOOL_LEN 16
#define FRIDGE_AI_ACTION_MAX_ACTION_LEN 24
#define FRIDGE_AI_ACTION_MAX_PAGE_LEN 24
#define FRIDGE_AI_ACTION_MAX_MESSAGE_LEN 160

typedef esp_err_t (*fridge_ai_action_ui_page_cb_t)(const char *page_key, const char *toast);

// AI 动作执行结果：用于 USB/Web、屏幕 AI 页和语音链路统一反馈。
typedef struct {
    bool executed;
    bool needs_confirmation;
    char tool[FRIDGE_AI_ACTION_MAX_TOOL_LEN + 1];
    char action[FRIDGE_AI_ACTION_MAX_ACTION_LEN + 1];
    char page[FRIDGE_AI_ACTION_MAX_PAGE_LEN + 1];
    char message[FRIDGE_AI_ACTION_MAX_MESSAGE_LEN + 1];
} fridge_ai_action_result_t;

// 注册 UI 页面切换回调。UI 组件初始化时注册；未注册时 UI 动作会安全失败。
void fridge_ai_actions_register_ui_page_handler(fridge_ai_action_ui_page_cb_t cb);

// 执行 AI 回复中的白名单 JSON 指令；非法或不支持的动作不会改变设备状态。
esp_err_t fridge_ai_actions_execute_json(const char *reply, fridge_ai_action_result_t *out);

// 从 AI 回复中剥离被执行的紧凑 JSON 指令，得到用户可见回复。
esp_err_t fridge_ai_actions_strip_directives(const char *reply,
                                             char *visible,
                                             size_t visible_size,
                                             fridge_ai_action_result_t *out);

#ifdef __cplusplus
}
#endif
