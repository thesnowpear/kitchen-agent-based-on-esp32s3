#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_ASR_MAX_URL_LEN 192
#define FRIDGE_ASR_MAX_MODEL_LEN 64
#define FRIDGE_ASR_MAX_API_KEY_LEN 256
#define FRIDGE_ASR_MAX_TEXT_LEN 768
#define FRIDGE_ASR_MAX_ERROR_LEN 160
#define FRIDGE_ASR_DEFAULT_URL "https://api.siliconflow.cn/v1/audio/transcriptions"
#define FRIDGE_ASR_DEFAULT_MODEL "TeleAI/TeleSpeechASR"
#define FRIDGE_ASR_DEFAULT_TIMEOUT_MS 45000

typedef struct {
    char api_base_url[FRIDGE_ASR_MAX_URL_LEN + 1];
    char model[FRIDGE_ASR_MAX_MODEL_LEN + 1];
    uint32_t timeout_ms;
    bool has_api_key;
    bool ready;
    char api_key_preview[32];
    char last_error[FRIDGE_ASR_MAX_ERROR_LEN + 1];
} fridge_asr_config_view_t;

typedef struct {
    char api_base_url[FRIDGE_ASR_MAX_URL_LEN + 1];
    char model[FRIDGE_ASR_MAX_MODEL_LEN + 1];
    char api_key[FRIDGE_ASR_MAX_API_KEY_LEN + 1];
    uint32_t timeout_ms;
    int64_t config_updated_at_ms;
    bool update_api_key;
} fridge_asr_config_update_t;

typedef struct {
    char text[FRIDGE_ASR_MAX_TEXT_LEN + 1];
    char model[FRIDGE_ASR_MAX_MODEL_LEN + 1];
    char status[32];
    char error[FRIDGE_ASR_MAX_ERROR_LEN + 1];
    int http_status;
    uint32_t latency_ms;
    size_t audio_bytes;
} fridge_asr_result_t;

esp_err_t fridge_asr_init(void);
esp_err_t fridge_asr_get_config(fridge_asr_config_view_t *out);
esp_err_t fridge_asr_set_config(const fridge_asr_config_update_t *config);
esp_err_t fridge_asr_clear_key(void);
// 生成设备本地 ASR 同步文档；比赛演示版会包含明文 API Key，只能走受控同步通道。
esp_err_t fridge_asr_get_sync_payload(char *out, size_t out_size);

// 将 audio 组件中最近一次录音封装为 16k/16bit/mono WAV，并上传到 ASR API。
esp_err_t fridge_asr_transcribe_latest_recording(fridge_asr_result_t *out);

#ifdef __cplusplus
}
#endif
