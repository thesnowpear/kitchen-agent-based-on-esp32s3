// 冰箱小精灵屏幕最小移植测试。
// 本文件按 example/screen 里的 QSPI LCD 例程改造，只替换为当前 ESP32-S3
// 实物接线：屏幕 VCC=5V、GPIO 逻辑=3.3V、共地；不假设存在独立背光 GPIO。
#include "fridge_display_test.h"

#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LCD_WIDTH 720
#define LCD_HEIGHT 720
#define LCD_BYTES_PER_PIXEL 2
#define LCD_TOTAL_BUF_SIZE (LCD_WIDTH * LCD_HEIGHT * LCD_BYTES_PER_PIXEL)
#define LCD_BLOCK_COUNT 50
#define LCD_BLOCK_SIZE (LCD_TOTAL_BUF_SIZE / LCD_BLOCK_COUNT)
#define LCD_TEST_STEP_DELAY_MS 20

#ifndef CONFIG_FRIDGE_SCREEN_TEST_PCLK_HZ
// 屏幕测试组件在正常主控模式下仍会参与编译；此默认值只保证编译通过，不会启动屏幕测试任务。
#define CONFIG_FRIDGE_SCREEN_TEST_PCLK_HZ 10000000
#endif

#ifndef CONFIG_FRIDGE_SCREEN_TEST_COLOR_HOLD_MS
// 正常 sdkconfig 会提供该值；这里保留编译兜底，避免非屏幕测试配置下缺少宏。
#define CONFIG_FRIDGE_SCREEN_TEST_COLOR_HOLD_MS 3000
#endif

#ifndef CONFIG_FRIDGE_SCREEN_TEST_REPORT_EVERY_CYCLES
// 长时间稳定性测试默认低频打印，减少 idf.py monitor 或 Web Serial 端的日志压力。
#define CONFIG_FRIDGE_SCREEN_TEST_REPORT_EVERY_CYCLES 10
#endif

#ifdef CONFIG_FRIDGE_SCREEN_TEST_VERBOSE_LOG
#define LCD_TEST_VERBOSE_LOG_ENABLED 1
#else
#define LCD_TEST_VERBOSE_LOG_ENABLED 0
#endif

#define LCD_PIN_CS GPIO_NUM_10
#define LCD_PIN_D0 GPIO_NUM_11
#define LCD_PIN_SCLK GPIO_NUM_12
#define LCD_PIN_D1 GPIO_NUM_13
#define LCD_PIN_D2 GPIO_NUM_14
#define LCD_PIN_D3 GPIO_NUM_9
#define LCD_PIN_RST GPIO_NUM_7
#define LCD_PIN_WAIT GPIO_NUM_6
#define LCD_SPI_HOST SPI2_HOST
#define LCD_WAIT_READY_TIMEOUT_MS 500
#define LCD_WAIT_POLL_INTERVAL_MS 10

#define COLOR_BLACK 0x0000
#define COLOR_RED 0xF800
#define COLOR_GREEN 0x07E0
#define COLOR_BLUE 0x001F
#define COLOR_WHITE 0xFFFF

static const char *TAG = "display_test";
static spi_device_handle_t s_lcd;
static uint8_t *s_lcd_buf;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t dir;
    uint16_t wramcmd;
    uint16_t setxcmd;
    uint16_t setycmd;
    gpio_num_t cs;
} qspilcd_dev_t;

static qspilcd_dev_t s_lcd_dev = {
    .width = LCD_WIDTH,
    .height = LCD_HEIGHT,
    .dir = 1,
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

// 等待 TR230S 允许发送命令。
// 注意：WAIT# 为屏幕侧 3.3V 逻辑输出，低电平表示不能继续写命令，首次接线异常时应停下复核硬件。
static esp_err_t example_lcd_wait_ready(const char *stage)
{
    for (int elapsed = 0; elapsed <= LCD_WAIT_READY_TIMEOUT_MS; elapsed += LCD_WAIT_POLL_INTERVAL_MS) {
        if (gpio_get_level(LCD_PIN_WAIT) == 1) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(LCD_WAIT_POLL_INTERVAL_MS));
    }

    ESP_LOGE(TAG, "WAIT# stayed low while %s; stop QSPI writes and check reset/power/FPC direction", stage);
    return ESP_ERR_TIMEOUT;
}

// 例程里的 QSPI 命令发送函数：普通命令阶段仍用单线发送，必要时保持 CS。
static esp_err_t example_qspi_write_cmd_bytes(const uint8_t *data, int len, bool keep_cs)
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

// 例程里的 QSPI 数据发送函数：像素数据阶段使用 4 线 QIO。
static esp_err_t example_qspi_write_data(const uint8_t *data, int len, bool keep_cs)
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

// 例程里的 TR230S 命令封包：0x02 写寄存器，0x12 写显存。
static esp_err_t example_lcd_write_cmd(uint8_t cmd, const uint8_t *data, uint8_t datalen)
{
    esp_err_t ready = example_lcd_wait_ready("writing TR230S command");
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
        return example_qspi_write_cmd_bytes(packet, pos, true);
    }

    for (uint8_t i = 0; i < datalen; i++) {
        packet[pos++] = data[i];
    }
    return example_qspi_write_cmd_bytes(packet, pos, false);
}

// 设置屏幕方向和基本寄存器号，保持 example 的 720x720 横屏路径。
static void example_lcd_display_dir(uint8_t dir)
{
    s_lcd_dev.dir = dir;
    s_lcd_dev.width = LCD_WIDTH;
    s_lcd_dev.height = LCD_HEIGHT;
    s_lcd_dev.wramcmd = 0x2C;
    s_lcd_dev.setxcmd = 0x2A;
    s_lcd_dev.setycmd = 0x2B;
}

// 当前主控没有例程板载 XL9555，RESET# 直接接 GPIO7，所以这里做最小硬复位适配。
static void example_lcd_hard_reset(void)
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

// 初始化 QSPI 总线。
// 注意：当前 20-pin 飞线使用的是 ESP32-S3 的 SPI2 常用 QSPI 引脚组；先强制走 GPIO matrix，
// 避免 IOMUX 自动路径和官方例程的 SPI3 命名差异干扰首次点亮排查。
static esp_err_t example_qspi_bus_init(void)
{
    ESP_LOGI(TAG, "step: spi bus init start host=%d block=%d total=%d", LCD_SPI_HOST, LCD_BLOCK_SIZE, LCD_TOTAL_BUF_SIZE);

    spi_bus_config_t bus_conf = {0};
    bus_conf.data0_io_num = LCD_PIN_D0;
    bus_conf.data1_io_num = LCD_PIN_D1;
    bus_conf.data2_io_num = LCD_PIN_D2;
    bus_conf.data3_io_num = LCD_PIN_D3;
    bus_conf.sclk_io_num = LCD_PIN_SCLK;
    // ESP-IDF 6.x 中 data2/quadwp、data3/quadhd 是同一组 union 字段，不能再单独写成 NC 覆盖掉 D2/D3。
    bus_conf.data4_io_num = GPIO_NUM_NC;
    bus_conf.data5_io_num = GPIO_NUM_NC;
    bus_conf.data6_io_num = GPIO_NUM_NC;
    bus_conf.data7_io_num = GPIO_NUM_NC;
    bus_conf.max_transfer_sz = LCD_BLOCK_SIZE;
    bus_conf.flags = SPICOMMON_BUSFLAG_QUAD | SPICOMMON_BUSFLAG_GPIO_PINS;

    esp_err_t ret = spi_bus_initialize(LCD_SPI_HOST, &bus_conf, SPI_DMA_CH_AUTO);
    ESP_LOGI(TAG, "step: spi bus init done ret=%s", esp_err_to_name(ret));
    return ret;
}

// 飞线调试时先让 CS 保持高电平，避免屏幕上电复位阶段误收命令。
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

// 初始化 LCD 设备和 TR230S 背光/显示寄存器。
// 注意：20-pin 模式没有独立 BL GPIO，背光由 TR230S 内部 PWM 控制；先关占空比，再设置频率和 Display On，
// 最后拉到 100%，避免复位后背光状态不确定。
static esp_err_t example_lcd_init(void)
{
    ESP_LOGI(TAG, "step: spi add lcd device start clock=%d", CONFIG_FRIDGE_SCREEN_TEST_PCLK_HZ);

    spi_device_interface_config_t dev_conf = {
        .clock_speed_hz = CONFIG_FRIDGE_SCREEN_TEST_PCLK_HZ,
        .mode = 0,
        .spics_io_num = s_lcd_dev.cs,
        .queue_size = 30,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };

    esp_err_t ret = spi_bus_add_device(LCD_SPI_HOST, &dev_conf, &s_lcd);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "step: spi add lcd device failed ret=%s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "step: spi add lcd device done");

    lcd_init_cmd_t init_cmds[] = {
        {0x20, {0x00}, 0x81},
        {0x21, {0x64}, 0x81},
        {0x29, {0x00}, 0x80},
        {0x20, {0x64}, 0x81},
        {0, {0}, 0xFF},
    };

    for (int i = 0; init_cmds[i].databytes != 0xFF; i++) {
        ESP_LOGI(TAG, "example init cmd=0x%02X data=0x%02X",
                 init_cmds[i].cmd, init_cmds[i].data[0]);
        ret = example_lcd_write_cmd(init_cmds[i].cmd,
                                    init_cmds[i].data,
                                    init_cmds[i].databytes & 0x1F);
        if (ret != ESP_OK) {
            return ret;
        }
        if (init_cmds[i].databytes & 0x80) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    example_lcd_display_dir(1);
    ESP_LOGI(TAG, "step: lcd init commands done");
    return ESP_OK;
}

// 设置整屏窗口，命令格式和 example 的 lcd_set_window() 保持一致。
static esp_err_t example_lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t data[4] = {0};

    data[0] = x0 >> 8;
    data[1] = x0 & 0xFF;
    data[2] = x1 >> 8;
    data[3] = x1 & 0xFF;
    esp_err_t ret = example_lcd_write_cmd(s_lcd_dev.setxcmd, data, 4);
    if (ret != ESP_OK) {
        return ret;
    }

    data[0] = y0 >> 8;
    data[1] = y0 & 0xFF;
    data[2] = y1 >> 8;
    data[3] = y1 & 0xFF;
    ret = example_lcd_write_cmd(s_lcd_dev.setycmd, data, 4);
    if (ret != ESP_OK) {
        return ret;
    }

    return example_lcd_write_cmd(s_lcd_dev.wramcmd, NULL, 0);
}

// 按 example 的 qspilcd_clear()：全屏缓冲填色，再分 50 块通过 QIO 写入。
static esp_err_t example_qspilcd_clear(uint16_t color)
{
    uint8_t high = color >> 8;
    uint8_t low = color & 0xFF;

    if (LCD_TEST_VERBOSE_LOG_ENABLED) {
        ESP_LOGI(TAG, "step: acquire spi bus color=0x%04X", color);
    }
    esp_err_t ret = spi_device_acquire_bus(s_lcd, portMAX_DELAY);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = example_lcd_set_window(0, 0, s_lcd_dev.width - 1, s_lcd_dev.height - 1);
    if (ret == ESP_OK) {
        for (uint32_t i = 0; i < s_lcd_dev.width * s_lcd_dev.height; i++) {
            s_lcd_buf[i * 2] = high;
            s_lcd_buf[i * 2 + 1] = low;
            if (i % 50000 == 0) {
                vTaskDelay(1);
            }
        }

        for (int i = 0; i < LCD_BLOCK_COUNT; i++) {
            if (LCD_TEST_VERBOSE_LOG_ENABLED && (i == 0 || i == LCD_BLOCK_COUNT - 1)) {
                ESP_LOGI(TAG, "step: write qspi block %d/%d size=%d", i + 1, LCD_BLOCK_COUNT, LCD_BLOCK_SIZE);
            }
            ret = example_qspi_write_data(s_lcd_buf + i * LCD_BLOCK_SIZE, LCD_BLOCK_SIZE, true);
            if (ret != ESP_OK) {
                break;
            }
            vTaskDelay(1);
        }
        if (ret == ESP_OK) {
            ret = example_qspi_write_data(NULL, 0, false);
        }
    }

    spi_device_release_bus(s_lcd);
    if (LCD_TEST_VERBOSE_LOG_ENABLED) {
        ESP_LOGI(TAG, "step: release spi bus color=0x%04X ret=%s", color, esp_err_to_name(ret));
    }
    return ret;
}

// 失败后停在日志现场，不继续向真实硬件反复发数据。
static void stop_forever(const char *reason, esp_err_t err)
{
    ESP_LOGE(TAG, "%s: %s", reason, esp_err_to_name(err));
    ESP_LOGE(TAG, "QSPI stopped. Power off before rewiring; keep LCD VCC=5V and GPIO logic=3.3V.");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void fridge_display_test_run(void)
{
    ESP_LOGW(TAG, "Example-based TR230S QSPI test is enabled.");
    ESP_LOGW(TAG, "Pins: CS=10 D0=11 SCLK=12 D1=13 D2=14 D3=9 RST=7 WAIT#=6; D/C# and QSPI-INT are not used.");
    ESP_LOGW(TAG, "Before power-on test: LCD VCC=5V, GPIO logic=3.3V, common GND, no 5V-to-GPIO short.");

    gpio_config_t wait_conf = {
        .pin_bit_mask = 1ULL << LCD_PIN_WAIT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&wait_conf));

    configure_safe_start_levels();
    ESP_LOGI(TAG, "step: hard reset start");
    example_lcd_hard_reset();
    ESP_LOGI(TAG, "after reset WAIT#=%d", gpio_get_level(LCD_PIN_WAIT));
    esp_err_t ret = example_lcd_wait_ready("after hardware reset");
    if (ret != ESP_OK) {
        stop_forever("LCD WAIT# stayed low after reset", ret);
    }
    vTaskDelay(pdMS_TO_TICKS(LCD_TEST_STEP_DELAY_MS));

    ret = example_qspi_bus_init();
    if (ret != ESP_OK) {
        stop_forever("example qspi bus init failed", ret);
    }
    vTaskDelay(pdMS_TO_TICKS(LCD_TEST_STEP_DELAY_MS));

    ret = example_lcd_init();
    if (ret != ESP_OK) {
        stop_forever("example lcd init failed", ret);
    }
    vTaskDelay(pdMS_TO_TICKS(LCD_TEST_STEP_DELAY_MS));

    ESP_LOGI(TAG, "step: allocate PSRAM framebuffer start");
    s_lcd_buf = (uint8_t *)heap_caps_malloc(LCD_TOTAL_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_lcd_buf) {
        stop_forever("example lcd full-frame PSRAM buffer allocation failed", ESP_ERR_NO_MEM);
    }
    ESP_LOGI(TAG, "example lcd buffer=%p size=%d clock=%d Hz", s_lcd_buf, LCD_TOTAL_BUF_SIZE, CONFIG_FRIDGE_SCREEN_TEST_PCLK_HZ);
    ESP_LOGI(TAG, "long-run mode: color_hold=%d ms report_every=%d cycles verbose_log=%s",
             CONFIG_FRIDGE_SCREEN_TEST_COLOR_HOLD_MS,
             CONFIG_FRIDGE_SCREEN_TEST_REPORT_EVERY_CYCLES,
             LCD_TEST_VERBOSE_LOG_ENABLED ? "yes" : "no");

    const uint16_t colors[] = {COLOR_WHITE, COLOR_RED, COLOR_GREEN, COLOR_BLUE, COLOR_BLACK};
    const char *names[] = {"white", "red", "green", "blue", "black"};
    uint32_t cycle_count = 0;
    while (true) {
        for (int i = 0; i < (int)(sizeof(colors) / sizeof(colors[0])); i++) {
            if (LCD_TEST_VERBOSE_LOG_ENABLED) {
                ESP_LOGI(TAG, "example clear: %s", names[i]);
            }
            ret = example_qspilcd_clear(colors[i]);
            if (ret != ESP_OK) {
                stop_forever("example qspilcd clear failed", ret);
            }
            vTaskDelay(pdMS_TO_TICKS(CONFIG_FRIDGE_SCREEN_TEST_COLOR_HOLD_MS));
        }

        cycle_count++;
        if ((cycle_count % CONFIG_FRIDGE_SCREEN_TEST_REPORT_EVERY_CYCLES) == 0) {
            ESP_LOGI(TAG, "screen test alive: cycles=%lu clock=%d Hz hold=%d ms",
                     (unsigned long)cycle_count,
                     CONFIG_FRIDGE_SCREEN_TEST_PCLK_HZ,
                     CONFIG_FRIDGE_SCREEN_TEST_COLOR_HOLD_MS);
        }
    }
}
