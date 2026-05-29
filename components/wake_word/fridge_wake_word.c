// 冰箱小精灵本地语音唤醒组件。
// 使用 ESP-SR WakeNet 在 ESP32-S3 本地识别“小冰小冰”，只负责唤醒事件上报，不直接触发 ASR 或 AI。
#include "fridge_wake_word.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fridge_audio.h"
#include "fridge_speaker.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "model_path.h"

#define WAKE_TASK_STACK 8192
#define WAKE_TASK_PRIORITY 5
#define WAKE_STOP_WAIT_MS 1500
#define WAKE_TRIGGER_GUARD_MS 1200

static const char *TAG = "fridge_wake";

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static const esp_afe_sr_iface_t *s_afe_handle;
static esp_afe_sr_data_t *s_afe_data;
static srmodel_list_t *s_models;
static bool s_initialized;
static bool s_stop_requested;
static bool s_enabled;
static fridge_wake_word_state_t s_state = FRIDGE_WAKE_WORD_STATE_IDLE;
static uint32_t s_trigger_count;
static uint32_t s_last_trigger_ms;
static int32_t s_vad_state = -1;
static int32_t s_rms;
static int32_t s_peak_abs;
static uint32_t s_timeout_count;
static char s_error[128];
static fridge_wake_word_event_cb_t s_event_cb;
static void *s_event_ctx;

static void set_error_locked(const char *error)
{
    strlcpy(s_error, error ? error : "", sizeof(s_error));
}

static void set_state(fridge_wake_word_state_t state, const char *error)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = state;
    if (error) {
        set_error_locked(error);
    }
    xSemaphoreGive(s_lock);
}

const char *fridge_wake_word_state_text(fridge_wake_word_state_t state)
{
    switch (state) {
    case FRIDGE_WAKE_WORD_STATE_LISTENING:
        return "listening";
    case FRIDGE_WAKE_WORD_STATE_ERROR:
        return "error";
    case FRIDGE_WAKE_WORD_STATE_IDLE:
    default:
        return "idle";
    }
}

static void update_audio_stats_locked(const int16_t *samples, size_t sample_count)
{
    uint64_t sum_squares = 0;
    int32_t peak = 0;
    for (size_t i = 0; i < sample_count; i++) {
        int32_t sample = samples[i];
        int32_t abs_sample = sample == INT16_MIN ? 32768 : (sample < 0 ? -sample : sample);
        if (abs_sample > peak) {
            peak = abs_sample;
        }
        sum_squares += (uint64_t)((int32_t)sample * (int32_t)sample);
    }
    s_peak_abs = peak;
    s_rms = sample_count > 0 ? (int32_t)sqrt((double)sum_squares / (double)sample_count) : 0;
}

static bool should_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool stop = s_stop_requested;
    xSemaphoreGive(s_lock);
    return stop;
}

static void notify_wake_detected(void)
{
    fridge_wake_word_event_cb_t cb = NULL;
    void *ctx = NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_last_trigger_ms == 0 || now_ms - s_last_trigger_ms >= WAKE_TRIGGER_GUARD_MS) {
        s_trigger_count++;
        s_last_trigger_ms = now_ms;
        cb = s_event_cb;
        ctx = s_event_ctx;
    }
    xSemaphoreGive(s_lock);

    if (cb) {
        cb(ctx);
    }
}

static void wake_task(void *arg)
{
    (void)arg;
    int feed_chunksize = s_afe_handle->get_feed_chunksize(s_afe_data);
    int feed_channel = s_afe_handle->get_feed_channel_num(s_afe_data);
    size_t frame_samples = (size_t)feed_chunksize * (size_t)feed_channel;
    int16_t *frame = heap_caps_malloc(frame_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame) {
        frame = heap_caps_malloc(frame_samples * sizeof(int16_t), MALLOC_CAP_8BIT);
    }
    if (!frame) {
        set_state(FRIDGE_WAKE_WORD_STATE_ERROR, "wake frame allocation failed");
        goto done;
    }

    esp_err_t err = fridge_audio_wake_stream_start();
    if (err != ESP_OK) {
        set_state(FRIDGE_WAKE_WORD_STATE_ERROR, esp_err_to_name(err));
        goto done;
    }

    set_state(FRIDGE_WAKE_WORD_STATE_LISTENING, "");
    while (!should_stop()) {
        size_t sample_count = 0;
        err = fridge_audio_wake_stream_read(frame, frame_samples, &sample_count, pdMS_TO_TICKS(250));
        if (err != ESP_OK) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_timeout_count++;
            xSemaphoreGive(s_lock);
            if (err != ESP_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "wake stream read failed: %s", esp_err_to_name(err));
            }
            continue;
        }
        if (sample_count != frame_samples) {
            continue;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        update_audio_stats_locked(frame, frame_samples);
        xSemaphoreGive(s_lock);

        int feed_ret = s_afe_handle->feed(s_afe_data, frame);
        if (feed_ret < 0) {
            ESP_LOGW(TAG, "AFE feed failed: %d", feed_ret);
            continue;
        }

        afe_fetch_result_t *result = s_afe_handle->fetch_with_delay
                                         ? s_afe_handle->fetch_with_delay(s_afe_data, pdMS_TO_TICKS(20))
                                         : s_afe_handle->fetch(s_afe_data);
        if (!result || result->ret_value == ESP_FAIL) {
            continue;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_vad_state = result->vad_state;
        xSemaphoreGive(s_lock);
        if (result->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "wake word detected: %s", FRIDGE_WAKE_WORD_TEXT);
            notify_wake_detected();
        }
    }

    (void)fridge_audio_wake_stream_stop();
    set_state(FRIDGE_WAKE_WORD_STATE_IDLE, "");

done:
    free(frame);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_enabled = false;
    s_stop_requested = false;
    s_task = NULL;
    xSemaphoreGive(s_lock);
    vTaskDelete(NULL);
}

esp_err_t fridge_wake_word_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    // ESP-SR 模型由固定 label 为 model 的分区加载，烧录前必须确认 partitions.csv 中存在该分区。
    s_models = esp_srmodel_init("model");
    if (!s_models) {
        set_state(FRIDGE_WAKE_WORD_STATE_ERROR, "load ESP-SR models failed");
        return ESP_FAIL;
    }
    char model_name[] = FRIDGE_WAKE_WORD_MODEL;
    if (esp_srmodel_exists(s_models, model_name) < 0) {
        set_state(FRIDGE_WAKE_WORD_STATE_ERROR, "wake model not found in model partition");
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    // 单麦克风 INMP441 只提供一个 16kHz mono 通道，因此输入格式必须是 "M"，不能声明播放参考通道。
    afe_config_t *afe_config = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!afe_config) {
        set_state(FRIDGE_WAKE_WORD_STATE_ERROR, "AFE config init failed");
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_ERR_NO_MEM;
    }
    afe_config->aec_init = false;
    afe_config->se_init = false;
    afe_config->vad_init = true;
    afe_config->wakenet_init = true;
    afe_config->wakenet_model_name = FRIDGE_WAKE_WORD_MODEL;
    // 用户实测单纯放大麦克风输入会降低触发率；这里改用 WakeNet 自带的 aggressive 检测模式提高灵敏度。
    afe_config->wakenet_mode = DET_MODE_95;
    afe_config->agc_init = true;
    afe_config->fixed_first_channel = true;
    afe_config->fixed_output_channel = true;
    afe_config->afe_perferred_core = 1;
    afe_config->afe_perferred_priority = WAKE_TASK_PRIORITY;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    s_afe_handle = esp_afe_handle_from_config(afe_config);
    if (!s_afe_handle) {
        afe_config_free(afe_config);
        set_state(FRIDGE_WAKE_WORD_STATE_ERROR, "AFE handle init failed");
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_FAIL;
    }
    s_afe_data = s_afe_handle->create_from_config(afe_config);
    afe_config_free(afe_config);
    if (!s_afe_data) {
        set_state(FRIDGE_WAKE_WORD_STATE_ERROR, "AFE data create failed");
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_FAIL;
    }
    if (s_afe_handle->get_samp_rate(s_afe_data) != FRIDGE_AUDIO_SAMPLE_RATE) {
        set_state(FRIDGE_WAKE_WORD_STATE_ERROR, "AFE sample rate mismatch");
        s_afe_handle->destroy(s_afe_data);
        s_afe_data = NULL;
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_ERR_INVALID_STATE;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "wake word ready: %s / %s", FRIDGE_WAKE_WORD_TEXT, FRIDGE_WAKE_WORD_MODEL);
    s_afe_handle->print_pipeline(s_afe_data);
    return ESP_OK;
}

esp_err_t fridge_wake_word_set_event_callback(fridge_wake_word_event_cb_t cb, void *user_ctx)
{
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "wake word not initialized");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_event_cb = cb;
    s_event_ctx = user_ctx;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_wake_word_start(void)
{
    esp_err_t err = fridge_wake_word_init();
    if (err != ESP_OK) {
        return err;
    }

    fridge_speaker_status_t speaker = {0};
    if (fridge_speaker_get_status(&speaker) == ESP_OK &&
        (speaker.state == FRIDGE_SPEAKER_STATE_SYNTHESIZING || speaker.state == FRIDGE_SPEAKER_STATE_PLAYING)) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_task) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    s_stop_requested = false;
    s_enabled = true;
    s_timeout_count = 0;
    s_error[0] = '\0';
    xSemaphoreGive(s_lock);

    BaseType_t ok = xTaskCreatePinnedToCore(wake_task, "wake_word", WAKE_TASK_STACK, NULL, WAKE_TASK_PRIORITY, &s_task, 1);
    if (ok != pdPASS) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_enabled = false;
        s_task = NULL;
        xSemaphoreGive(s_lock);
        set_state(FRIDGE_WAKE_WORD_STATE_ERROR, "wake task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t fridge_wake_word_stop(void)
{
    if (!s_initialized || !s_lock) {
        return ESP_OK;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    TaskHandle_t task = s_task;
    s_stop_requested = true;
    xSemaphoreGive(s_lock);

    for (int i = 0; task && i < (WAKE_STOP_WAIT_MS / 20); i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        task = s_task;
        xSemaphoreGive(s_lock);
    }
    if (task) {
        set_state(FRIDGE_WAKE_WORD_STATE_ERROR, "wake task stop timeout");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t fridge_wake_word_reset_stats(void)
{
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "wake word not initialized");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_trigger_count = 0;
    s_last_trigger_ms = 0;
    s_timeout_count = 0;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_wake_word_get_status(fridge_wake_word_status_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!s_lock) {
        out->state = FRIDGE_WAKE_WORD_STATE_IDLE;
        strlcpy(out->wake_word, FRIDGE_WAKE_WORD_TEXT, sizeof(out->wake_word));
        strlcpy(out->model, FRIDGE_WAKE_WORD_MODEL, sizeof(out->model));
        strlcpy(out->error, "wake word not initialized", sizeof(out->error));
        return ESP_OK;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->enabled = s_enabled;
    out->state = s_state;
    out->trigger_count = s_trigger_count;
    out->last_trigger_ms = s_last_trigger_ms;
    out->vad_state = s_vad_state;
    out->rms = s_rms;
    out->peak_abs = s_peak_abs;
    out->timeout_count = s_timeout_count;
    strlcpy(out->wake_word, FRIDGE_WAKE_WORD_TEXT, sizeof(out->wake_word));
    strlcpy(out->model, FRIDGE_WAKE_WORD_MODEL, sizeof(out->model));
    strlcpy(out->error, s_error, sizeof(out->error));
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
