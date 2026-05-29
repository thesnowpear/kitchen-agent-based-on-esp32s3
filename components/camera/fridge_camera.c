// OV3660 摄像头测试组件。
// 当前普通拍照优先使用 YUV422 抓帧后软件压 JPEG，避免 OV3660 片上 JPEG 路径 NO-SOI 导致预览不可用。
// 硬件注意：OV3660 的 VDD/DVDD1.5V 接 1.5V 稳压；VDD2.8V/IOVDD/AVDD 按当前模组参数接 3.3V。

#include "fridge_camera.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_psram.h"
#include "img_converters.h"

#ifndef CONFIG_FRIDGE_CAMERA_XCLK_HZ
#define CONFIG_FRIDGE_CAMERA_XCLK_HZ 10000000
#endif

#define CAM_RAW_WARMUP_FRAME_COUNT 6
#define CAM_JPEG_WARMUP_FRAME_COUNT 8
#define CAM_WARMUP_DELAY_MS 180
#define CAM_AE_LEVEL 5
#define CAM_BRIGHTNESS_LEVEL 2
#define CAM_LOW_LIGHT_TARGET_LUMA 92
#define CAM_LOW_LIGHT_MAX_GAIN_X100 240
#define CAM_LOW_LIGHT_CHROMA_X100 78
#define CAM_AI_FULLRES_JPEG_QUALITY 82

typedef struct {
    framesize_t frame_size;
    const char *label;
    const char *mode_name;
    size_t min_free_psram;
} cam_ai_capture_option_t;

static const cam_ai_capture_option_t CAM_AI_CAPTURE_OPTIONS[] = {
    // 成品路径使用当前实测最高稳定档 XGA。QXGA 会 SCCB 寄存器写失败，UXGA/SXGA 首帧超时，
    // 不应让用户每次 AI 识别都等待失败重试；更高分辨率后续作为专项诊断链路继续攻关。
    {FRAMESIZE_XGA, "XGA", "ai xga software jpeg", 3U * 1024U * 1024U},
};

static const char *TAG = "fridge_camera";

// OV3660 DVP 引脚。
// 注意：N8R8 板必须避开 GPIO35/36/37；GPIO4/5 与触摸、MPU6050 共用 I2C/SCCB。
// RESET/PWDN 先不占 GPIO，触摸 TP_RST 和雷达 OT2 已在排线方案中让出。
// esp32-camera 的 pin_d0..pin_d7 对应 OV3660 8-bit DVP 有效窗口 D2..D9；
// 摄像头物理 D0/D1 当前悬空，用于避免把 10-bit 输出的低两位误接进 8-bit 采样。
enum {
    CAM_PIN_SIOD = 4,
    CAM_PIN_SIOC = 5,
    CAM_PIN_XCLK = 47,
    CAM_PIN_PWDN = -1,
    CAM_PIN_RESET = -1,
    CAM_PIN_VSYNC = 2,
    CAM_PIN_HREF = 38,
    CAM_PIN_PCLK = 19,
    CAM_PIN_D0 = 8,   // pin_d0 <- OV3660 D2
    CAM_PIN_D1 = 3,   // pin_d1 <- OV3660 D3
    CAM_PIN_D2 = 46,  // pin_d2 <- OV3660 D4
    CAM_PIN_D3 = 48,  // pin_d3 <- OV3660 D5
    CAM_PIN_D4 = 45,  // pin_d4 <- OV3660 D6
    CAM_PIN_D5 = 16,  // pin_d5 <- OV3660 D7
    CAM_PIN_D6 = 17,  // pin_d6 <- OV3660 D8
    CAM_PIN_D7 = 18,  // pin_d7 <- OV3660 D9
};

#define CAM_PROBE_I2C_PORT I2C_NUM_1
#define CAM_PROBE_I2C_HZ 100000
#define CAM_PROBE_TIMEOUT_MS 80
#define CAM_PROBE_SCCB_ADDR 0x3C
#define CAM_PROBE_PID_HIGH_REG 0x300A
#define CAM_PROBE_PID_LOW_REG 0x300B
#define CAM_PROBE_EXPECTED_PID 0x3660

static bool s_initialized;
static bool s_rgb565_diag_mode;
static bool s_software_jpeg_mode;
static bool s_hardware_jpeg_diag_mode;
static pixformat_t s_preview_source_format;
static uint8_t *s_jpeg;
static size_t s_jpeg_len;
static int s_width;
static int s_height;
static uint32_t s_capture_ms;
static uint32_t s_frame_id;
static char s_last_error[FRIDGE_CAMERA_MAX_ERROR_LEN + 1];

static void set_last_error(const char *message)
{
    strlcpy(s_last_error, message ? message : "", sizeof(s_last_error));
}

static void probe_set_error(fridge_camera_probe_result_t *out, const char *message)
{
    if (out) {
        strlcpy(out->last_error, message ? message : "", sizeof(out->last_error));
    }
}

static esp_err_t probe_xclk_start(void)
{
    // 安全探测只需要给 OV3660 提供 XCLK，不启用 DVP DMA。
    // 注意：GPIO47 只输出 10MHz 低占空比时钟，探测结束后会关闭，避免长时间扰动未完整接线的模组。
    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_1_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = CONFIG_FRIDGE_CAMERA_XCLK_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
        .deconfigure = false,
#endif
    };
    esp_err_t err = ledc_timer_config(&timer_conf);
    if (err != ESP_OK) {
        return err;
    }

    ledc_channel_config_t channel_conf = {
        .gpio_num = CAM_PIN_XCLK,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
        .intr_type = LEDC_INTR_DISABLE,
#endif
        .timer_sel = LEDC_TIMER_0,
        .duty = 1,
        .hpoint = 0,
    };
    return ledc_channel_config(&channel_conf);
}

static void probe_xclk_stop(void)
{
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
}

static esp_err_t probe_i2c_start(void)
{
    // SCCB 兼容 I2C 读写。这里单独使用 I2C1，探测完成立即卸载，避免与完整 esp32-camera 初始化状态混在一起。
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = CAM_PIN_SIOD,
        .scl_io_num = CAM_PIN_SIOC,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = CAM_PROBE_I2C_HZ,
        .clk_flags = 0,
    };
    esp_err_t err = i2c_param_config(CAM_PROBE_I2C_PORT, &conf);
    if (err != ESP_OK) {
        return err;
    }
    return i2c_driver_install(CAM_PROBE_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
}

static void probe_i2c_stop(void)
{
    i2c_driver_delete(CAM_PROBE_I2C_PORT);
}

static esp_err_t probe_read_reg16(uint8_t addr, uint16_t reg, uint8_t *value)
{
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t reg_buf[2] = {
        (uint8_t)(reg >> 8),
        (uint8_t)(reg & 0xff),
    };
    return i2c_master_write_read_device(
        CAM_PROBE_I2C_PORT,
        addr,
        reg_buf,
        sizeof(reg_buf),
        value,
        1,
        pdMS_TO_TICKS(CAM_PROBE_TIMEOUT_MS));
}

static void release_frame(void)
{
    free(s_jpeg);
    s_jpeg = NULL;
    s_jpeg_len = 0;
    s_width = 0;
    s_height = 0;
    s_capture_ms = 0;
}

static uint32_t checksum32(const uint8_t *data, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum = (sum << 5) - sum + data[i];
    }
    return sum;
}

static uint8_t clamp_u8_int(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (uint8_t)value;
}

static uint32_t calc_yuv422_luma_avg(const uint8_t *data, size_t len)
{
    if (!data || len < 4) {
        return 0;
    }

    uint32_t y_sum = 0;
    size_t y_samples = 0;
    for (size_t i = 0; i + 3 < len; i += 4) {
        y_sum += data[i + 0] + data[i + 2];
        y_samples += 2;
    }
    return y_samples ? (uint32_t)(y_sum / y_samples) : 0;
}

static const char *frame_size_label_from_dimensions(int width, int height)
{
    if (width == 2048 && height == 1536) {
        return "QXGA";
    }
    if (width == 1600 && height == 1200) {
        return "UXGA";
    }
    if (width == 1280 && height == 1024) {
        return "SXGA";
    }
    if (width == 1024 && height == 768) {
        return "XGA";
    }
    if (width == 320 && height == 240) {
        return "QVGA";
    }
    if (width == 160 && height == 120) {
        return "QQVGA";
    }
    return "custom";
}

static void log_frame_luma_stats(const char *mode_name, const camera_fb_t *fb, int frame_index, int frame_count)
{
    if (!fb || !fb->buf || fb->len == 0) {
        return;
    }

    uint32_t y_sum = 0;
    uint8_t y_min = 0xff;
    uint8_t y_max = 0x00;
    size_t y_samples = 0;
    int32_t u_sum = 0;
    int32_t v_sum = 0;
    size_t uv_samples = 0;

    if (fb->format == PIXFORMAT_YUV422) {
        // OV3660 当前输出 YUYV；只抽样统计亮度和色度中心，避免在预热阶段增加太多 CPU 时间。
        size_t step = 8;
        for (size_t i = 0; i + 3 < fb->len; i += step) {
            uint8_t y0 = fb->buf[i + 0];
            uint8_t u = fb->buf[i + 1];
            uint8_t y1 = fb->buf[i + 2];
            uint8_t v = fb->buf[i + 3];
            y_sum += y0 + y1;
            y_min = y0 < y_min ? y0 : y_min;
            y_min = y1 < y_min ? y1 : y_min;
            y_max = y0 > y_max ? y0 : y_max;
            y_max = y1 > y_max ? y1 : y_max;
            y_samples += 2;
            u_sum += (int32_t)u - 128;
            v_sum += (int32_t)v - 128;
            uv_samples++;
        }
    } else if (fb->format == PIXFORMAT_RGB565) {
        size_t step = 8;
        for (size_t i = 0; i + 1 < fb->len; i += step) {
            uint16_t px = ((uint16_t)fb->buf[i] << 8) | fb->buf[i + 1];
            uint8_t r = (uint8_t)(((px >> 11) & 0x1f) * 255 / 31);
            uint8_t g = (uint8_t)(((px >> 5) & 0x3f) * 255 / 63);
            uint8_t b = (uint8_t)((px & 0x1f) * 255 / 31);
            uint8_t y = (uint8_t)(((uint32_t)77 * r + (uint32_t)150 * g + (uint32_t)29 * b) >> 8);
            y_sum += y;
            y_min = y < y_min ? y : y_min;
            y_max = y > y_max ? y : y_max;
            y_samples++;
        }
    }

    if (y_samples > 0) {
        ESP_LOGI(TAG,
                 "%s warmup drop frame %d/%d: %dx%d, fmt=%d, len=%u, luma avg=%lu min=%u max=%u, uv_bias=%ld/%ld",
                 mode_name,
                 frame_index,
                 frame_count,
                 fb->width,
                 fb->height,
                 fb->format,
                 (unsigned)fb->len,
                 (unsigned long)(y_sum / y_samples),
                 (unsigned)y_min,
                 (unsigned)y_max,
                 (long)(uv_samples ? u_sum / (int32_t)uv_samples : 0),
                 (long)(uv_samples ? v_sum / (int32_t)uv_samples : 0));
    } else {
        ESP_LOGI(TAG,
                 "%s warmup drop frame %d/%d: %dx%d, fmt=%d, len=%u",
                 mode_name,
                 frame_index,
                 frame_count,
                 fb->width,
                 fb->height,
                 fb->format,
                 (unsigned)fb->len);
    }
}

static void log_sensor_exposure_stats(const char *mode_name)
{
    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor || !sensor->get_reg) {
        return;
    }

    int aec_value = sensor->get_reg(sensor, 0x3500, 0x0fffff);
    int agc_gain = sensor->get_reg(sensor, 0x350a, 0x03ff);
    int gain_ceiling = sensor->get_reg(sensor, 0x3a18, 0x03ff);
    int max_aec = sensor->get_reg(sensor, 0x380e, 0xffff);
    ESP_LOGI(TAG,
             "%s exposure stats: aec=%d/%d, agc_reg=0x%03x, gain_ceiling=0x%03x",
             mode_name,
             aec_value,
             max_aec,
             agc_gain,
             gain_ceiling);
}

static void adjust_yuv422_low_light_inplace(uint8_t *data, size_t len, int gain_x100)
{
    if (!data || len < 4 || gain_x100 <= 100) {
        return;
    }
    // QXGA 原始帧约 6MB，不能再复制一份；AI 全分辨率路径直接原地提升 Y 并收敛 UV。
    for (size_t i = 0; i + 3 < len; i += 4) {
        data[i + 0] = clamp_u8_int(((int)data[i + 0] * gain_x100) / 100);
        data[i + 1] = clamp_u8_int(128 + (((int)data[i + 1] - 128) * CAM_LOW_LIGHT_CHROMA_X100) / 100);
        data[i + 2] = clamp_u8_int(((int)data[i + 2] * gain_x100) / 100);
        data[i + 3] = clamp_u8_int(128 + (((int)data[i + 3] - 128) * CAM_LOW_LIGHT_CHROMA_X100) / 100);
    }
}

static uint8_t *prepare_low_light_yuv422(const camera_fb_t *fb, uint32_t *out_luma_avg, int *out_gain_x100, bool allow_inplace)
{
    if (out_luma_avg) {
        *out_luma_avg = 0;
    }
    if (out_gain_x100) {
        *out_gain_x100 = 100;
    }
    if (!fb || fb->format != PIXFORMAT_YUV422 || !fb->buf || fb->len < 4) {
        return NULL;
    }

    uint32_t avg = calc_yuv422_luma_avg(fb->buf, fb->len);
    if (out_luma_avg) {
        *out_luma_avg = avg;
    }
    if (avg == 0 || avg >= CAM_LOW_LIGHT_TARGET_LUMA) {
        return NULL;
    }

    int gain_x100 = (int)((CAM_LOW_LIGHT_TARGET_LUMA * 100U) / avg);
    if (gain_x100 > CAM_LOW_LIGHT_MAX_GAIN_X100) {
        gain_x100 = CAM_LOW_LIGHT_MAX_GAIN_X100;
    }
    if (out_gain_x100) {
        *out_gain_x100 = gain_x100;
    }

    if (allow_inplace) {
        adjust_yuv422_low_light_inplace(fb->buf, fb->len, gain_x100);
        ESP_LOGI(TAG,
                 "low-light YUV inplace adjust enabled: luma avg=%lu -> target=%d, gain=%d.%02dx, chroma=%d%%",
                 (unsigned long)avg,
                 CAM_LOW_LIGHT_TARGET_LUMA,
                 gain_x100 / 100,
                 gain_x100 % 100,
                 CAM_LOW_LIGHT_CHROMA_X100);
        return NULL;
    }

    uint8_t *adjusted = heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!adjusted) {
        adjusted = heap_caps_malloc(fb->len, MALLOC_CAP_8BIT);
    }
    if (!adjusted) {
        ESP_LOGW(TAG, "low-light YUV adjust allocation failed, using original frame");
        return NULL;
    }

    // 低光环境下只提升亮度 Y，并轻微收敛 UV 到 128，减少暗部绿/紫色噪声被增亮后放大。
    for (size_t i = 0; i + 3 < fb->len; i += 4) {
        adjusted[i + 0] = clamp_u8_int(((int)fb->buf[i + 0] * gain_x100) / 100);
        adjusted[i + 1] = clamp_u8_int(128 + (((int)fb->buf[i + 1] - 128) * CAM_LOW_LIGHT_CHROMA_X100) / 100);
        adjusted[i + 2] = clamp_u8_int(((int)fb->buf[i + 2] * gain_x100) / 100);
        adjusted[i + 3] = clamp_u8_int(128 + (((int)fb->buf[i + 3] - 128) * CAM_LOW_LIGHT_CHROMA_X100) / 100);
    }

    ESP_LOGI(TAG,
             "low-light YUV adjust enabled: luma avg=%lu -> target=%d, gain=%d.%02dx, chroma=%d%%",
             (unsigned long)avg,
             CAM_LOW_LIGHT_TARGET_LUMA,
             gain_x100 / 100,
             gain_x100 % 100,
             CAM_LOW_LIGHT_CHROMA_X100);
    return adjusted;
}

static esp_err_t convert_yuv_frame_to_jpeg(camera_fb_t *fb, uint8_t quality, const char *mode_name, bool allow_inplace_adjust)
{
    if (!fb || fb->format != PIXFORMAT_YUV422 || fb->len == 0) {
        set_last_error("camera frame is not YUV422");
        ESP_LOGE(TAG, "%s", s_last_error);
        return ESP_FAIL;
    }

    uint8_t *jpeg = NULL;
    size_t jpeg_len = 0;
    uint32_t luma_avg = 0;
    int low_light_gain_x100 = 100;
    uint8_t *adjusted_yuv = prepare_low_light_yuv422(fb, &luma_avg, &low_light_gain_x100, allow_inplace_adjust);
    const uint8_t *jpg_src = adjusted_yuv ? adjusted_yuv : fb->buf;

    // OV3660 参考驱动把 YUV422 配置为 YUYV；低光时先在 YUV 域做亮度兜底，再交给 esp32-camera 转 JPEG。
    bool ok = fmt2jpg((uint8_t *)jpg_src, fb->len, fb->width, fb->height, fb->format, quality, &jpeg, &jpeg_len);
    free(adjusted_yuv);
    if (!ok || !jpeg || jpeg_len == 0) {
        free(jpeg);
        set_last_error("YUV422 software JPEG conversion failed");
        ESP_LOGE(TAG, "%s", s_last_error);
        return ESP_FAIL;
    }

    release_frame();
    s_jpeg = jpeg;
    s_jpeg_len = jpeg_len;
    s_width = fb->width;
    s_height = fb->height;
    s_capture_ms = 0;
    s_frame_id++;

    ESP_LOGI(TAG,
             "%s JPEG convert ok: frame=%lu, %dx%d, raw=%u bytes, luma_avg=%lu, low_light_gain=%d.%02dx, jpeg=%u bytes",
             mode_name,
             (unsigned long)s_frame_id,
             s_width,
             s_height,
             (unsigned)(s_width * s_height * 2),
             (unsigned long)luma_avg,
             low_light_gain_x100 / 100,
             low_light_gain_x100 % 100,
             (unsigned)s_jpeg_len);
    return ESP_OK;
}

static void apply_auto_image_controls(const char *mode_name)
{
    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor) {
        return;
    }
    // 每次重新初始化后显式打开 AE/AWB/AGC。OV3660 前几帧可能偏黑或偏绿，
    // 这里先让自动曝光和白平衡开始工作，再由 warmup 丢弃收敛帧。
    if (sensor->set_whitebal) {
        sensor->set_whitebal(sensor, 1);
    }
    if (sensor->set_awb_gain) {
        sensor->set_awb_gain(sensor, 1);
    }
    if (sensor->set_exposure_ctrl) {
        sensor->set_exposure_ctrl(sensor, 1);
    }
    if (sensor->set_gain_ctrl) {
        sensor->set_gain_ctrl(sensor, 1);
    }
    if (sensor->set_aec2) {
        sensor->set_aec2(sensor, 1);
    }
    if (sensor->set_ae_level) {
        sensor->set_ae_level(sensor, CAM_AE_LEVEL);
    }
    if (sensor->set_gainceiling) {
        sensor->set_gainceiling(sensor, GAINCEILING_128X);
    }
    if (sensor->set_brightness) {
        sensor->set_brightness(sensor, CAM_BRIGHTNESS_LEVEL);
    }
    if (sensor->set_raw_gma) {
        sensor->set_raw_gma(sensor, 1);
    }
    if (sensor->set_lenc) {
        sensor->set_lenc(sensor, 1);
    }
    if (sensor->set_denoise) {
        sensor->set_denoise(sensor, 2);
    }
    if (sensor->set_saturation) {
        sensor->set_saturation(sensor, 0);
    }
    ESP_LOGI(TAG,
             "%s auto controls enabled: AE/AWB/AGC, ae_level=%d, gainceiling=128x, brightness=+%d",
             mode_name,
             CAM_AE_LEVEL,
             CAM_BRIGHTNESS_LEVEL);
}

static esp_err_t warmup_frames(const char *mode_name, int frame_count)
{
    // OV3660 初始化后 AE/AWB/AGC 通常需要数帧收敛；实测前两帧可能黑/绿。
    // 这里丢弃预热帧，只把稳定帧返回给 Web，避免用户每次重启都看到异常首帧。
    for (int i = 0; i < frame_count; i++) {
        vTaskDelay(pdMS_TO_TICKS(CAM_WARMUP_DELAY_MS));
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            snprintf(s_last_error, sizeof(s_last_error), "%s warmup frame %d failed", mode_name, i + 1);
            ESP_LOGW(TAG, "%s", s_last_error);
            return ESP_FAIL;
        }
        log_frame_luma_stats(mode_name, fb, i + 1, frame_count);
        esp_camera_fb_return(fb);
    }
    log_sensor_exposure_stats(mode_name);
    return ESP_OK;
}

static void fill_camera_config(camera_config_t *config, pixformat_t pixel_format, framesize_t frame_size)
{
    memset(config, 0, sizeof(*config));
    config->pin_pwdn = CAM_PIN_PWDN;
    config->pin_reset = CAM_PIN_RESET;
    config->pin_xclk = CAM_PIN_XCLK;
    config->pin_sccb_sda = CAM_PIN_SIOD;
    config->pin_sccb_scl = CAM_PIN_SIOC;
    config->pin_d7 = CAM_PIN_D7;
    config->pin_d6 = CAM_PIN_D6;
    config->pin_d5 = CAM_PIN_D5;
    config->pin_d4 = CAM_PIN_D4;
    config->pin_d3 = CAM_PIN_D3;
    config->pin_d2 = CAM_PIN_D2;
    config->pin_d1 = CAM_PIN_D1;
    config->pin_d0 = CAM_PIN_D0;
    config->pin_vsync = CAM_PIN_VSYNC;
    config->pin_href = CAM_PIN_HREF;
    config->pin_pclk = CAM_PIN_PCLK;
    config->xclk_freq_hz = CONFIG_FRIDGE_CAMERA_XCLK_HZ;
    config->ledc_timer = LEDC_TIMER_0;
    config->ledc_channel = LEDC_CHANNEL_0;
    config->pixel_format = pixel_format;
    config->frame_size = frame_size;
    config->jpeg_quality = 18;
    config->fb_count = 1;
    config->fb_location = CAMERA_FB_IN_PSRAM;
    config->grab_mode = CAMERA_GRAB_WHEN_EMPTY;
}

static esp_err_t init_raw_camera(pixformat_t pixel_format, framesize_t frame_size, bool diag_mode, const char *mode_name)
{
    // 原始帧路径已在实机诊断中验证能成帧；普通预览先用软件 JPEG 绕过 OV3660 片上 JPEG 编码不稳定问题。
    camera_config_t config;
    fill_camera_config(&config, pixel_format, frame_size);

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        snprintf(s_last_error, sizeof(s_last_error), "%s esp_camera_init failed: %s", mode_name, esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", s_last_error);
        return err;
    }
    s_initialized = true;
    s_rgb565_diag_mode = diag_mode;
    s_software_jpeg_mode = !s_rgb565_diag_mode;
    s_hardware_jpeg_diag_mode = false;
    s_preview_source_format = pixel_format;

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor) {
        // 部分 OV3660 模组在 JPEG/QVGA 配置寄存器写入失败；原始帧只在低分辨率下做稳定 bring-up。
        sensor->set_framesize(sensor, frame_size);
    }
    apply_auto_image_controls(mode_name);
    err = warmup_frames(mode_name, CAM_RAW_WARMUP_FRAME_COUNT);
    if (err != ESP_OK) {
        return err;
    }
    return ESP_OK;
}

static esp_err_t init_hardware_jpeg_camera(void)
{
    // 硬件 JPEG 诊断沿用 esp32-camera 参考例程的小尺寸 JPEG 初始化策略：
    // 先以较大 JPEG 帧尺寸初始化，再切回 QVGA，避免部分 OV3660 寄存器在小尺寸 JPEG 下配置失败。
    camera_config_t config;
    fill_camera_config(&config, PIXFORMAT_JPEG, FRAMESIZE_HD);
    config.jpeg_quality = 12;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        snprintf(s_last_error, sizeof(s_last_error), "hardware jpeg esp_camera_init failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", s_last_error);
        return err;
    }

    s_initialized = true;
    s_rgb565_diag_mode = false;
    s_software_jpeg_mode = false;
    s_hardware_jpeg_diag_mode = true;
    s_preview_source_format = PIXFORMAT_JPEG;

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor) {
        int set_err = sensor->set_framesize(sensor, FRAMESIZE_QVGA);
        if (set_err != 0) {
            snprintf(s_last_error, sizeof(s_last_error), "hardware jpeg set QVGA failed: %d", set_err);
            ESP_LOGW(TAG, "%s", s_last_error);
        }
    }
    apply_auto_image_controls("hardware jpeg");
    err = warmup_frames("hardware jpeg", CAM_JPEG_WARMUP_FRAME_COUNT);
    if (err != ESP_OK) {
        return err;
    }
    return ESP_OK;
}

esp_err_t fridge_camera_probe(fridge_camera_probe_result_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->sccb_address = CAM_PROBE_SCCB_ADDR;
    out->expected_pid = CAM_PROBE_EXPECTED_PID;

    int64_t start_us = esp_timer_get_time();
    esp_err_t err = probe_xclk_start();
    if (err != ESP_OK) {
        snprintf(out->last_error, sizeof(out->last_error), "XCLK start failed: %s", esp_err_to_name(err));
        out->duration_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
        return err;
    }
    out->xclk_enabled = true;
    vTaskDelay(pdMS_TO_TICKS(20));

    err = probe_i2c_start();
    if (err != ESP_OK) {
        snprintf(out->last_error, sizeof(out->last_error), "SCCB/I2C start failed: %s", esp_err_to_name(err));
        probe_xclk_stop();
        out->duration_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
        return err;
    }
    out->sccb_ready = true;

    uint8_t pid_h = 0xff;
    uint8_t pid_l = 0xff;
    err = probe_read_reg16(CAM_PROBE_SCCB_ADDR, CAM_PROBE_PID_HIGH_REG, &pid_h);
    if (err == ESP_OK) {
        err = probe_read_reg16(CAM_PROBE_SCCB_ADDR, CAM_PROBE_PID_LOW_REG, &pid_l);
    }
    if (err == ESP_OK) {
        out->pid_high = pid_h;
        out->pid_low = pid_l;
        out->pid = ((uint16_t)pid_h << 8) | pid_l;
        out->ok = out->pid == CAM_PROBE_EXPECTED_PID;
        if (out->ok) {
            ESP_LOGI(TAG, "OV3660 probe ok: addr=0x%02x pid=0x%04x", CAM_PROBE_SCCB_ADDR, out->pid);
        } else {
            snprintf(out->last_error, sizeof(out->last_error), "unexpected PID 0x%04x, expected 0x%04x", out->pid, CAM_PROBE_EXPECTED_PID);
            ESP_LOGW(TAG, "OV3660 probe mismatch: %s", out->last_error);
            err = ESP_ERR_NOT_FOUND;
        }
    } else {
        snprintf(out->last_error, sizeof(out->last_error), "SCCB read 0x300A/0x300B failed: %s", esp_err_to_name(err));
        ESP_LOGW(TAG, "%s", out->last_error);
    }

    probe_i2c_stop();
    probe_xclk_stop();
    out->xclk_enabled = false;
    out->duration_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
    if (err != ESP_OK) {
        probe_set_error(out, out->last_error);
    }
    return err;
}

esp_err_t fridge_camera_init(void)
{
    if (s_initialized && s_software_jpeg_mode) {
        return ESP_OK;
    }
    if (s_initialized) {
        // 普通预览和诊断都会切换 esp32-camera 像素格式；切换前必须释放 DMA/LEDC/SCCB 状态。
        (void)fridge_camera_reset();
    }

    if (!esp_psram_is_initialized()) {
        set_last_error("PSRAM 未初始化，不能安全启动 OV3660 RGB565 帧缓冲");
        ESP_LOGE(TAG, "%s", s_last_error);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = init_raw_camera(PIXFORMAT_YUV422, FRAMESIZE_QVGA, false, "yuv422 qvga software jpeg");
    if (err != ESP_OK) {
        return err;
    }

    set_last_error("");
    ESP_LOGI(TAG,
             "OV3660 camera initialized: YUV422 QVGA xclk=%dHz, fb=PSRAM single frame, software JPEG preview",
             CONFIG_FRIDGE_CAMERA_XCLK_HZ);
    return ESP_OK;
}

esp_err_t fridge_camera_capture(void)
{
    esp_err_t err = fridge_camera_init();
    if (err != ESP_OK) {
        return err;
    }

    int64_t start_us = esp_timer_get_time();
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        set_last_error("esp_camera_fb_get returned NULL");
        ESP_LOGE(TAG, "%s", s_last_error);
        return ESP_FAIL;
    }
    if (fb->format != PIXFORMAT_YUV422 || fb->len == 0) {
        esp_camera_fb_return(fb);
        set_last_error("camera frame is not YUV422");
        ESP_LOGE(TAG, "%s", s_last_error);
        return ESP_FAIL;
    }

    err = convert_yuv_frame_to_jpeg(fb, 70, "qvga preview", false);
    if (err != ESP_OK) {
        esp_camera_fb_return(fb);
        return err;
    }
    s_capture_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
    esp_camera_fb_return(fb);

    set_last_error("");
    ESP_LOGI(TAG,
             "camera capture ok: frame=%lu, %dx%d, yuv_order=YUYV, software_jpeg=%u bytes, %lu ms, free_psram=%u",
             (unsigned long)s_frame_id,
             s_width,
             s_height,
             (unsigned)s_jpeg_len,
             (unsigned long)s_capture_ms,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    return ESP_OK;
}

esp_err_t fridge_camera_capture_ai_fullres(void)
{
    // AI 识别使用实测稳定的高分辨率 XGA，串口预览仍保持 QVGA。
    // 这里抓到 JPEG 后立即反初始化 DMA/帧缓冲，给后续 base64 和 HTTPS 请求释放 PSRAM。
    // QXGA/UXGA/SXGA 当前在该排线和驱动组合下不稳定，不进入成品主路径，避免用户每次识别都等待失败重试。
    if (s_initialized) {
        (void)fridge_camera_reset();
    } else {
        release_frame();
    }
    if (!esp_psram_is_initialized()) {
        set_last_error("PSRAM 未初始化，不能安全启动 OV3660 XGA AI 抓拍");
        ESP_LOGE(TAG, "%s", s_last_error);
        return ESP_ERR_INVALID_STATE;
    }

    int64_t start_us = esp_timer_get_time();
    esp_err_t last_err = ESP_ERR_NO_MEM;
    char last_reason[FRIDGE_CAMERA_MAX_ERROR_LEN + 1] = {0};

    for (size_t i = 0; i < sizeof(CAM_AI_CAPTURE_OPTIONS) / sizeof(CAM_AI_CAPTURE_OPTIONS[0]); i++) {
        const cam_ai_capture_option_t *option = &CAM_AI_CAPTURE_OPTIONS[i];
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        if (free_psram < option->min_free_psram) {
            snprintf(last_reason,
                     sizeof(last_reason),
                     "%s AI 抓拍跳过：PSRAM 需要约 %uKB，当前 %uKB",
                     option->label,
                     (unsigned)(option->min_free_psram / 1024),
                     (unsigned)(free_psram / 1024));
            ESP_LOGW(TAG, "%s", last_reason);
            continue;
        }

        ESP_LOGI(TAG,
                 "AI capture try %s: free_psram=%uKB, software JPEG quality=%u",
                 option->label,
                 (unsigned)(free_psram / 1024),
                 CAM_AI_FULLRES_JPEG_QUALITY);
        last_err = init_raw_camera(PIXFORMAT_YUV422, option->frame_size, false, option->mode_name);
        if (last_err != ESP_OK) {
            strlcpy(last_reason, s_last_error, sizeof(last_reason));
            (void)fridge_camera_reset();
            continue;
        }

        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            snprintf(last_reason, sizeof(last_reason), "%s esp_camera_fb_get returned NULL", option->label);
            set_last_error(last_reason);
            ESP_LOGW(TAG, "%s", s_last_error);
            last_err = ESP_FAIL;
            (void)fridge_camera_reset();
            continue;
        }

        last_err = convert_yuv_frame_to_jpeg(fb, CAM_AI_FULLRES_JPEG_QUALITY, option->mode_name, true);
        esp_camera_fb_return(fb);
        if (last_err != ESP_OK) {
            strlcpy(last_reason, s_last_error, sizeof(last_reason));
            (void)fridge_camera_reset();
            continue;
        }

        // 此处不能调用 fridge_camera_reset()，否则会把刚生成、准备提交 AI 的 JPEG 一起释放。
        (void)esp_camera_deinit();
        s_initialized = false;
        s_software_jpeg_mode = true;
        s_hardware_jpeg_diag_mode = false;
        s_rgb565_diag_mode = false;
        s_preview_source_format = PIXFORMAT_YUV422;
        s_capture_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
        set_last_error("");
        ESP_LOGI(TAG,
                 "AI high-res capture ok: requested=%s, actual=%dx%d, jpeg=%u bytes, %lu ms, free_psram=%uKB",
                 option->label,
                 s_width,
                 s_height,
                 (unsigned)s_jpeg_len,
                 (unsigned long)s_capture_ms,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        return ESP_OK;
    }

    strlcpy(s_last_error, "AI 高分辨率抓拍失败：", sizeof(s_last_error));
    strlcat(s_last_error, last_reason[0] ? last_reason : esp_err_to_name(last_err), sizeof(s_last_error));
    ESP_LOGE(TAG, "%s", s_last_error);
    return last_err;
}

esp_err_t fridge_camera_capture_hardware_jpeg_diag(void)
{
    if (s_initialized) {
        // 硬件 JPEG 诊断和软件 JPEG/RGB565 诊断使用不同的传感器输出格式；
        // 切换前完整释放 DMA、SCCB 和 LEDC 状态，避免前一模式残留影响判断。
        (void)fridge_camera_reset();
    }
    if (!esp_psram_is_initialized()) {
        set_last_error("PSRAM 未初始化，不能安全启动 OV3660 硬件 JPEG 帧缓冲");
        ESP_LOGE(TAG, "%s", s_last_error);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = init_hardware_jpeg_camera();
    if (err != ESP_OK) {
        return err;
    }

    int64_t start_us = esp_timer_get_time();
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        set_last_error("hardware jpeg esp_camera_fb_get returned NULL");
        ESP_LOGE(TAG, "%s", s_last_error);
        return ESP_FAIL;
    }
    if (fb->format != PIXFORMAT_JPEG || fb->len < 4 || fb->buf[0] != 0xff || fb->buf[1] != 0xd8) {
        esp_camera_fb_return(fb);
        set_last_error("hardware jpeg frame missing SOI marker");
        ESP_LOGE(TAG, "%s", s_last_error);
        return ESP_FAIL;
    }

    uint8_t *jpeg = heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpeg) {
        jpeg = heap_caps_malloc(fb->len, MALLOC_CAP_8BIT);
    }
    if (!jpeg) {
        esp_camera_fb_return(fb);
        set_last_error("hardware jpeg copy allocation failed");
        ESP_LOGE(TAG, "%s", s_last_error);
        return ESP_ERR_NO_MEM;
    }
    memcpy(jpeg, fb->buf, fb->len);

    release_frame();
    s_jpeg = jpeg;
    s_jpeg_len = fb->len;
    s_width = fb->width;
    s_height = fb->height;
    s_capture_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
    s_frame_id++;
    esp_camera_fb_return(fb);

    set_last_error("");
    ESP_LOGI(TAG,
             "hardware jpeg diag ok: frame=%lu, %dx%d, jpeg=%u bytes, %lu ms, free_psram=%u",
             (unsigned long)s_frame_id,
             s_width,
             s_height,
             (unsigned)s_jpeg_len,
             (unsigned long)s_capture_ms,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    return ESP_OK;
}

esp_err_t fridge_camera_capture_rgb565_diag(fridge_camera_diag_result_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    // RGB565 诊断需要用不同像素格式重新初始化 esp32-camera。
    // 这里先完整反初始化，避免 JPEG 模式残留的 DMA/SCCB/LEDC 状态影响诊断结果。
    (void)fridge_camera_reset();
    if (!esp_psram_is_initialized()) {
        strlcpy(out->last_error, "PSRAM 未初始化，不能安全启动 RGB565 诊断帧缓冲", sizeof(out->last_error));
        set_last_error(out->last_error);
        return ESP_ERR_INVALID_STATE;
    }

    // 诊断帧使用 QQVGA + RGB565，数据量小且不依赖 JPEG SOI 标记，适合判断 DVP 同步是否基本可用。
    esp_err_t err = init_raw_camera(PIXFORMAT_RGB565, FRAMESIZE_QQVGA, true, "rgb565 diag");
    if (err != ESP_OK) {
        snprintf(out->last_error, sizeof(out->last_error), "rgb565 esp_camera_init failed: %s", esp_err_to_name(err));
        set_last_error(out->last_error);
        ESP_LOGE(TAG, "%s", out->last_error);
        return err;
    }

    int64_t start_us = esp_timer_get_time();
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        strlcpy(out->last_error, "rgb565 esp_camera_fb_get returned NULL", sizeof(out->last_error));
        set_last_error(out->last_error);
        ESP_LOGE(TAG, "%s", out->last_error);
        return ESP_FAIL;
    }

    out->ok = true;
    out->width = fb->width;
    out->height = fb->height;
    out->bytes = fb->len;
    out->capture_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
    out->checksum = checksum32(fb->buf, fb->len);
    out->first_len = fb->len < sizeof(out->first_bytes) ? fb->len : sizeof(out->first_bytes);
    memcpy(out->first_bytes, fb->buf, out->first_len);
    esp_camera_fb_return(fb);

    set_last_error("");
    ESP_LOGI(TAG,
             "RGB565 diag ok: %dx%d, bytes=%u, checksum=0x%08lx, %lu ms",
             out->width,
             out->height,
             (unsigned)out->bytes,
             (unsigned long)out->checksum,
             (unsigned long)out->capture_ms);
    return ESP_OK;
}

void fridge_camera_get_status(fridge_camera_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->initialized = s_initialized;
    out->has_frame = s_jpeg != NULL && s_jpeg_len > 0;
    out->width = s_width;
    out->height = s_height;
    out->jpeg_bytes = s_jpeg_len;
    out->capture_ms = s_capture_ms;
    out->frame_id = s_frame_id;
    out->free_heap_kb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024);
    out->free_psram_kb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    strlcpy(out->pixel_format,
            s_rgb565_diag_mode ? "RGB565" : (s_hardware_jpeg_diag_mode ? "JPEG" : (s_software_jpeg_mode && s_preview_source_format == PIXFORMAT_YUV422 ? "YUV422->JPEG" : "JPEG")),
            sizeof(out->pixel_format));
    strlcpy(out->frame_size, frame_size_label_from_dimensions(s_width, s_height), sizeof(out->frame_size));
    strlcpy(out->last_error, s_last_error, sizeof(out->last_error));
}

esp_err_t fridge_camera_get_frame(fridge_camera_frame_view_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_jpeg || s_jpeg_len == 0) {
        set_last_error("没有最近照片，请先执行 camera_capture");
        return ESP_ERR_NOT_FOUND;
    }
    memset(out, 0, sizeof(*out));
    out->data = s_jpeg;
    out->len = s_jpeg_len;
    out->width = s_width;
    out->height = s_height;
    out->capture_ms = s_capture_ms;
    out->frame_id = s_frame_id;
    return ESP_OK;
}

esp_err_t fridge_camera_clear_frame(void)
{
    release_frame();
    set_last_error("");
    return ESP_OK;
}

esp_err_t fridge_camera_reset(void)
{
    release_frame();
    if (s_initialized) {
        esp_camera_deinit();
        s_initialized = false;
    }
    s_rgb565_diag_mode = false;
    s_software_jpeg_mode = false;
    s_hardware_jpeg_diag_mode = false;
    s_preview_source_format = 0;
    set_last_error("");
    return ESP_OK;
}
