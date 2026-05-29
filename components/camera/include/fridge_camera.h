#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_CAMERA_MAX_ERROR_LEN 128
#define FRIDGE_CAMERA_DATA_URL_PREFIX "data:image/jpeg;base64,"

// 摄像头状态：只描述 OV3660 专用测试链路，不代表后续与屏幕/麦克风共存后的最终状态。
typedef struct {
    bool initialized;
    bool has_frame;
    int width;
    int height;
    size_t jpeg_bytes;
    uint32_t capture_ms;
    uint32_t frame_id;
    uint32_t free_heap_kb;
    uint32_t free_psram_kb;
    char pixel_format[16];
    char frame_size[16];
    char last_error[FRIDGE_CAMERA_MAX_ERROR_LEN + 1];
} fridge_camera_status_t;

// 最近一张 JPEG 的只读视图：调用者不得释放 data，需在下一次抓拍或 clear 前使用完。
typedef struct {
    const uint8_t *data;
    size_t len;
    int width;
    int height;
    uint32_t capture_ms;
    uint32_t frame_id;
} fridge_camera_frame_view_t;

// UI 实时预览帧：RGB565 数据由调用者持有，使用后必须调用 fridge_camera_free_preview_frame() 释放。
// 注意：该帧只服务屏幕取景，不用于 AI 识别；AI 识别仍使用更清晰的 JPEG 抓拍路径。
typedef struct {
    uint8_t *rgb565;
    size_t len;
    int width;
    int height;
    uint32_t capture_ms;
    uint32_t frame_id;
} fridge_camera_preview_frame_t;

// RGB565 诊断结果：只用于判断 DVP 同步和并口采样是否能形成完整帧，不返回大图。
typedef struct {
    bool ok;
    int width;
    int height;
    size_t bytes;
    uint32_t capture_ms;
    uint32_t checksum;
    uint8_t first_bytes[16];
    size_t first_len;
    char last_error[FRIDGE_CAMERA_MAX_ERROR_LEN + 1];
} fridge_camera_diag_result_t;

// 摄像头安全探测结果：只使用 XCLK + SCCB 读取芯片 ID，不启动 DVP/DMA 抓图。
typedef struct {
    bool ok;
    bool xclk_enabled;
    bool sccb_ready;
    uint8_t sccb_address;
    uint8_t pid_high;
    uint8_t pid_low;
    uint16_t pid;
    uint16_t expected_pid;
    uint32_t duration_ms;
    char last_error[FRIDGE_CAMERA_MAX_ERROR_LEN + 1];
} fridge_camera_probe_result_t;

// 初始化 OV3660。硬件注意：VDD/DVDD1.5V 接 1.5V 稳压，VDD2.8V/IOVDD/AVDD 按当前模组参数接 3.3V。
esp_err_t fridge_camera_init(void);

// 安全探测 OV3660：只接电源/GND、SDA/SCL 和 XCLK 时使用，避免完整拍照链路触发 DVP/DMA。
esp_err_t fridge_camera_probe(fridge_camera_probe_result_t *out);

// 抓拍一张 QVGA YUV422，并软件压缩为 JPEG 后保存到 PSRAM/RAM 中作为最近帧。
esp_err_t fridge_camera_capture(void);

// 抓取一张低分辨率 RGB565 预览帧，用于 LVGL 登记页实时取景。
esp_err_t fridge_camera_capture_preview_rgb565(fridge_camera_preview_frame_t *out);

// 释放 UI 预览帧数据。
void fridge_camera_free_preview_frame(fridge_camera_preview_frame_t *frame);

// 抓拍当前实测稳定的 OV3660 XGA 图片并软件压缩为 JPEG，供 AI 图片识别使用。
// 注意：该路径比串口预览清晰，但不会硬闯当前不稳定的 QXGA/UXGA/SXGA 档位。
esp_err_t fridge_camera_capture_ai_fullres(void);

// 抓拍一张 OV3660 片上 JPEG 诊断帧；只用于验证硬件 JPEG 编码链路，不替代稳定的软件 JPEG 主路径。
esp_err_t fridge_camera_capture_hardware_jpeg_diag(void);

// 抓取一帧 RGB565 诊断帧，只返回统计信息，用于区分 JPEG 编码问题和 DVP 总线问题。
esp_err_t fridge_camera_capture_rgb565_diag(fridge_camera_diag_result_t *out);

// 获取最近帧状态。
void fridge_camera_get_status(fridge_camera_status_t *out);

// 获取最近 JPEG 帧只读视图。
esp_err_t fridge_camera_get_frame(fridge_camera_frame_view_t *out);

// 清除最近帧，释放 PSRAM/RAM；不会反初始化摄像头硬件。
esp_err_t fridge_camera_clear_frame(void);

// 反初始化摄像头驱动并释放帧缓存；用于切换 JPEG/RGB565 诊断模式或清理 LEDC/I2C 资源。
esp_err_t fridge_camera_reset(void);

#ifdef __cplusplus
}
#endif
