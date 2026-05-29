#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_TTS_MAX_URL_LEN 192
#define FRIDGE_TTS_MAX_MODEL_LEN 64
#define FRIDGE_TTS_MAX_VOICE_LEN 32
#define FRIDGE_TTS_MAX_API_KEY_LEN 256
#define FRIDGE_TTS_MAX_TEXT_LEN 512
#define FRIDGE_TTS_MAX_ERROR_LEN 160
#define FRIDGE_TTS_DEFAULT_URL "https://api.siliconflow.cn/v1/audio/speech"
#define FRIDGE_TTS_DEFAULT_MODEL "fnlp/MOSS-TTSD-v0.5"
#define FRIDGE_TTS_DEFAULT_VOICE "fnlp/MOSS-TTSD-v0.5:alex"
#define FRIDGE_TTS_DEFAULT_TIMEOUT_MS 45000
#define FRIDGE_TTS_SAMPLE_RATE 24000

typedef enum {
    FRIDGE_SPEAKER_STATE_IDLE = 0,
    FRIDGE_SPEAKER_STATE_SYNTHESIZING,
    FRIDGE_SPEAKER_STATE_PLAYING,
    FRIDGE_SPEAKER_STATE_DONE,
    FRIDGE_SPEAKER_STATE_ERROR,
} fridge_speaker_state_t;

typedef struct {
    char api_base_url[FRIDGE_TTS_MAX_URL_LEN + 1];
    char model[FRIDGE_TTS_MAX_MODEL_LEN + 1];
    char voice[FRIDGE_TTS_MAX_VOICE_LEN + 1];
    uint32_t timeout_ms;
    bool has_api_key;
    bool ready;
    char api_key_preview[32];
    char last_error[FRIDGE_TTS_MAX_ERROR_LEN + 1];
} fridge_tts_config_view_t;

typedef struct {
    char api_base_url[FRIDGE_TTS_MAX_URL_LEN + 1];
    char model[FRIDGE_TTS_MAX_MODEL_LEN + 1];
    char voice[FRIDGE_TTS_MAX_VOICE_LEN + 1];
    char api_key[FRIDGE_TTS_MAX_API_KEY_LEN + 1];
    uint32_t timeout_ms;
    bool update_api_key;
} fridge_tts_config_update_t;

typedef struct {
    fridge_speaker_state_t state;
    uint32_t sample_rate;
    size_t audio_bytes;
    size_t played_bytes;
    uint32_t duration_ms;
    uint32_t latency_ms;
    int http_status;
    char model[FRIDGE_TTS_MAX_MODEL_LEN + 1];
    char voice[FRIDGE_TTS_MAX_VOICE_LEN + 1];
    char error[FRIDGE_TTS_MAX_ERROR_LEN + 1];
} fridge_speaker_status_t;

esp_err_t fridge_speaker_init(void);
esp_err_t fridge_tts_get_config(fridge_tts_config_view_t *out);
esp_err_t fridge_tts_set_config(const fridge_tts_config_update_t *config);
esp_err_t fridge_tts_clear_key(void);
esp_err_t fridge_speaker_synthesize_and_play(const char *text, fridge_speaker_status_t *out);
esp_err_t fridge_speaker_get_status(fridge_speaker_status_t *out);
esp_err_t fridge_speaker_stop(void);

#ifdef __cplusplus
}
#endif
