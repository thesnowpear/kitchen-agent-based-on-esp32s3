#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_WAKE_WORD_TEXT "小冰小冰"
#define FRIDGE_WAKE_WORD_MODEL "wn9_xiaobinxiaobin_tts"

typedef enum {
    FRIDGE_WAKE_WORD_STATE_IDLE = 0,
    FRIDGE_WAKE_WORD_STATE_LISTENING,
    FRIDGE_WAKE_WORD_STATE_ERROR,
} fridge_wake_word_state_t;

typedef void (*fridge_wake_word_event_cb_t)(void *user_ctx);

typedef struct {
    bool enabled;
    fridge_wake_word_state_t state;
    uint32_t trigger_count;
    uint32_t last_trigger_ms;
    int32_t vad_state;
    int32_t rms;
    int32_t peak_abs;
    uint32_t timeout_count;
    char wake_word[32];
    char model[48];
    char error[128];
} fridge_wake_word_status_t;

// 初始化本地唤醒词服务。
// 硬件约束：服务复用 INMP441 I2S RX，启动监听前必须确认麦克风为 3.3V 供电并与主控共地。
esp_err_t fridge_wake_word_init(void);

// 注册唤醒事件回调。回调在唤醒任务上下文中执行，不能做长时间阻塞操作。
esp_err_t fridge_wake_word_set_event_callback(fridge_wake_word_event_cb_t cb, void *user_ctx);

// 启动/停止本地唤醒监听；监听期间会占用麦克风 I2S RX，和手动录音互斥。
esp_err_t fridge_wake_word_start(void);
esp_err_t fridge_wake_word_stop(void);

// 清零触发次数和最近触发时间，保留当前启停状态。
esp_err_t fridge_wake_word_reset_stats(void);

esp_err_t fridge_wake_word_get_status(fridge_wake_word_status_t *out);
const char *fridge_wake_word_state_text(fridge_wake_word_state_t state);

#ifdef __cplusplus
}
#endif
