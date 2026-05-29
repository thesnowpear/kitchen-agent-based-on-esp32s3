// 冰箱小精灵扬声器测试组件。
// 通过 MAX98357 I2S 数字功放播放云端 TTS 返回的 24kHz/16-bit/mono PCM。
// 硬件注意：MAX98357 VCC 可接 5V，但 BCLK/LRC/DIN/SD/GAIN 只能接 ESP32-S3 的 3.3V 逻辑信号；上电前必须确认共地和扬声器阻抗。
#include "fridge_speaker.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cJSON.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fridge_network.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#define SPEAKER_BCLK_GPIO 40
#define SPEAKER_WS_GPIO 41
#define SPEAKER_DOUT_GPIO 39
#define SPEAKER_DMA_DESC_NUM 4
#define SPEAKER_DMA_FRAME_NUM 256
#define SPEAKER_TASK_STACK 4096
#define SPEAKER_HTTP_MAX_BYTES (384 * 1024)
#define TTS_NVS_NAMESPACE "fridge_tts"
#define TTS_NVS_KEY_URL "url"
#define TTS_NVS_KEY_MODEL "model"
#define TTS_NVS_KEY_VOICE "voice"
#define TTS_NVS_KEY_API_KEY "api_key"
#define TTS_NVS_KEY_TIMEOUT "timeout_ms"
#define TTS_NVS_KEY_UPDATED_MS "updated_ms"
#define TTS_NVS_KEY_VOLUME "volume"
#define TTS_NVS_KEY_ENABLED "enabled"

static const char *TAG = "fridge_speaker";

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
    bool overflow;
} speaker_http_buffer_t;

static i2s_chan_handle_t s_tx_chan;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_play_task;
static uint8_t *s_audio;
static size_t s_audio_bytes;
static size_t s_played_bytes;
static uint32_t s_duration_ms;
static uint32_t s_latency_ms;
static int s_http_status;
static fridge_speaker_state_t s_state = FRIDGE_SPEAKER_STATE_IDLE;
static char s_model[FRIDGE_TTS_MAX_MODEL_LEN + 1];
static char s_voice[FRIDGE_TTS_MAX_VOICE_LEN + 1];
static char s_error[FRIDGE_TTS_MAX_ERROR_LEN + 1];
static uint8_t s_volume = FRIDGE_SPEAKER_DEFAULT_VOLUME;
static bool s_volume_loaded;
static bool s_tts_enabled = true;
static bool s_tts_enabled_loaded;
static bool s_initialized;
static bool s_i2s_enabled;
static volatile bool s_stop_requested;

static void *speaker_large_alloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static void make_key_preview(const char *key, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!key || key[0] == '\0') {
        return;
    }
    size_t len = strlen(key);
    if (len <= 8) {
        strlcpy(out, "已保存", out_size);
        return;
    }
    snprintf(out, out_size, "%.*s...%s", strncmp(key, "sk-", 3) == 0 ? 3 : 4, key, key + len - 4);
}

static uint32_t clamp_timeout_ms(uint32_t timeout_ms)
{
    if (timeout_ms < 5000) {
        return 5000;
    }
    if (timeout_ms > 90000) {
        return 90000;
    }
    return timeout_ms;
}

static int64_t tts_config_now_ms(void)
{
    time_t now = time(NULL);
    if (now >= 1735689600LL) {
        return (int64_t)now * 1000;
    }
    return esp_timer_get_time() / 1000;
}

static int64_t load_tts_updated_ms(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(TTS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return 0;
    }
    int64_t updated_ms = 0;
    (void)nvs_get_i64(handle, TTS_NVS_KEY_UPDATED_MS, &updated_ms);
    nvs_close(handle);
    return updated_ms > 0 ? updated_ms : 0;
}

static uint8_t clamp_volume(uint8_t percent)
{
    return percent > 100 ? 100 : percent;
}

static void set_state(fridge_speaker_state_t state, const char *error)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = state;
    strlcpy(s_error, error ? error : "", sizeof(s_error));
    xSemaphoreGive(s_lock);
}

static void read_nvs_string(nvs_handle_t handle, const char *key, char *out, size_t out_size, const char *fallback)
{
    size_t len = out_size;
    if (!out || out_size == 0 || nvs_get_str(handle, key, out, &len) != ESP_OK) {
        if (out && out_size > 0) {
            strlcpy(out, fallback ? fallback : "", out_size);
        }
    }
}

static esp_err_t load_tts_config(fridge_tts_config_update_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    memset(config, 0, sizeof(*config));
    strlcpy(config->api_base_url, FRIDGE_TTS_DEFAULT_URL, sizeof(config->api_base_url));
    strlcpy(config->model, FRIDGE_TTS_DEFAULT_MODEL, sizeof(config->model));
    strlcpy(config->voice, FRIDGE_TTS_DEFAULT_VOICE, sizeof(config->voice));
    config->timeout_ms = FRIDGE_TTS_DEFAULT_TIMEOUT_MS;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(TTS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open TTS NVS failed");
    read_nvs_string(handle, TTS_NVS_KEY_URL, config->api_base_url, sizeof(config->api_base_url), FRIDGE_TTS_DEFAULT_URL);
    read_nvs_string(handle, TTS_NVS_KEY_MODEL, config->model, sizeof(config->model), FRIDGE_TTS_DEFAULT_MODEL);
    read_nvs_string(handle, TTS_NVS_KEY_VOICE, config->voice, sizeof(config->voice), FRIDGE_TTS_DEFAULT_VOICE);
    size_t key_len = sizeof(config->api_key);
    if (nvs_get_str(handle, TTS_NVS_KEY_API_KEY, config->api_key, &key_len) == ESP_OK && config->api_key[0] != '\0') {
        config->update_api_key = true;
    }
    uint32_t timeout_ms = FRIDGE_TTS_DEFAULT_TIMEOUT_MS;
    if (nvs_get_u32(handle, TTS_NVS_KEY_TIMEOUT, &timeout_ms) == ESP_OK) {
        config->timeout_ms = clamp_timeout_ms(timeout_ms);
    }
    int64_t updated_ms = 0;
    if (nvs_get_i64(handle, TTS_NVS_KEY_UPDATED_MS, &updated_ms) == ESP_OK && updated_ms > 0) {
        config->config_updated_at_ms = updated_ms;
    }
    nvs_close(handle);
    return ESP_OK;
}

static uint8_t load_saved_volume(void)
{
    uint8_t volume = FRIDGE_SPEAKER_DEFAULT_VOLUME;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(TTS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        (void)nvs_get_u8(handle, TTS_NVS_KEY_VOLUME, &volume);
        nvs_close(handle);
    }
    return clamp_volume(volume);
}

static esp_err_t save_volume(uint8_t volume)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(TTS_NVS_NAMESPACE, NVS_READWRITE, &handle), TAG, "open TTS volume NVS failed");
    esp_err_t err = nvs_set_u8(handle, TTS_NVS_KEY_VOLUME, clamp_volume(volume));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static bool load_saved_tts_enabled(void)
{
    uint8_t enabled = 1;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(TTS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        (void)nvs_get_u8(handle, TTS_NVS_KEY_ENABLED, &enabled);
        nvs_close(handle);
    }
    return enabled != 0;
}

static esp_err_t save_tts_enabled(bool enabled)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(TTS_NVS_NAMESPACE, NVS_READWRITE, &handle), TAG, "open TTS enable NVS failed");
    esp_err_t err = nvs_set_u8(handle, TTS_NVS_KEY_ENABLED, enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void scale_pcm16(uint8_t *data, size_t len, uint8_t volume)
{
    // MAX98357 没有数字音量寄存器，这里在 I2S 写入前按百分比缩放 16-bit PCM。
    int16_t *samples = (int16_t *)data;
    size_t count = len / sizeof(int16_t);
    for (size_t i = 0; i < count; i++) {
        int32_t scaled = ((int32_t)samples[i] * (int32_t)volume) / 100;
        if (scaled > INT16_MAX) {
            scaled = INT16_MAX;
        } else if (scaled < INT16_MIN) {
            scaled = INT16_MIN;
        }
        samples[i] = (int16_t)scaled;
    }
}

static esp_err_t speaker_http_event_handler(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || !event->user_data || !event->data || event->data_len <= 0) {
        return ESP_OK;
    }
    speaker_http_buffer_t *buffer = (speaker_http_buffer_t *)event->user_data;
    size_t available = buffer->cap > buffer->len ? buffer->cap - buffer->len : 0;
    size_t copy_len = (size_t)event->data_len;
    if (copy_len > available) {
        copy_len = available;
        buffer->overflow = true;
    }
    if (copy_len > 0) {
        memcpy(buffer->data + buffer->len, event->data, copy_len);
        buffer->len += copy_len;
    }
    return ESP_OK;
}

static char *json_escape_alloc(const char *text)
{
    size_t len = 0;
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
        len += (*p == '"' || *p == '\\' || *p == '\n' || *p == '\r' || *p == '\t') ? 2 : 1;
    }
    char *out = calloc(1, len + 1);
    if (!out) {
        return NULL;
    }
    char *w = out;
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
        if (*p == '"') {
            *w++ = '\\';
            *w++ = '"';
        } else if (*p == '\\') {
            *w++ = '\\';
            *w++ = '\\';
        } else if (*p == '\n') {
            *w++ = '\\';
            *w++ = 'n';
        } else if (*p == '\r') {
            *w++ = '\\';
            *w++ = 'r';
        } else if (*p == '\t') {
            *w++ = '\\';
            *w++ = 't';
        } else if (*p < 0x20) {
            *w++ = '?';
        } else {
            *w++ = (char)*p;
        }
    }
    return out;
}

static void normalize_voice_for_model(const fridge_tts_config_update_t *config, char *out, size_t out_size)
{
    // 硅基流动 MOSS-TTSD 要求 voice 使用“模型名:音色名”的完整格式。
    // Web 里如果残留 OpenAI 的 alloy 或只填 alex，这里自动修正，避免云端 400。
    if (!out || out_size == 0) {
        return;
    }
    const bool is_moss = strcmp(config->model, "fnlp/MOSS-TTSD-v0.5") == 0;
    if (config->voice[0] == '\0') {
        strlcpy(out, FRIDGE_TTS_DEFAULT_VOICE, out_size);
        return;
    }
    if (is_moss && strcmp(config->voice, "alloy") == 0) {
        strlcpy(out, FRIDGE_TTS_DEFAULT_VOICE, out_size);
        return;
    }
    if (strchr(config->voice, ':')) {
        strlcpy(out, config->voice, out_size);
        return;
    }
    if (is_moss) {
        snprintf(out, out_size, "%s:%s", config->model, config->voice);
        return;
    }
    strlcpy(out, config->voice, out_size);
}

static char *build_tts_request(const fridge_tts_config_update_t *config, const char *text)
{
    char normalized_voice[FRIDGE_TTS_MAX_VOICE_LEN + 1] = {0};
    normalize_voice_for_model(config, normalized_voice, sizeof(normalized_voice));
    char *model = json_escape_alloc(config->model);
    char *voice = json_escape_alloc(normalized_voice);
    char *input = json_escape_alloc(text);
    if (!model || !voice || !input) {
        free(model);
        free(voice);
        free(input);
        return NULL;
    }
    size_t body_len = strlen(model) + strlen(voice) + strlen(input) + 128;
    char *body = calloc(1, body_len);
    if (body) {
        snprintf(body, body_len,
                 "{\"model\":\"%s\",\"voice\":\"%s\",\"input\":\"%s\","
                 "\"response_format\":\"pcm\",\"sample_rate\":%d,\"stream\":false}",
                 model, voice, input, FRIDGE_TTS_SAMPLE_RATE);
    }
    free(model);
    free(voice);
    free(input);
    return body;
}

static void play_task(void *arg)
{
    (void)arg;
    size_t offset = 0;
    uint8_t chunk_buf[2048];
    while (true) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool stop = s_stop_requested;
        uint8_t *audio = s_audio;
        size_t total = s_audio_bytes;
        uint8_t volume = s_volume;
        xSemaphoreGive(s_lock);
        if (stop || offset >= total || !audio) {
            break;
        }
        size_t chunk = total - offset;
        if (chunk > 2048) {
            chunk = 2048;
        }
        memcpy(chunk_buf, audio + offset, chunk);
        scale_pcm16(chunk_buf, chunk, volume);
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx_chan, chunk_buf, chunk, &written, pdMS_TO_TICKS(500));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s speaker write failed: %s", esp_err_to_name(err));
            set_state(FRIDGE_SPEAKER_STATE_ERROR, "i2s speaker write failed");
            break;
        }
        offset += written;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_played_bytes = offset;
        xSemaphoreGive(s_lock);
        if (written == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    bool should_disable = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_i2s_enabled) {
        s_i2s_enabled = false;
        should_disable = true;
    }
    if (s_state == FRIDGE_SPEAKER_STATE_PLAYING) {
        s_state = s_stop_requested ? FRIDGE_SPEAKER_STATE_IDLE : FRIDGE_SPEAKER_STATE_DONE;
    }
    s_stop_requested = false;
    s_play_task = NULL;
    xSemaphoreGive(s_lock);
    if (should_disable) {
        (void)i2s_channel_disable(s_tx_chan);
    }
    vTaskDelete(NULL);
}

esp_err_t fridge_speaker_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "speaker lock allocation failed");

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = SPEAKER_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = SPEAKER_DMA_FRAME_NUM;
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
    ESP_RETURN_ON_ERROR(err, TAG, "speaker i2s channel create failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(FRIDGE_TTS_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPEAKER_BCLK_GPIO,
            .ws = SPEAKER_WS_GPIO,
            .dout = SPEAKER_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    err = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    ESP_RETURN_ON_ERROR(err, TAG, "speaker i2s std init failed");
    s_volume = load_saved_volume();
    s_volume_loaded = true;
    s_tts_enabled = load_saved_tts_enabled();
    s_tts_enabled_loaded = true;
    s_initialized = true;
    ESP_LOGI(TAG, "MAX98357 test output ready: BCLK=GPIO%d LRC=GPIO%d DIN=GPIO%d sample_rate=%d volume=%u%% tts=%s; BCLK/WS share the INMP441 clock wiring",
             SPEAKER_BCLK_GPIO,
             SPEAKER_WS_GPIO,
             SPEAKER_DOUT_GPIO,
             FRIDGE_TTS_SAMPLE_RATE,
             (unsigned)s_volume,
             s_tts_enabled ? "on" : "off");
    return ESP_OK;
}

esp_err_t fridge_tts_get_config(fridge_tts_config_view_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "config view is NULL");
    fridge_tts_config_update_t config = {0};
    ESP_RETURN_ON_ERROR(load_tts_config(&config), TAG, "load TTS config failed");
    char normalized_voice[FRIDGE_TTS_MAX_VOICE_LEN + 1] = {0};
    normalize_voice_for_model(&config, normalized_voice, sizeof(normalized_voice));
    memset(out, 0, sizeof(*out));
    strlcpy(out->api_base_url, config.api_base_url, sizeof(out->api_base_url));
    strlcpy(out->model, config.model, sizeof(out->model));
    strlcpy(out->voice, normalized_voice, sizeof(out->voice));
    out->timeout_ms = clamp_timeout_ms(config.timeout_ms);
    out->has_api_key = config.api_key[0] != '\0';
    out->ready = out->api_base_url[0] != '\0' && out->model[0] != '\0' && out->voice[0] != '\0' && out->has_api_key;
    make_key_preview(config.api_key, out->api_key_preview, sizeof(out->api_key_preview));
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(out->last_error, s_error, sizeof(out->last_error));
        xSemaphoreGive(s_lock);
    }
    return ESP_OK;
}

esp_err_t fridge_tts_set_config(const fridge_tts_config_update_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(TTS_NVS_NAMESPACE, NVS_READWRITE, &handle), TAG, "open TTS NVS failed");
    esp_err_t err = nvs_set_str(handle, TTS_NVS_KEY_URL, config->api_base_url[0] ? config->api_base_url : FRIDGE_TTS_DEFAULT_URL);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, TTS_NVS_KEY_MODEL, config->model[0] ? config->model : FRIDGE_TTS_DEFAULT_MODEL);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, TTS_NVS_KEY_VOICE, config->voice[0] ? config->voice : FRIDGE_TTS_DEFAULT_VOICE);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, TTS_NVS_KEY_TIMEOUT, clamp_timeout_ms(config->timeout_ms));
    }
    if (err == ESP_OK && config->update_api_key) {
        err = nvs_set_str(handle, TTS_NVS_KEY_API_KEY, config->api_key);
    }
    if (err == ESP_OK) {
        int64_t previous_ms = load_tts_updated_ms();
        int64_t updated_ms = config->config_updated_at_ms > 0 ? config->config_updated_at_ms : tts_config_now_ms();
        if (config->config_updated_at_ms <= 0 && updated_ms <= previous_ms) {
            updated_ms = previous_ms + 1;
        }
        err = nvs_set_i64(handle, TTS_NVS_KEY_UPDATED_MS, updated_ms);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t fridge_tts_clear_key(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(TTS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open TTS NVS failed");
    err = nvs_erase_key(handle, TTS_NVS_KEY_API_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t fridge_tts_get_sync_payload(char *out, size_t out_size)
{
    ESP_RETURN_ON_FALSE(out && out_size > 0, ESP_ERR_INVALID_ARG, TAG, "invalid TTS sync payload buffer");
    out[0] = '\0';

    fridge_tts_config_update_t config = {0};
    ESP_RETURN_ON_ERROR(load_tts_config(&config), TAG, "load TTS sync config failed");
    char normalized_voice[FRIDGE_TTS_MAX_VOICE_LEN + 1] = {0};
    normalize_voice_for_model(&config, normalized_voice, sizeof(normalized_voice));

    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root, ESP_ERR_NO_MEM, TAG, "create TTS sync payload failed");
    cJSON_AddNumberToObject(root, "configUpdatedAt", (double)config.config_updated_at_ms);
    cJSON_AddStringToObject(root, "ttsApiBaseUrl", config.api_base_url);
    cJSON_AddStringToObject(root, "ttsModel", config.model);
    cJSON_AddStringToObject(root, "ttsVoice", normalized_voice);
    cJSON_AddStringToObject(root, "ttsApiKey", config.api_key);
    cJSON_AddNumberToObject(root, "ttsTimeoutMs", config.timeout_ms);

    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ESP_RETURN_ON_FALSE(printed, ESP_ERR_NO_MEM, TAG, "print TTS sync payload failed");
    size_t len = strlen(printed);
    if (len >= out_size) {
        cJSON_free(printed);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, printed, len + 1);
    cJSON_free(printed);
    return ESP_OK;
}

esp_err_t fridge_speaker_preview_volume(uint8_t percent)
{
    ESP_RETURN_ON_ERROR(fridge_speaker_init(), TAG, "speaker init failed");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_volume = clamp_volume(percent);
    s_volume_loaded = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_speaker_set_volume(uint8_t percent)
{
    ESP_RETURN_ON_ERROR(fridge_speaker_preview_volume(percent), TAG, "speaker volume preview failed");
    uint8_t volume = clamp_volume(percent);
    esp_err_t err = save_volume(volume);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save speaker volume failed: %s", esp_err_to_name(err));
    }
    return err;
}

uint8_t fridge_speaker_get_volume(void)
{
    if (!s_initialized || !s_lock) {
        if (!s_volume_loaded) {
            s_volume = load_saved_volume();
            s_volume_loaded = true;
        }
        return s_volume;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t volume = s_volume;
    xSemaphoreGive(s_lock);
    return volume;
}

esp_err_t fridge_speaker_set_tts_enabled(bool enabled)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_tts_enabled = enabled;
        s_tts_enabled_loaded = true;
        xSemaphoreGive(s_lock);
    } else {
        s_tts_enabled = enabled;
        s_tts_enabled_loaded = true;
    }
    esp_err_t err = save_tts_enabled(enabled);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save TTS enabled failed: %s", esp_err_to_name(err));
    }
    return err;
}

bool fridge_speaker_get_tts_enabled(void)
{
    if (!s_initialized || !s_lock) {
        if (!s_tts_enabled_loaded) {
            s_tts_enabled = load_saved_tts_enabled();
            s_tts_enabled_loaded = true;
        }
        return s_tts_enabled;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool enabled = s_tts_enabled;
    xSemaphoreGive(s_lock);
    return enabled;
}

esp_err_t fridge_speaker_synthesize_and_play(const char *text, fridge_speaker_status_t *out)
{
    ESP_RETURN_ON_FALSE(text && text[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "TTS text is empty");
    ESP_RETURN_ON_ERROR(fridge_speaker_init(), TAG, "speaker init failed");
    ESP_RETURN_ON_ERROR(fridge_speaker_stop(), TAG, "speaker stop before synth failed");

    fridge_network_status_t net = {0};
    fridge_network_get_status(&net);
    if (!net.connected) {
        set_state(FRIDGE_SPEAKER_STATE_ERROR, "Wi-Fi 未连接，请先完成配网");
        return ESP_ERR_INVALID_STATE;
    }
    if (!net.internet_ready) {
        (void)fridge_network_sync_time();
        fridge_network_get_status(&net);
        if (!net.internet_ready) {
            set_state(FRIDGE_SPEAKER_STATE_ERROR, "网络未校时或外网不可用");
            return ESP_ERR_INVALID_STATE;
        }
    }

    fridge_tts_config_update_t config = {0};
    ESP_RETURN_ON_ERROR(load_tts_config(&config), TAG, "load TTS config failed");
    char normalized_voice[FRIDGE_TTS_MAX_VOICE_LEN + 1] = {0};
    normalize_voice_for_model(&config, normalized_voice, sizeof(normalized_voice));
    if (config.api_base_url[0] == '\0' || strncmp(config.api_base_url, "https://", 8) != 0) {
        set_state(FRIDGE_SPEAKER_STATE_ERROR, "TTS URL 必须使用 https://");
        return ESP_ERR_INVALID_ARG;
    }
    if (config.api_key[0] == '\0') {
        set_state(FRIDGE_SPEAKER_STATE_ERROR, "缺少 TTS API Key");
        return ESP_ERR_INVALID_STATE;
    }

    char *request_body = build_tts_request(&config, text);
    ESP_RETURN_ON_FALSE(request_body, ESP_ERR_NO_MEM, TAG, "build TTS request failed");
    uint8_t *audio = speaker_large_alloc(SPEAKER_HTTP_MAX_BYTES);
    if (!audio) {
        free(request_body);
        set_state(FRIDGE_SPEAKER_STATE_ERROR, "分配 TTS 音频缓冲失败");
        return ESP_ERR_NO_MEM;
    }
    speaker_http_buffer_t rx = {
        .data = audio,
        .len = 0,
        .cap = SPEAKER_HTTP_MAX_BYTES,
        .overflow = false,
    };
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = FRIDGE_SPEAKER_STATE_SYNTHESIZING;
    s_audio_bytes = 0;
    s_played_bytes = 0;
    s_duration_ms = 0;
    s_latency_ms = 0;
    s_http_status = 0;
    strlcpy(s_model, config.model, sizeof(s_model));
    strlcpy(s_voice, normalized_voice, sizeof(s_voice));
    s_error[0] = '\0';
    xSemaphoreGive(s_lock);

    esp_http_client_config_t http_config = {
        .url = config.api_base_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = (int)clamp_timeout_ms(config.timeout_ms),
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = speaker_http_event_handler,
        .user_data = &rx,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        free(request_body);
        free(audio);
        set_state(FRIDGE_SPEAKER_STATE_ERROR, "初始化 TTS HTTP 客户端失败");
        return ESP_FAIL;
    }
    char auth_header[FRIDGE_TTS_MAX_API_KEY_LEN + 16] = {0};
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", config.api_key);
    esp_http_client_set_header(client, "Accept", "audio/pcm");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_post_field(client, request_body, (int)strlen(request_body));

    int64_t start_us = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    uint32_t latency_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
    esp_http_client_cleanup(client);
    free(request_body);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_http_status = status;
    s_latency_ms = latency_ms;
    xSemaphoreGive(s_lock);
    if (err != ESP_OK || status < 200 || status >= 300 || rx.overflow || rx.len == 0) {
        char error[FRIDGE_TTS_MAX_ERROR_LEN + 1];
        if (err != ESP_OK) {
            snprintf(error, sizeof(error), "TTS HTTP 请求失败：%s", esp_err_to_name(err));
        } else if (rx.overflow) {
            strlcpy(error, "TTS 音频超过设备缓冲", sizeof(error));
        } else if (rx.len == 0) {
            strlcpy(error, "TTS 响应为空", sizeof(error));
        } else {
            char preview[96] = {0};
            size_t preview_len = rx.len < sizeof(preview) - 1 ? rx.len : sizeof(preview) - 1;
            for (size_t i = 0; i < preview_len; i++) {
                unsigned char ch = rx.data[i];
                preview[i] = isprint(ch) ? (char)ch : ' ';
            }
            snprintf(error, sizeof(error), "TTS HTTP 状态异常：%d %s", status, preview);
        }
        free(audio);
        set_state(FRIDGE_SPEAKER_STATE_ERROR, error);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    free(s_audio);
    s_audio = audio;
    s_audio_bytes = rx.len;
    s_played_bytes = 0;
    s_duration_ms = (uint32_t)(rx.len * 1000 / (FRIDGE_TTS_SAMPLE_RATE * sizeof(int16_t)));
    s_stop_requested = false;
    s_state = FRIDGE_SPEAKER_STATE_PLAYING;
    xSemaphoreGive(s_lock);

    err = i2s_channel_enable(s_tx_chan);
    if (err != ESP_OK) {
        set_state(FRIDGE_SPEAKER_STATE_ERROR, "扬声器 I2S enable 失败");
        return err;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_i2s_enabled = true;
    xSemaphoreGive(s_lock);

    BaseType_t ok = xTaskCreate(play_task, "speaker_play", SPEAKER_TASK_STACK, NULL, 5, &s_play_task);
    if (ok != pdPASS) {
        (void)i2s_channel_disable(s_tx_chan);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_i2s_enabled = false;
        xSemaphoreGive(s_lock);
        set_state(FRIDGE_SPEAKER_STATE_ERROR, "创建扬声器播放任务失败");
        return ESP_ERR_NO_MEM;
    }
    return fridge_speaker_get_status(out);
}

esp_err_t fridge_speaker_get_status(fridge_speaker_status_t *out)
{
    ESP_RETURN_ON_FALSE(out && s_lock, ESP_ERR_INVALID_ARG, TAG, "speaker status args invalid");
    memset(out, 0, sizeof(*out));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->state = s_state;
    out->sample_rate = FRIDGE_TTS_SAMPLE_RATE;
    out->audio_bytes = s_audio_bytes;
    out->played_bytes = s_played_bytes;
    out->duration_ms = s_duration_ms;
    out->latency_ms = s_latency_ms;
    out->http_status = s_http_status;
    strlcpy(out->model, s_model, sizeof(out->model));
    strlcpy(out->voice, s_voice, sizeof(out->voice));
    strlcpy(out->error, s_error, sizeof(out->error));
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_speaker_stop(void)
{
    if (!s_lock) {
        return ESP_OK;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool playing = s_play_task != NULL || s_state == FRIDGE_SPEAKER_STATE_PLAYING;
    s_stop_requested = true;
    xSemaphoreGive(s_lock);
    for (int i = 0; playing && i < 50 && s_play_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_play_task) {
        set_state(FRIDGE_SPEAKER_STATE_ERROR, "扬声器播放任务停止超时");
        return ESP_ERR_TIMEOUT;
    }
    if (playing) {
        set_state(FRIDGE_SPEAKER_STATE_IDLE, "");
    }
    return ESP_OK;
}
