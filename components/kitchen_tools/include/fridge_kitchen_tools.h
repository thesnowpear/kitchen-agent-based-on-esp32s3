// 冰箱小精灵厨房计时工具公共接口。
// 本组件只做软件计时、闹钟持久化和到点提醒，不新增 GPIO 或外设引脚；扬声器播放沿用 speaker 组件的硬件安全约束。

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_KITCHEN_TOOL_MAX_LABEL_LEN 32
#define FRIDGE_KITCHEN_TOOL_MAX_MESSAGE_LEN 160
#define FRIDGE_KITCHEN_TOOL_MAX_ALARMS 5

typedef enum {
    FRIDGE_KITCHEN_TIMER_IDLE = 0,
    FRIDGE_KITCHEN_TIMER_RUNNING,
    FRIDGE_KITCHEN_TIMER_PAUSED,
    FRIDGE_KITCHEN_TIMER_RINGING,
} fridge_kitchen_timer_state_t;

typedef enum {
    FRIDGE_KITCHEN_STOPWATCH_IDLE = 0,
    FRIDGE_KITCHEN_STOPWATCH_RUNNING,
    FRIDGE_KITCHEN_STOPWATCH_PAUSED,
} fridge_kitchen_stopwatch_state_t;

// 倒计时状态快照：remaining_seconds 会按当前单调时钟实时折算。
typedef struct {
    fridge_kitchen_timer_state_t state;
    uint32_t duration_seconds;
    uint32_t remaining_seconds;
    char label[FRIDGE_KITCHEN_TOOL_MAX_LABEL_LEN + 1];
    int64_t started_at_ms;
    int64_t target_at_ms;
} fridge_kitchen_timer_snapshot_t;

// 秒表状态快照：elapsed_seconds 会按当前单调时钟实时折算。
typedef struct {
    fridge_kitchen_stopwatch_state_t state;
    uint32_t elapsed_seconds;
    int64_t started_at_ms;
} fridge_kitchen_stopwatch_snapshot_t;

// 闹钟状态：hour/minute 使用本地时区时间；持久化只保存启用状态和标签，不保存明文隐私外的额外数据。
typedef struct {
    uint8_t id;
    bool enabled;
    bool ringing;
    uint8_t hour;
    uint8_t minute;
    int64_t last_trigger_day;
    char label[FRIDGE_KITCHEN_TOOL_MAX_LABEL_LEN + 1];
} fridge_kitchen_alarm_t;

typedef struct {
    bool initialized;
    bool time_ready;
    fridge_kitchen_timer_snapshot_t timer;
    fridge_kitchen_stopwatch_snapshot_t stopwatch;
    size_t alarm_count;
    fridge_kitchen_alarm_t alarms[FRIDGE_KITCHEN_TOOL_MAX_ALARMS];
    char last_alert[FRIDGE_KITCHEN_TOOL_MAX_MESSAGE_LEN + 1];
    char last_error[FRIDGE_KITCHEN_TOOL_MAX_MESSAGE_LEN + 1];
} fridge_kitchen_tools_snapshot_t;

typedef struct {
    bool executed;
    bool needs_confirmation;
    char tool[16];
    char action[16];
    char message[FRIDGE_KITCHEN_TOOL_MAX_MESSAGE_LEN + 1];
} fridge_kitchen_tools_ai_result_t;

esp_err_t fridge_kitchen_tools_init(void);
esp_err_t fridge_kitchen_tools_get_snapshot(fridge_kitchen_tools_snapshot_t *out);

esp_err_t fridge_kitchen_tools_timer_start(uint32_t seconds, const char *label);
esp_err_t fridge_kitchen_tools_timer_pause(void);
esp_err_t fridge_kitchen_tools_timer_resume(void);
esp_err_t fridge_kitchen_tools_timer_cancel(void);
esp_err_t fridge_kitchen_tools_timer_dismiss(void);

esp_err_t fridge_kitchen_tools_stopwatch_start(void);
esp_err_t fridge_kitchen_tools_stopwatch_pause(void);
esp_err_t fridge_kitchen_tools_stopwatch_reset(void);

esp_err_t fridge_kitchen_tools_alarm_set(uint8_t hour, uint8_t minute, const char *label, uint8_t *out_id);
esp_err_t fridge_kitchen_tools_alarm_cancel(uint8_t id);
esp_err_t fridge_kitchen_tools_alarm_dismiss(uint8_t id);

// 执行 AI 返回的白名单 JSON 指令；非法或槽位不足时只返回需要确认，不改变本地状态。
esp_err_t fridge_kitchen_tools_execute_ai_json(const char *json, fridge_kitchen_tools_ai_result_t *out);

const char *fridge_kitchen_timer_state_text(fridge_kitchen_timer_state_t state);
const char *fridge_kitchen_stopwatch_state_text(fridge_kitchen_stopwatch_state_t state);

#ifdef __cplusplus
}
#endif
