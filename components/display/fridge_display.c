// 冰箱小精灵 TR230S QSPI 屏幕驱动。
// 本文件从已验证的 display_test 命令路径拆出可复用接口：GPIO7 复位、GPIO6 WAIT#、QSPI 4SDA 写显存。
// 硬件注意：屏幕 VCC 为 5V，但所有 QSPI/RESET/WAIT 信号必须是 3.3V 逻辑，且所有模块必须共地。

#include "fridge_display.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#define LCD_BYTES_PER_PIXEL 2
#define LCD_FLUSH_CHUNK_ROWS 20
#define LCD_FLUSH_CHUNK_BYTES (FRIDGE_DISPLAY_WIDTH * LCD_FLUSH_CHUNK_ROWS * LCD_BYTES_PER_PIXEL)
#define LCD_PIN_CS GPIO_NUM_10
#define LCD_PIN_D0 GPIO_NUM_11
#define LCD_PIN_SCLK GPIO_NUM_12
#define LCD_PIN_D1 GPIO_NUM_13
#define LCD_PIN_D2 GPIO_NUM_14
#define LCD_PIN_D3 GPIO_NUM_9
#define LCD_PIN_RST GPIO_NUM_7
#define LCD_PIN_WAIT GPIO_NUM_6
#define LCD_SPI_HOST SPI2_HOST
#define LCD_PCLK_HZ 40000000
#define LCD_WAIT_READY_TIMEOUT_MS 500
#define LCD_WAIT_POLL_INTERVAL_MS 10
#define LCD_BRIGHTNESS_DEFAULT 80
#define LCD_NVS_NAMESPACE "fridge_ui"
#define LCD_NVS_KEY_BRIGHTNESS "brightness"

static const char *TAG = "fridge_display";

static spi_device_handle_t s_lcd;
static uint8_t *s_tx_buf;
static bool s_ready;
static uint8_t s_brightness = LCD_BRIGHTNESS_DEFAULT;
static int64_t s_last_brightness_write_ms;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t wramcmd;
    uint16_t setxcmd;
    uint16_t setycmd;
    gpio_num_t cs;
} qspilcd_dev_t;

static qspilcd_dev_t s_lcd_dev = {
    .width = FRIDGE_DISPLAY_WIDTH,
    .height = FRIDGE_DISPLAY_HEIGHT,
    .wramcmd = 0x2C,
    .setxcmd = 0x2A,
    .setycmd = 0x2B,
    .cs = LCD_PIN_CS,
};

typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t databytes;
} lcd_init_cmd_t;

// 等待 TR230S 释放 WAIT#。低电平时继续写命令可能造成花屏或总线卡死。
static esp_err_t lcd_wait_ready(const char *stage)
{
    for (int elapsed = 0; elapsed <= LCD_WAIT_READY_TIMEOUT_MS; elapsed += LCD_WAIT_POLL_INTERVAL_MS) {
        if (gpio_get_level(LCD_PIN_WAIT) == 1) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(LCD_WAIT_POLL_INTERVAL_MS));
    }
    ESP_LOGE(TAG, "WAIT# stayed low while %s; check LCD power, reset and FPC direction", stage);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t qspi_write_cmd_bytes(const uint8_t *data, int len, bool keep_cs)
{
    if (len == 0) {
        return ESP_OK;
    }

    spi_transaction_t t = {0};
    t.length = len * 8;
    t.tx_buffer = data;
    if (keep_cs) {
        t.flags = SPI_TRANS_CS_KEEP_ACTIVE;
    }
    esp_err_t ret = spi_device_polling_transmit(s_lcd, &t);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "QSPI command transmit failed len=%d keep_cs=%d ret=%s",
                 len, keep_cs, esp_err_to_name(ret));
    }
    return ret;
}

// 像素数据阶段使用 4 线 QIO；最后用 0 长度、不保持 CS 的事务收尾。
static esp_err_t qspi_write_data(const uint8_t *data, int len, bool keep_cs)
{
    spi_transaction_t t = {0};
    t.flags = SPI_TRANS_MODE_QIO;
    if (keep_cs) {
        t.flags |= SPI_TRANS_CS_KEEP_ACTIVE;
    }
    t.length = len * 8;
    t.tx_buffer = data;
    esp_err_t ret = spi_device_polling_transmit(s_lcd, &t);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "QSPI pixel transmit failed len=%d keep_cs=%d ret=%s",
                 len, keep_cs, esp_err_to_name(ret));
    }
    return ret;
}

// TR230S 命令封包：0x02 写寄存器，0x12 写显存，沿用已验证 display_test 路径。
static esp_err_t lcd_write_cmd(uint8_t cmd, const uint8_t *data, uint8_t datalen)
{
    esp_err_t ready = lcd_wait_ready("writing TR230S command");
    if (ready != ESP_OK) {
        return ready;
    }

    uint8_t packet[100] = {0x02, 0x00, 0x00, 0x00};
    uint8_t pos = 4;
    packet[2] = cmd;

    if (cmd == 0x2C) {
        packet[0] = 0x12;
        packet[1] = 0x00;
        packet[2] = 0x2C;
        packet[3] = 0x00;
        return qspi_write_cmd_bytes(packet, pos, true);
    }

    for (uint8_t i = 0; i < datalen; i++) {
        packet[pos++] = data[i];
    }
    return qspi_write_cmd_bytes(packet, pos, false);
}

static void lcd_hard_reset(void)
{
    gpio_config_t rst_conf = {
        .pin_bit_mask = 1ULL << LCD_PIN_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&rst_conf));

    gpio_set_level(LCD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(LCD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
}

static void configure_safe_start_levels(void)
{
    gpio_config_t cs_conf = {
        .pin_bit_mask = 1ULL << LCD_PIN_CS,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cs_conf));
    gpio_set_level(LCD_PIN_CS, 1);
}

static esp_err_t qspi_bus_init(void)
{
    spi_bus_config_t bus_conf = {0};
    bus_conf.data0_io_num = LCD_PIN_D0;
    bus_conf.data1_io_num = LCD_PIN_D1;
    bus_conf.data2_io_num = LCD_PIN_D2;
    bus_conf.data3_io_num = LCD_PIN_D3;
    bus_conf.sclk_io_num = LCD_PIN_SCLK;
    bus_conf.data4_io_num = GPIO_NUM_NC;
    bus_conf.data5_io_num = GPIO_NUM_NC;
    bus_conf.data6_io_num = GPIO_NUM_NC;
    bus_conf.data7_io_num = GPIO_NUM_NC;
    bus_conf.max_transfer_sz = LCD_FLUSH_CHUNK_BYTES;
    bus_conf.flags = SPICOMMON_BUSFLAG_QUAD | SPICOMMON_BUSFLAG_GPIO_PINS;

    esp_err_t ret = spi_bus_initialize(LCD_SPI_HOST, &bus_conf, SPI_DMA_CH_AUTO);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SPI bus already initialized, reusing host=%d", LCD_SPI_HOST);
        return ESP_OK;
    }
    return ret;
}

static esp_err_t lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t data[4] = {0};
    data[0] = x0 >> 8;
    data[1] = x0 & 0xFF;
    data[2] = x1 >> 8;
    data[3] = x1 & 0xFF;
    esp_err_t ret = lcd_write_cmd(s_lcd_dev.setxcmd, data, 4);
    if (ret != ESP_OK) {
        return ret;
    }

    data[0] = y0 >> 8;
    data[1] = y0 & 0xFF;
    data[2] = y1 >> 8;
    data[3] = y1 & 0xFF;
    ret = lcd_write_cmd(s_lcd_dev.setycmd, data, 4);
    if (ret != ESP_OK) {
        return ret;
    }

    return lcd_write_cmd(s_lcd_dev.wramcmd, NULL, 0);
}

static esp_err_t lcd_init_commands(void)
{
    lcd_init_cmd_t init_cmds[] = {
        {0x20, {0x00}, 0x81},
        {0x21, {0x64}, 0x81},
        {0x29, {0x00}, 0x80},
        {0x20, {LCD_BRIGHTNESS_DEFAULT}, 0x81},
        {0, {0}, 0xFF},
    };

    for (int i = 0; init_cmds[i].databytes != 0xFF; i++) {
        esp_err_t ret = lcd_write_cmd(init_cmds[i].cmd,
                                      init_cmds[i].data,
                                      init_cmds[i].databytes & 0x1F);
        if (ret != ESP_OK) {
            return ret;
        }
        if (init_cmds[i].databytes & 0x80) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    return ESP_OK;
}

static uint8_t load_saved_brightness(void)
{
    nvs_handle_t handle;
    uint8_t value = LCD_BRIGHTNESS_DEFAULT;
    if (nvs_open(LCD_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        (void)nvs_get_u8(handle, LCD_NVS_KEY_BRIGHTNESS, &value);
        nvs_close(handle);
    }
    if (value > 100) {
        value = 100;
    }
    return value;
}

static esp_err_t save_brightness(uint8_t value)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(LCD_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_u8(handle, LCD_NVS_KEY_BRIGHTNESS, value);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

esp_err_t fridge_display_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "init pins: CS=10 D0=11 SCLK=12 D1=13 D2=14 D3=9 RST=7 WAIT#=6");
    ESP_LOGW(TAG, "LCD RESET uses GPIO7; GPIO8 is reserved for OV3660 D2 in the current wiring plan");

    gpio_config_t wait_conf = {
        .pin_bit_mask = 1ULL << LCD_PIN_WAIT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&wait_conf));

    configure_safe_start_levels();
    lcd_hard_reset();
    ESP_RETURN_ON_ERROR(lcd_wait_ready("after hardware reset"), TAG, "LCD WAIT# not ready");
    ESP_RETURN_ON_ERROR(qspi_bus_init(), TAG, "QSPI bus init failed");

    spi_device_interface_config_t dev_conf = {
        .clock_speed_hz = LCD_PCLK_HZ,
        .mode = 0,
        .spics_io_num = s_lcd_dev.cs,
        .queue_size = 8,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(LCD_SPI_HOST, &dev_conf, &s_lcd), TAG, "add LCD SPI device failed");
    ESP_RETURN_ON_ERROR(lcd_init_commands(), TAG, "LCD init commands failed");

    s_tx_buf = heap_caps_malloc(LCD_FLUSH_CHUNK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!s_tx_buf) {
        s_tx_buf = heap_caps_malloc(LCD_FLUSH_CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    }
    if (!s_tx_buf) {
        return ESP_ERR_NO_MEM;
    }

    s_ready = true;
    s_brightness = load_saved_brightness();
    ESP_RETURN_ON_ERROR(fridge_display_set_brightness(s_brightness), TAG, "restore brightness failed");
    ESP_LOGI(TAG, "TR230S display ready, pclk=%uHz chunk_rows=%u brightness=%u%%",
             (unsigned)LCD_PCLK_HZ,
             (unsigned)LCD_FLUSH_CHUNK_ROWS,
             (unsigned)s_brightness);
    return ESP_OK;
}

esp_err_t fridge_display_flush_area(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, const uint16_t *rgb565_pixels)
{
    if (!s_ready || !rgb565_pixels) {
        return ESP_ERR_INVALID_STATE;
    }
    if (x2 >= FRIDGE_DISPLAY_WIDTH || y2 >= FRIDGE_DISPLAY_HEIGHT || x1 > x2 || y1 > y2) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint16_t width = x2 - x1 + 1;
    const uint16_t height = y2 - y1 + 1;
    esp_err_t ret = spi_device_acquire_bus(s_lcd, portMAX_DELAY);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = lcd_set_window(x1, y1, x2, y2);
    if (ret == ESP_OK) {
        uint16_t row = 0;
        while (row < height) {
            uint16_t rows = height - row;
            if (rows > LCD_FLUSH_CHUNK_ROWS) {
                rows = LCD_FLUSH_CHUNK_ROWS;
            }
            size_t pixel_count = (size_t)width * rows;
            for (size_t i = 0; i < pixel_count; i++) {
                uint16_t color = rgb565_pixels[(size_t)row * width + i];
                s_tx_buf[i * 2] = (uint8_t)(color >> 8);
                s_tx_buf[i * 2 + 1] = (uint8_t)(color & 0xFF);
            }
            ret = qspi_write_data(s_tx_buf, (int)(pixel_count * LCD_BYTES_PER_PIXEL), true);
            if (ret != ESP_OK) {
                break;
            }
            row += rows;
        }
        if (ret == ESP_OK) {
            ret = qspi_write_data(NULL, 0, false);
        }
    }

    spi_device_release_bus(s_lcd);
    return ret;
}

esp_err_t fridge_display_set_brightness(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    if (!s_ready) {
        s_brightness = percent;
        return ESP_OK;
    }

    int64_t now_ms = (int64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (now_ms - s_last_brightness_write_ms < 50 && percent != 0 && percent != 100) {
        s_brightness = percent;
        return ESP_OK;
    }

    uint8_t value = percent;
    esp_err_t ret = lcd_write_cmd(0x20, &value, 1);
    if (ret != ESP_OK) {
        return ret;
    }
    s_last_brightness_write_ms = now_ms;
    s_brightness = percent;
    esp_err_t save_ret = save_brightness(percent);
    if (save_ret != ESP_OK) {
        ESP_LOGW(TAG, "save brightness failed: %s", esp_err_to_name(save_ret));
    }
    return ESP_OK;
}

uint8_t fridge_display_get_brightness(void)
{
    return s_brightness;
}

bool fridge_display_is_ready(void)
{
    return s_ready;
}
