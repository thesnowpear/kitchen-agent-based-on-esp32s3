// 冰箱小精灵主控启动入口。
// 这里只做系统初始化编排：NVS、诊断、网络、USB JSON 协议和专用硬件测试模式。
// 硬件注意：屏幕测试与摄像头测试都会占用专用 GPIO，不能和正常运维链路同时启用。

#include "esp_log.h"

#if CONFIG_FRIDGE_SCREEN_TEST && CONFIG_FRIDGE_CAMERA_TEST
#error "CONFIG_FRIDGE_SCREEN_TEST and CONFIG_FRIDGE_CAMERA_TEST cannot be enabled at the same time"
#endif

#if CONFIG_FRIDGE_SCREEN_TEST
#include "fridge_display_test.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#include "esp_err.h"
#include "nvs_flash.h"
#include "fridge_ai_client.h"
#include "fridge_camera.h"
#include "fridge_diagnostics.h"
#include "fridge_mqtt_protocol.h"
#include "fridge_network.h"
#include "fridge_usb_protocol.h"
#include "fridge_ui.h"
#if !CONFIG_FRIDGE_CAMERA_TEST
#include "fridge_asr.h"
#include "fridge_audio.h"
#include "fridge_radar.h"
#include "fridge_sensors.h"
#include "fridge_speaker.h"
#include "fridge_wake_word.h"
#endif
#endif

static const char *TAG = "fridge_main";

#if CONFIG_FRIDGE_SCREEN_TEST
static void display_test_task(void *arg)
{
    (void)arg;
    // 独立屏幕测试任务：长期循环刷测试图案，避免阻塞 ESP-IDF 的 main_task 导致系统看门狗复位。
    fridge_display_test_run();
}
#endif

#if !CONFIG_FRIDGE_SCREEN_TEST
static void nvs_init_safe(void)
{
    // 初始化 NVS，用于保存 Wi-Fi SSID/密码。
    // 注意：NVS 写入发生在 Flash 中，后续要避免高频写入；本项目只在用户更新配网时写入。
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}
#endif

void app_main(void)
{
#if CONFIG_FRIDGE_SCREEN_TEST
    // 独立屏幕测试模式：创建后台任务运行 QSPI 点亮和彩色图案，避免 Wi-Fi 发射峰值干扰首次硬件排查。
    ESP_LOGW(TAG, "CONFIG_FRIDGE_SCREEN_TEST is enabled");
    BaseType_t ok = xTaskCreate(display_test_task, "display_test", 8192, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "create display_test task failed");
    }
#else
#if CONFIG_FRIDGE_CAMERA_TEST
    // 摄像头专用测试模式：跳过屏幕、触摸、传感器和麦克风，避免 OV3660 DVP 引脚与既有外设互相冲突。
    // 注意：上电前必须确认 OV3660 模组是 3.3V 直连版，且 PWDN/XCLK/SCCB/DVP 接线与计划表一致。
    ESP_LOGW(TAG, "CONFIG_FRIDGE_CAMERA_TEST is enabled; screen/sensors/audio are skipped");
    nvs_init_safe();
    fridge_diagnostics_init();

    ESP_ERROR_CHECK(fridge_network_init());
    ESP_ERROR_CHECK(fridge_ai_client_init());
    ESP_ERROR_CHECK(fridge_mqtt_protocol_init());
    // 摄像头测试模式下先启动 USB 控制台，再由 Web Serial 的 camera_capture 命令按需初始化 OV3660。
    // 这样即使摄像头断开或供电异常，主控也能保持在线，避免上电阶段卡在 SCCB 探测导致看门狗复位。
    ESP_LOGW(TAG, "camera init is deferred until camera_capture; keep OV3660 disconnected until power rails are verified");
    ESP_ERROR_CHECK(fridge_usb_protocol_start());
    ESP_LOGI(TAG, "camera-only USB console is ready");
#else
    // 正常主控模式：初始化 NVS、诊断、Wi-Fi、AI 配置和 USB 配网控制台。
    // 注意：此路径会启动 Wi-Fi，真实硬件调试时需确认 5V/USB 供电足够稳定。
    nvs_init_safe();
    fridge_diagnostics_init();

    ESP_ERROR_CHECK(fridge_network_init());
    ESP_ERROR_CHECK(fridge_ai_client_init());
    ESP_ERROR_CHECK(fridge_mqtt_protocol_init());
    ESP_ERROR_CHECK(fridge_asr_init());
    ESP_ERROR_CHECK(fridge_sensors_init());
    ESP_ERROR_CHECK(fridge_radar_init());
    ESP_ERROR_CHECK(fridge_audio_init());
    ESP_ERROR_CHECK(fridge_speaker_init());
    esp_err_t wake_ret = fridge_wake_word_init();
    if (wake_ret != ESP_OK) {
        ESP_LOGW(TAG, "wake word init deferred: %s", esp_err_to_name(wake_ret));
    }
    ESP_ERROR_CHECK(fridge_usb_protocol_start());
    ESP_LOGI(TAG, "USB provisioning console is ready");

#if CONFIG_FRIDGE_UI_ENABLE
    esp_err_t ui_ret = fridge_ui_init();
    if (ui_ret == ESP_OK) {
        ESP_LOGI(TAG, "local LVGL touch UI started");
    } else {
        ESP_LOGW(TAG, "local LVGL touch UI skipped: %s", esp_err_to_name(ui_ret));
    }
#endif

    esp_err_t saved_ret = fridge_network_connect_saved_async();
    if (saved_ret == ESP_OK) {
        ESP_LOGI(TAG, "saved Wi-Fi background connect started");
    } else {
        ESP_LOGW(TAG, "start saved Wi-Fi background connect failed: %s", esp_err_to_name(saved_ret));
    }

    esp_err_t mqtt_ret = fridge_mqtt_start();
    if (mqtt_ret == ESP_OK) {
        ESP_LOGI(TAG, "saved MQTT background connect started");
    } else {
        ESP_LOGI(TAG, "MQTT not started yet: %s", esp_err_to_name(mqtt_ret));
    }
#endif
#endif
}
