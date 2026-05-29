// 冰箱小精灵公共语音会话接口。
// 该组件只复用最近一次录音结果，不直接控制 I2S 引脚；录音的开始/停止由调用方负责。

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_VOICE_SESSION_MAX_ERROR_LEN 160

typedef struct {
    char transcript[768 + 1];
    char reply[2048 + 1];
    char asr_model[64 + 1];
    char ai_model[64 + 1];
    char error[FRIDGE_VOICE_SESSION_MAX_ERROR_LEN + 1];
    int asr_http_status;
    int ai_http_status;
    uint32_t asr_latency_ms;
    uint32_t ai_latency_ms;
    size_t audio_bytes;
    size_t history_pruned_count;
} fridge_voice_session_result_t;

// 执行“最近录音 -> ASR -> AI -> 保存历史”。
// 注意：该函数会进行 HTTPS/TLS 请求，必须在普通任务上下文调用，不能放在 UI/USB 热路径或中断中。
esp_err_t fridge_voice_session_run_latest_recording(fridge_voice_session_result_t *out);

#ifdef __cplusplus
}
#endif
