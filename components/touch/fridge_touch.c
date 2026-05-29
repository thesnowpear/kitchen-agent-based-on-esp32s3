// 冰箱小精灵 FT6336U 触摸驱动。
// 使用 ESP-IDF legacy I2C API 复用 sensors 已初始化的 I2C0，总线同时挂 MPU6050、FT6336U 和 OV3660 SCCB。
// 硬件注意：TP_RST 当前不接，GPIO16 已让给摄像头 D7；I2C 上拉必须到 3.3V，不能上拉到 5V。

#include "fridge_touch.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "fridge_sensors.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define FT6336U_REG_TD_STATUS 0x02
#define FT6336U_REG_P1_XH 0x03
#define FT6336U_REG_CHIP_ID 0xA3
#define FT6336U_REG_VENDOR_ID 0xA8
#define TOUCH_READ_TIMEOUT_MS 50

static const char *TAG = "fridge_touch";
static bool s_initialized;

static esp_err_t touch_read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_write_read_device(FRIDGE_IMU_I2C_PORT,
                                        FRIDGE_TOUCH_I2C_ADDR,
                                        &reg,
                                        1,
                                        data,
                                        len,
                                        pdMS_TO_TICKS(TOUCH_READ_TIMEOUT_MS));
}

esp_err_t fridge_touch_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    // 确保 I2C0 已按 GPIO4/GPIO5 初始化。该调用是幂等的，但会启动传感器采样任务。
    esp_err_t ret = fridge_sensors_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "sensors init before touch failed: %s", esp_err_to_name(ret));
    }

    gpio_config_t int_conf = {
        .pin_bit_mask = 1ULL << FRIDGE_TOUCH_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&int_conf));

    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t id = 0;
    uint8_t vendor = 0;
    ret = touch_read_reg(FT6336U_REG_CHIP_ID, &id, 1);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "FT6336U chip id read failed at fixed addr 0x%02X: %s",
                 FRIDGE_TOUCH_I2C_ADDR, esp_err_to_name(ret));
        return ret;
    }
    (void)touch_read_reg(FT6336U_REG_VENDOR_ID, &vendor, 1);

    s_initialized = true;
    ESP_LOGI(TAG, "FT6336U ready addr=0x%02X chip=0x%02X vendor=0x%02X INT=GPIO%d TP_RST=not connected",
             FRIDGE_TOUCH_I2C_ADDR,
             id,
             vendor,
             FRIDGE_TOUCH_INT_GPIO);
    return ESP_OK;
}

esp_err_t fridge_touch_read(fridge_touch_point_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t data[5] = {0};
    esp_err_t ret = touch_read_reg(FT6336U_REG_TD_STATUS, data, sizeof(data));
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t points = data[0] & 0x0F;
    if (points == 0 || points > 2) {
        out->pressed = false;
        out->points = 0;
        return ESP_OK;
    }

    uint16_t x = (uint16_t)(((data[1] & 0x0F) << 8) | data[2]);
    uint16_t y = (uint16_t)(((data[3] & 0x0F) << 8) | data[4]);
    if (x >= 720 || y >= 720) {
        ESP_LOGD(TAG, "ignore out-of-range touch x=%u y=%u points=%u", x, y, points);
        return ESP_OK;
    }

    out->pressed = true;
    out->points = points;
    out->x = x;
    out->y = y;
    return ESP_OK;
}
