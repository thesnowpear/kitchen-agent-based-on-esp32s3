// 冰箱小精灵传感器组件。
// 当前首版同时采集 GPIO1 光敏模拟量与 MPU6050 六轴姿态数据，给 Web 面板和后续状态机复用。
// 硬件注意：GPIO1 仅用于 3.3V 模拟输入；MPU6050 走 GPIO4/GPIO5 的 I2C 总线，AD0 低电平时地址为 0x68。

#include "fridge_sensors.h"

#include <math.h>
#include <string.h>
#include "esp_attr.h"
#include "driver/i2c.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fridge_radar.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define LIGHT_ADC_UNIT ADC_UNIT_1
#define LIGHT_ADC_CHANNEL ADC_CHANNEL_0
#define LIGHT_SAMPLE_PERIOD_MS 200
#define LIGHT_BASELINE_WINDOW 5
#define SENSOR_TASK_STACK 4096
#define IMU_SAMPLE_PERIOD_MS 20
#define IMU_WHO_AM_I_REG 0x75
#define IMU_PWR_MGMT_1_REG 0x6B
#define IMU_SMPLRT_DIV_REG 0x19
#define IMU_CONFIG_REG 0x1A
#define IMU_GYRO_CONFIG_REG 0x1B
#define IMU_ACCEL_CONFIG_REG 0x1C
#define IMU_ACCEL_XOUT_H_REG 0x3B
#define IMU_TEMP_OUT_H_REG 0x41
#define IMU_GYRO_XOUT_H_REG 0x43
#define IMU_EXPECTED_WHO_AM_I 0x68
#define FRIDGE_PI 3.14159265358979323846f

static const char *TAG = "fridge_sensors";

static adc_oneshot_unit_handle_t s_adc_unit;
static SemaphoreHandle_t s_lock;
static fridge_sensor_snapshot_t s_snapshot;
static bool s_initialized;
static bool s_imu_bus_ready;

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

static esp_err_t imu_write_reg(uint8_t reg, uint8_t value)
{
    return i2c_master_write_to_device(FRIDGE_IMU_I2C_PORT, FRIDGE_IMU_DEFAULT_ADDR, (uint8_t[]){reg, value}, 2, pdMS_TO_TICKS(100));
}

static esp_err_t imu_read_bytes(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_write_read_device(FRIDGE_IMU_I2C_PORT, FRIDGE_IMU_DEFAULT_ADDR, &reg, 1, data, len, pdMS_TO_TICKS(100));
}

static float accel_lsb_to_g(int16_t raw)
{
    return (float)raw / 16384.0f;
}

static float gyro_lsb_to_dps(int16_t raw)
{
    return (float)raw / 131.0f;
}

static void update_snapshot(const fridge_sensor_snapshot_t *next)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_snapshot = *next;
    xSemaphoreGive(s_lock);
}

static void publish_light_sample(int raw, int baseline)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_snapshot.ready = true;
    s_snapshot.light_raw_12bit = clamp_light_raw(raw);
    s_snapshot.light_value_10bit = raw_to_brightness_10bit(raw);
    s_snapshot.light_percent = (uint8_t)((s_snapshot.light_value_10bit * 100) / FRIDGE_LIGHT_VALUE_MAX_10BIT);
    s_snapshot.light_delta = (int16_t)((int)s_snapshot.light_value_10bit - (int)raw_to_brightness_10bit(baseline));
    s_snapshot.updated_at_ms = esp_timer_get_time() / 1000;
    xSemaphoreGive(s_lock);
}

static esp_err_t imu_init(void)
{
    // MPU6050 直接挂在 ESP32-S3 的 I2C 总线上，首轮先跑 100kHz，确认读数稳定后再考虑提速。
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = FRIDGE_IMU_I2C_SDA_GPIO,
        .scl_io_num = FRIDGE_IMU_I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
        .clk_flags = 0,
    };
    esp_err_t err = i2c_param_config(FRIDGE_IMU_I2C_PORT, &conf);
    if (err != ESP_OK) {
        s_imu_bus_ready = false;
        return err;
    }

    err = i2c_driver_install(FRIDGE_IMU_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        s_imu_bus_ready = false;
        return err;
    }

    s_imu_bus_ready = true;
    vTaskDelay(pdMS_TO_TICKS(100));

    uint8_t who_am_i = 0;
    err = imu_read_bytes(IMU_WHO_AM_I_REG, &who_am_i, 1);
    if (err != ESP_OK) {
        s_imu_bus_ready = false;
        return err;
    }

    if (who_am_i != IMU_EXPECTED_WHO_AM_I) {
        s_imu_bus_ready = false;
        return ESP_ERR_NOT_FOUND;
    }

    // 复位后退出休眠，先用低量程做静态和轻微动作测试，减少首轮饱和风险。
    err = imu_write_reg(IMU_PWR_MGMT_1_REG, 0x80);
    if (err != ESP_OK) {
        s_imu_bus_ready = false;
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    err = imu_write_reg(IMU_PWR_MGMT_1_REG, 0x00);
    if (err != ESP_OK) {
        s_imu_bus_ready = false;
        return err;
    }
    err = imu_write_reg(IMU_SMPLRT_DIV_REG, 0x07);
    if (err != ESP_OK) {
        s_imu_bus_ready = false;
        return err;
    }
    err = imu_write_reg(IMU_CONFIG_REG, 0x03);
    if (err != ESP_OK) {
        s_imu_bus_ready = false;
        return err;
    }
    err = imu_write_reg(IMU_ACCEL_CONFIG_REG, 0x00);
    if (err != ESP_OK) {
        s_imu_bus_ready = false;
        return err;
    }
    err = imu_write_reg(IMU_GYRO_CONFIG_REG, 0x00);
    if (err != ESP_OK) {
        s_imu_bus_ready = false;
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_snapshot.imu_ready = true;
    s_snapshot.imu_address = FRIDGE_IMU_DEFAULT_ADDR;
    s_snapshot.imu_who_am_i = who_am_i;
    s_snapshot.imu_error = 0;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "MPU6050 ready at 0x%02X, WHO_AM_I=0x%02X", FRIDGE_IMU_DEFAULT_ADDR, who_am_i);
    return ESP_OK;
}

static esp_err_t imu_read_sample(fridge_sensor_snapshot_t *snapshot)
{
    uint8_t raw[14] = {0};
    esp_err_t err = imu_read_bytes(IMU_ACCEL_XOUT_H_REG, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    int16_t ax = (int16_t)((raw[0] << 8) | raw[1]);
    int16_t ay = (int16_t)((raw[2] << 8) | raw[3]);
    int16_t az = (int16_t)((raw[4] << 8) | raw[5]);
    int16_t temp = (int16_t)((raw[6] << 8) | raw[7]);
    int16_t gx = (int16_t)((raw[8] << 8) | raw[9]);
    int16_t gy = (int16_t)((raw[10] << 8) | raw[11]);
    int16_t gz = (int16_t)((raw[12] << 8) | raw[13]);

    snapshot->accel_x_g = accel_lsb_to_g(ax);
    snapshot->accel_y_g = accel_lsb_to_g(ay);
    snapshot->accel_z_g = accel_lsb_to_g(az);
    snapshot->gyro_x_dps = gyro_lsb_to_dps(gx);
    snapshot->gyro_y_dps = gyro_lsb_to_dps(gy);
    snapshot->gyro_z_dps = gyro_lsb_to_dps(gz);
    snapshot->imu_temperature_c = ((float)temp / 340.0f) + 36.53f;
    snapshot->pitch_deg = atan2f(snapshot->accel_y_g, sqrtf((snapshot->accel_x_g * snapshot->accel_x_g) + (snapshot->accel_z_g * snapshot->accel_z_g))) * (180.0f / FRIDGE_PI);
    snapshot->roll_deg = atan2f(-snapshot->accel_x_g, snapshot->accel_z_g) * (180.0f / FRIDGE_PI);
    snapshot->angle_delta = fabsf(snapshot->pitch_deg) + fabsf(snapshot->roll_deg);
    snapshot->vibration_peak = fmaxf(fmaxf(fabsf(snapshot->accel_x_g), fabsf(snapshot->accel_y_g)), fabsf(snapshot->accel_z_g));
    snapshot->imu_error = 0;
    snapshot->imu_ready = true;
    snapshot->imu_address = FRIDGE_IMU_DEFAULT_ADDR;
    snapshot->imu_who_am_i = IMU_EXPECTED_WHO_AM_I;
    return ESP_OK;
}

static void sensor_task(void *arg)
{
    (void)arg;
    int window[LIGHT_BASELINE_WINDOW] = {0};
    size_t index = 0;
    size_t count = 0;
    fridge_sensor_snapshot_t local = {0};

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

        xSemaphoreTake(s_lock, portMAX_DELAY);
        local = s_snapshot;
        xSemaphoreGive(s_lock);

        if (s_imu_bus_ready) {
            err = imu_read_sample(&local);
            if (err == ESP_OK) {
                local.updated_at_ms = esp_timer_get_time() / 1000;
                update_snapshot(&local);
            } else {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_snapshot.imu_ready = false;
                s_snapshot.imu_error = err;
                xSemaphoreGive(s_lock);
                if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_TIMEOUT) {
                    s_imu_bus_ready = false;
                }
                ESP_LOGW(TAG, "mpu6050 read failed: %s", esp_err_to_name(err));
            }
        }

        // 雷达组件独立维护 UART 采集任务，这里只汇总最近快照，避免传感器任务阻塞在串口读取上。
        (void)fridge_radar_get_snapshot(&local.radar);
        update_snapshot(&local);

        vTaskDelay(pdMS_TO_TICKS(IMU_SAMPLE_PERIOD_MS));
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

    err = imu_init();
    if (err != ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_snapshot.imu_ready = false;
        s_snapshot.imu_error = err;
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "mpu6050 init failed: %s", esp_err_to_name(err));
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
