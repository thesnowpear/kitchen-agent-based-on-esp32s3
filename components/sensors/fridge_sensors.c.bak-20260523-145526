// 冰箱小精灵传感器组件。
// 当前首版只接入 GPIO1/ADC1_CH0 光敏模拟量，后续 IMU、雷达和门状态机可复用这里的快照接口。
#include "fridge_sensors.h"

#include <string.h>
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define LIGHT_ADC_UNIT ADC_UNIT_1
#define LIGHT_ADC_CHANNEL ADC_CHANNEL_0
#define LIGHT_SAMPLE_PERIOD_MS 200
#define LIGHT_BASELINE_WINDOW 5
#define SENSOR_TASK_STACK 3072

static const char *TAG = "fridge_sensors";

static adc_oneshot_unit_handle_t s_adc_unit;
static SemaphoreHandle_t s_lock;
static fridge_sensor_snapshot_t s_snapshot;
static bool s_initialized;

static uint16_t clamp_light_raw(int raw)
{
    if (raw < 0) {
        raw = 0;
    }
    if (raw > FRIDGE_LIGHT_ADC_MAX_RAW) {
        raw = FRIDGE_LIGHT_ADC_MAX_RAW;
    }
    return (uint16_t)raw;
}

static uint16_t raw_to_brightness_10bit(int raw)
{
    // 当前光敏模块是反向输出：ADC 原始值越高越暗。这里统一换算成亮度值，便于状态机用“变亮”为正方向。
    uint16_t clamped = clamp_light_raw(raw);
    int brightness_raw = FRIDGE_LIGHT_ADC_MAX_RAW - (int)clamped;
    return (uint16_t)((brightness_raw * FRIDGE_LIGHT_VALUE_MAX_10BIT) / FRIDGE_LIGHT_ADC_MAX_RAW);
}

static void publish_light_sample(int raw, int baseline)
{
    uint16_t clamped_raw = clamp_light_raw(raw);
    uint16_t brightness_10bit = raw_to_brightness_10bit(raw);
    uint16_t baseline_brightness_10bit = raw_to_brightness_10bit(baseline);
    fridge_sensor_snapshot_t next = {
        .ready = true,
        .light_raw_12bit = clamped_raw,
        .light_value_10bit = brightness_10bit,
        .light_percent = (uint8_t)((brightness_10bit * 100) / FRIDGE_LIGHT_VALUE_MAX_10BIT),
        .light_delta = (int16_t)((int)brightness_10bit - (int)baseline_brightness_10bit),
        .updated_at_ms = esp_timer_get_time() / 1000,
    };

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_snapshot = next;
    xSemaphoreGive(s_lock);
}

static void sensor_task(void *arg)
{
    (void)arg;
    int window[LIGHT_BASELINE_WINDOW] = {0};
    size_t index = 0;
    size_t count = 0;

    while (true) {
        int raw = 0;
        esp_err_t err = adc_oneshot_read(s_adc_unit, LIGHT_ADC_CHANNEL, &raw);
        if (err == ESP_OK) {
            window[index] = raw;
            index = (index + 1) % LIGHT_BASELINE_WINDOW;
            if (count < LIGHT_BASELINE_WINDOW) {
                count++;
            }

            int sum = 0;
            for (size_t i = 0; i < count; i++) {
                sum += window[i];
            }
            int baseline = count ? (sum / (int)count) : raw;
            publish_light_sample(raw, baseline);
        } else {
            ESP_LOGW(TAG, "light adc read failed: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(LIGHT_SAMPLE_PERIOD_MS));
    }
}

esp_err_t fridge_sensors_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    // GPIO1 是 ESP32-S3 的 ADC1_CH0。这里只读取模拟输入，不输出电平，避免对光敏模块反灌电。
    // 当前实测光敏模块为反向输出：ADC 原始值越高越暗，业务快照会换算成“亮度越高数值越高”。
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = LIGHT_ADC_UNIT,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc_unit);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc unit init failed: %s", esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    err = adc_oneshot_config_channel(s_adc_unit, LIGHT_ADC_CHANNEL, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc channel config failed: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t ok = xTaskCreate(sensor_task, "sensor_task", SENSOR_TASK_STACK, NULL, 5, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "light sensor ready on GPIO%d ADC1_CH0, period=%d ms", FRIDGE_LIGHT_ADC_GPIO, LIGHT_SAMPLE_PERIOD_MS);
    return ESP_OK;
}

esp_err_t fridge_sensors_get_snapshot(fridge_sensor_snapshot_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_snapshot;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
