// 冰箱小精灵音频采集组件。
// 使用 INMP441 I2S 数字麦克风采集短语音片段，首版只做录音和转写输入，不做扬声器播放。
#include "fridge_audio.h"

#include <math.h>
#include <string.h>
#include "driver/i2s_std.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define MIC_BCLK_GPIO 40
#define MIC_WS_GPIO 41
#define MIC_DIN_GPIO 42
#define AUDIO_TASK_STACK 4096
#define AUDIO_DMA_DESC_NUM 4
#define AUDIO_DMA_FRAME_NUM 256
#define AUDIO_READ_FRAMES 256
#define AUDIO_WAKE_READ_FRAMES 256
#define AUDIO_PCM_SHIFT 14
#define AUDIO_CLIP_THRESHOLD 32000

static const char *TAG = "fridge_audio";

static i2s_chan_handle_t s_rx_chan;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_record_task;
static bool s_wake_stream_active;
static int16_t *s_pcm;
static size_t s_pcm_capacity;
static size_t s_pcm_bytes;
static int64_t s_record_start_us;
static uint32_t s_duration_ms;
static int32_t s_rms;
static int16_t s_min_sample;
static int16_t s_max_sample;
static int32_t s_mean_sample;
static int32_t s_peak_abs;
static uint32_t s_clip_count;
static uint32_t s_sample_count;
static uint64_t s_sum_squares;
static int64_t s_sum_samples;
static fridge_audio_state_t s_state = FRIDGE_AUDIO_STATE_IDLE;
static char s_error[128];
static bool s_initialized;
static bool s_i2s_enabled;
static uint32_t s_i2s_timeout_count;

static void set_state(fridge_audio_state_t state, const char *error)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = state;
    strlcpy(s_error, error ? error : "", sizeof(s_error));
    xSemaphoreGive(s_lock);
}

static int16_t raw_to_pcm16(int32_t raw)
{
    // INMP441 常见为 24-bit 有符号数据左对齐到 32-bit slot。
    // 这里集中处理缩放，后续排查音量过小/削顶时只需调整 AUDIO_PCM_SHIFT。
    int32_t sample = raw >> AUDIO_PCM_SHIFT;
    if (sample > INT16_MAX) {
        return INT16_MAX;
    }
    if (sample < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)sample;
}

static int32_t abs_i16(int16_t value)
{
    return value == INT16_MIN ? 32768 : (value < 0 ? -value : value);
}

static void reset_stats_locked(void)
{
    s_duration_ms = 0;
    s_rms = 0;
    s_min_sample = INT16_MAX;
    s_max_sample = INT16_MIN;
    s_mean_sample = 0;
    s_peak_abs = 0;
    s_clip_count = 0;
    s_sample_count = 0;
    s_sum_squares = 0;
    s_sum_samples = 0;
    s_i2s_timeout_count = 0;
}

static void update_stats_locked(const int16_t *samples, size_t sample_count)
{
    for (size_t i = 0; i < sample_count; i++) {
        int16_t sample = samples[i];
        int32_t abs_sample = abs_i16(sample);
        if (sample < s_min_sample) {
            s_min_sample = sample;
        }
        if (sample > s_max_sample) {
            s_max_sample = sample;
        }
        if (abs_sample > s_peak_abs) {
            s_peak_abs = abs_sample;
        }
        if (abs_sample >= AUDIO_CLIP_THRESHOLD) {
            s_clip_count++;
        }
        s_sum_samples += sample;
        s_sum_squares += (uint64_t)((int32_t)sample * (int32_t)sample);
        s_sample_count++;
    }
    if (s_sample_count > 0) {
        s_mean_sample = (int32_t)(s_sum_samples / (int64_t)s_sample_count);
        s_rms = (int32_t)sqrt((double)s_sum_squares / (double)s_sample_count);
    }
}

static void record_task(void *arg)
{
    (void)arg;
    int32_t raw[AUDIO_READ_FRAMES];
    size_t bytes_read = 0;

    while (true) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool recording = s_state == FRIDGE_AUDIO_STATE_RECORDING;
        size_t offset = s_pcm_bytes;
        xSemaphoreGive(s_lock);
        if (!recording || offset >= s_pcm_capacity) {
            break;
        }

        esp_err_t err = i2s_channel_read(s_rx_chan, raw, sizeof(raw), &bytes_read, pdMS_TO_TICKS(250));
        if (err != ESP_OK) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (err == ESP_ERR_TIMEOUT) {
                // 麦克风未接好或 I2S 边沿不稳定时可能连续超时；限频记录，避免录音期间刷爆 USB 日志。
                s_i2s_timeout_count++;
                if (s_i2s_timeout_count == 1 || (s_i2s_timeout_count % 32) == 0) {
                    ESP_LOGW(TAG, "i2s read timeout, count=%lu", (unsigned long)s_i2s_timeout_count);
                }
            } else {
                ESP_LOGW(TAG, "i2s read failed: %s", esp_err_to_name(err));
            }
            xSemaphoreGive(s_lock);
            continue;
        }
        size_t frames = bytes_read / sizeof(raw[0]);
        if (frames == 0) {
            continue;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        size_t writable = (s_pcm_capacity - s_pcm_bytes) / sizeof(int16_t);
        if (frames > writable) {
            frames = writable;
        }
        int16_t *write_ptr = &s_pcm[s_pcm_bytes / sizeof(int16_t)];
        for (size_t i = 0; i < frames; i++) {
            write_ptr[i] = raw_to_pcm16(raw[i]);
        }
        s_pcm_bytes += frames * sizeof(int16_t);
        update_stats_locked(write_ptr, frames);
        s_duration_ms = (uint32_t)(s_sample_count * 1000 / FRIDGE_AUDIO_SAMPLE_RATE);
        bool full = s_pcm_bytes >= s_pcm_capacity;
        xSemaphoreGive(s_lock);
        if (full) {
            ESP_LOGI(TAG, "record buffer full, auto stop");
            break;
        }
    }

    bool should_disable = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state == FRIDGE_AUDIO_STATE_RECORDING || s_state == FRIDGE_AUDIO_STATE_WAKE_LISTENING || s_wake_stream_active) {
        s_state = s_pcm_bytes > 0 ? FRIDGE_AUDIO_STATE_READY : FRIDGE_AUDIO_STATE_IDLE;
    }
    if (s_i2s_enabled) {
        s_i2s_enabled = false;
        should_disable = true;
    }
    s_record_task = NULL;
    xSemaphoreGive(s_lock);
    if (should_disable) {
        esp_err_t err = i2s_channel_disable(s_rx_chan);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s disable failed: %s", esp_err_to_name(err));
        }
    }
    vTaskDelete(NULL);
}

esp_err_t fridge_audio_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    s_pcm_capacity = FRIDGE_AUDIO_MAX_PCM_BYTES;
    s_pcm = heap_caps_malloc(s_pcm_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_pcm) {
        s_pcm = heap_caps_malloc(s_pcm_capacity, MALLOC_CAP_8BIT);
    }
    if (!s_pcm) {
        return ESP_ERR_NO_MEM;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = AUDIO_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = AUDIO_DMA_FRAME_NUM;
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s channel create failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(FRIDGE_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_BCLK_GPIO,
            .ws = MIC_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    err = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s std init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "INMP441 ready: SCK=GPIO%d WS=GPIO%d SD=GPIO%d sample_rate=%d", MIC_BCLK_GPIO, MIC_WS_GPIO, MIC_DIN_GPIO, FRIDGE_AUDIO_SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t fridge_audio_start_recording(void)
{
    esp_err_t err = fridge_audio_init();
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state == FRIDGE_AUDIO_STATE_RECORDING || s_state == FRIDGE_AUDIO_STATE_WAKE_LISTENING ||
        s_wake_stream_active || s_i2s_enabled) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_pcm_bytes = 0;
    reset_stats_locked();
    s_error[0] = '\0';
    s_record_start_us = esp_timer_get_time();
    s_state = FRIDGE_AUDIO_STATE_RECORDING;
    xSemaphoreGive(s_lock);

    err = i2s_channel_enable(s_rx_chan);
    if (err != ESP_OK) {
        set_state(FRIDGE_AUDIO_STATE_ERROR, "i2s enable failed");
        ESP_LOGE(TAG, "i2s enable failed: %s", esp_err_to_name(err));
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_i2s_enabled = true;
    xSemaphoreGive(s_lock);

    BaseType_t ok = xTaskCreate(record_task, "audio_record", AUDIO_TASK_STACK, NULL, 6, &s_record_task);
    if (ok != pdPASS) {
        (void)i2s_channel_disable(s_rx_chan);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_i2s_enabled = false;
        xSemaphoreGive(s_lock);
        set_state(FRIDGE_AUDIO_STATE_ERROR, "audio record task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t fridge_audio_stop_recording(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state != FRIDGE_AUDIO_STATE_RECORDING) {
        bool ready = s_state == FRIDGE_AUDIO_STATE_READY;
        xSemaphoreGive(s_lock);
        return ready ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    s_state = FRIDGE_AUDIO_STATE_READY;
    s_duration_ms = (uint32_t)((esp_timer_get_time() - s_record_start_us) / 1000);
    xSemaphoreGive(s_lock);

    for (int i = 0; i < 50 && s_record_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_record_task) {
        set_state(FRIDGE_AUDIO_STATE_ERROR, "audio record task stop timeout");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t fridge_audio_wake_stream_start(void)
{
    esp_err_t err = fridge_audio_init();
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state == FRIDGE_AUDIO_STATE_RECORDING || s_wake_stream_active || s_i2s_enabled) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_wake_stream_active = true;
    s_i2s_enabled = true;
    s_state = FRIDGE_AUDIO_STATE_WAKE_LISTENING;
    s_error[0] = '\0';
    reset_stats_locked();
    xSemaphoreGive(s_lock);

    // 唤醒监听长期占用 I2S RX，只输出短帧 PCM 给 ESP-SR，不缓存完整语音，避免长期占用 PSRAM。
    err = i2s_channel_enable(s_rx_chan);
    if (err != ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_wake_stream_active = false;
        s_i2s_enabled = false;
        s_state = FRIDGE_AUDIO_STATE_ERROR;
        strlcpy(s_error, "wake i2s enable failed", sizeof(s_error));
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "wake i2s enable failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

esp_err_t fridge_audio_wake_stream_read(int16_t *out_samples, size_t max_samples, size_t *sample_count, TickType_t timeout_ticks)
{
    if (!out_samples || !sample_count || max_samples == 0 || !s_initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    *sample_count = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool active = s_wake_stream_active && s_state == FRIDGE_AUDIO_STATE_WAKE_LISTENING;
    xSemaphoreGive(s_lock);
    if (!active) {
        return ESP_ERR_INVALID_STATE;
    }

    int32_t raw[AUDIO_WAKE_READ_FRAMES];
    size_t total_frames = 0;
    while (total_frames < max_samples) {
        size_t wanted = max_samples - total_frames;
        if (wanted > AUDIO_WAKE_READ_FRAMES) {
            wanted = AUDIO_WAKE_READ_FRAMES;
        }
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_rx_chan, raw, wanted * sizeof(raw[0]), &bytes_read, timeout_ticks);
        if (err != ESP_OK) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (err == ESP_ERR_TIMEOUT) {
                s_i2s_timeout_count++;
            } else {
                strlcpy(s_error, esp_err_to_name(err), sizeof(s_error));
            }
            xSemaphoreGive(s_lock);
            if (total_frames > 0) {
                break;
            }
            return err;
        }

        size_t frames = bytes_read / sizeof(raw[0]);
        if (frames == 0) {
            break;
        }
        for (size_t i = 0; i < frames; i++) {
            out_samples[total_frames + i] = raw_to_pcm16(raw[i]);
        }
        total_frames += frames;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    update_stats_locked(out_samples, total_frames);
    s_duration_ms = (uint32_t)(s_sample_count * 1000 / FRIDGE_AUDIO_SAMPLE_RATE);
    xSemaphoreGive(s_lock);
    *sample_count = total_frames;
    return ESP_OK;
}

esp_err_t fridge_audio_wake_stream_stop(void)
{
    if (!s_initialized || !s_lock) {
        return ESP_OK;
    }

    bool should_disable = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_wake_stream_active) {
        s_wake_stream_active = false;
        should_disable = s_i2s_enabled;
        s_i2s_enabled = false;
        s_state = FRIDGE_AUDIO_STATE_IDLE;
    }
    xSemaphoreGive(s_lock);

    if (should_disable) {
        esp_err_t err = i2s_channel_disable(s_rx_chan);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "wake i2s disable failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

static const char *quality_hint_locked(void)
{
    if (s_sample_count == 0 || s_pcm_bytes == 0) {
        return "too_short";
    }
    if (s_i2s_timeout_count > 0) {
        return "i2s_timeout";
    }
    if (s_clip_count > 0) {
        return "clipping";
    }
    if (s_duration_ms < 500 || s_pcm_bytes < 16000) {
        return "too_short";
    }
    if (s_rms < 80 || s_peak_abs < 400) {
        return "silent";
    }
    return "ok";
}

esp_err_t fridge_audio_get_status(fridge_audio_status_t *out)
{
    if (!out || !s_lock) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->state = s_state;
    out->pcm_bytes = s_pcm_bytes;
    out->duration_ms = s_duration_ms;
    out->rms = s_rms;
    out->min_sample = s_sample_count > 0 ? s_min_sample : 0;
    out->max_sample = s_sample_count > 0 ? s_max_sample : 0;
    out->mean_sample = s_mean_sample;
    out->peak_abs = s_peak_abs;
    out->clip_count = s_clip_count;
    out->timeout_count = s_i2s_timeout_count;
    out->sample_count = s_sample_count;
    strlcpy(out->quality_hint, quality_hint_locked(), sizeof(out->quality_hint));
    strlcpy(out->error, s_error, sizeof(out->error));
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_audio_get_pcm(const int16_t **pcm, size_t *pcm_bytes, uint32_t *duration_ms)
{
    if (!pcm || !pcm_bytes || !duration_ms || !s_lock) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *pcm = s_pcm;
    *pcm_bytes = s_pcm_bytes;
    *duration_ms = s_duration_ms;
    bool ready = s_state == FRIDGE_AUDIO_STATE_READY && s_pcm_bytes > 0;
    xSemaphoreGive(s_lock);
    return ready ? ESP_OK : ESP_ERR_INVALID_STATE;
}
