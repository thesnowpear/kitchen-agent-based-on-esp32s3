// 冰箱小精灵主控启动入口。
// 这里只做系统初始化编排：NVS、诊断、网络、USB JSON 协议、recovery 兜底和专用硬件测试模式。
// 硬件注意：屏幕测试与摄像头测试都会占用专用 GPIO，不能和正常运维链路同时启用。

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#if CONFIG_FRIDGE_SCREEN_TEST && CONFIG_FRIDGE_CAMERA_TEST
#error "CONFIG_FRIDGE_SCREEN_TEST and CONFIG_FRIDGE_CAMERA_TEST cannot be enabled at the same time"
#endif

#if CONFIG_FRIDGE_RECOVERY_APP
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "fridge_diagnostics.h"
#include "fridge_network.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#elif CONFIG_FRIDGE_SCREEN_TEST
#include "fridge_display_test.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#include "fridge_ai_client.h"
#include "fridge_camera.h"
#include "fridge_diagnostics.h"
#include "fridge_display.h"
#include "fridge_mqtt_protocol.h"
#include "fridge_network.h"
#include "fridge_usb_protocol.h"
#include "fridge_ui.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if !CONFIG_FRIDGE_CAMERA_TEST
#include "fridge_asr.h"
#include "fridge_audio.h"
#include "fridge_kitchen_tools.h"
#include "fridge_radar.h"
#include "fridge_sensors.h"
#include "fridge_speaker.h"
#include "fridge_state_machine.h"
#include "fridge_wake_word.h"
#endif
#endif

static const char *TAG = "fridge_main";

#if !CONFIG_FRIDGE_SCREEN_TEST
static void nvs_init_safe(void)
{
    // 初始化 NVS，用于保存 Wi-Fi、API 配置和恢复模式需要的网络参数。
    // 注意：NVS 写入发生在 Flash 中，后续要避免高频写入；本项目只在用户更新配置时写入。
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}
#endif

#if !CONFIG_FRIDGE_RECOVERY_APP && !CONFIG_FRIDGE_SCREEN_TEST && !CONFIG_FRIDGE_CAMERA_TEST
static void mqtt_autostart_task(void *arg)
{
    (void)arg;
    // Wi-Fi 是后台连接的，MQTT 必须等拿到 IP 后再启动；否则只会在启动早期返回 ESP_ERR_INVALID_STATE。
    for (int i = 0; i < 120; i++) {
        fridge_network_status_t net = {0};
        if (fridge_network_get_status(&net) == ESP_OK && net.connected) {
            esp_err_t err = fridge_mqtt_start();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "MQTT autostart after Wi-Fi connected");
            } else {
                ESP_LOGW(TAG, "MQTT autostart failed after Wi-Fi connected: %s", esp_err_to_name(err));
            }
            vTaskDelete(NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGW(TAG, "MQTT autostart timed out waiting for Wi-Fi");
    vTaskDelete(NULL);
}

static void start_saved_network_and_mqtt_early(void)
{
    esp_err_t saved_ret = fridge_network_connect_saved_async();
    if (saved_ret == ESP_OK) {
        ESP_LOGI(TAG, "saved Wi-Fi background connect started");
    } else {
        ESP_LOGW(TAG, "start saved Wi-Fi background connect failed: %s", esp_err_to_name(saved_ret));
    }

    esp_err_t mqtt_ret = fridge_mqtt_start();
    if (mqtt_ret == ESP_OK) {
        ESP_LOGI(TAG, "saved MQTT background connect started");
        return;
    }

    // 首次上电时 Wi-Fi 往往还没拿到 IP。这里在语音/唤醒词等大内存组件启动前短暂等待，
    // 让 MQTT 任务优先占住连续内部 SRAM，保证离线恢复后能立刻接收云端快照并上传本地脏数据。
    ESP_LOGI(TAG, "MQTT not started yet: %s; waiting briefly before audio stack", esp_err_to_name(mqtt_ret));
    for (int i = 0; i < 20; i++) {
        fridge_network_status_t net = {0};
        if (fridge_network_get_status(&net) == ESP_OK && net.connected) {
            mqtt_ret = fridge_mqtt_start();
            if (mqtt_ret == ESP_OK) {
                ESP_LOGI(TAG, "MQTT started before audio stack after Wi-Fi connected");
                return;
            }
            ESP_LOGW(TAG, "MQTT early start after Wi-Fi connected failed: %s", esp_err_to_name(mqtt_ret));
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    BaseType_t ok = xTaskCreate(mqtt_autostart_task, "mqtt_autostart", 4096, NULL, 4, NULL);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "create MQTT autostart task failed");
    }
}
#endif

#if CONFIG_FRIDGE_RECOVERY_APP
static void recovery_print_json_status(const char *request_id, const char *command)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    fridge_network_status_t net = {0};
    fridge_network_get_status(&net);

    // recovery 响应保持一行 JSON，方便 Web Serial 或脚本在日志噪声中按 request_id 匹配。
    printf("{\"type\":\"response\",\"request_id\":\"%s\",\"command\":\"%s\",\"ok\":true,"
           "\"payload\":{\"mode\":\"recovery\",\"firmware\":\"%s\",\"runningPartition\":\"%s\","
           "\"targetPartition\":\"ota_0\",\"wifiConnected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\","
           "\"note\":\"local recovery shell; future OTA restore writes main firmware to ota_0\"}}\n",
           request_id,
           command,
           app_desc ? app_desc->version : "dev",
           running ? running->label : "unknown",
           net.connected ? "true" : "false",
           net.ssid,
           net.ip);
    fflush(stdout);
}

static void recovery_print_error(const char *request_id, const char *command, const char *error)
{
    printf("{\"type\":\"response\",\"request_id\":\"%s\",\"command\":\"%s\",\"ok\":false,\"error\":\"%s\"}\n",
           request_id,
           command,
           error);
    fflush(stdout);
}

static bool recovery_get_json_string(const char *line, const char *key, char *out, size_t out_size)
{
    // recovery 只解析调试脚本发来的短 JSON Lines，避免引入完整 JSON 库扩大 ota_1 固件体积。
    if (!line || !key || !out || out_size == 0) {
        return false;
    }
    out[0] = '\0';

    char pattern[40];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(line, pattern);
    if (!p) {
        return false;
    }
    p = strchr(p + strlen(pattern), ':');
    if (!p) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;

    size_t used = 0;
    while (*p && *p != '"' && used + 1 < out_size) {
        if (*p == '\\' && p[1]) {
            p++;
        }
        out[used++] = *p++;
    }
    out[used] = '\0';
    return used > 0;
}

static void recovery_handle_set_network(const char *line, const char *request_id, const char *command)
{
    fridge_wifi_config_t cfg = {0};
    recovery_get_json_string(line, "ssid", cfg.ssid, sizeof(cfg.ssid));
    recovery_get_json_string(line, "password", cfg.password, sizeof(cfg.password));
    if (cfg.ssid[0] == '\0') {
        recovery_print_error(request_id, command, "ssid is required");
        return;
    }

    // recovery 允许保存 Wi-Fi，便于主固件损坏时仍能靠本地兜底程序联网下载后续主固件。
    esp_err_t err = fridge_network_connect(&cfg, true);
    if (err != ESP_OK) {
        fridge_network_status_t status = {0};
        fridge_network_get_status(&status);
        recovery_print_error(request_id, command, status.last_error[0] ? status.last_error : esp_err_to_name(err));
        return;
    }
    recovery_print_json_status(request_id, command);
}

static void recovery_handle_boot_main(const char *request_id, const char *command)
{
    const esp_partition_t *main_app = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, "ota_0");
    if (!main_app) {
        recovery_print_error(request_id, command, "ota_0 main partition not found");
        return;
    }

    // recovery 不覆盖自身，只把下次启动目标切回主固件 ota_0。
    // 如果 ota_0 镜像已损坏，bootloader 仍会回到可启动分区，避免擦写恢复固件导致失去本地兜底。
    esp_err_t err = esp_ota_set_boot_partition(main_app);
    if (err != ESP_OK) {
        recovery_print_error(request_id, command, esp_err_to_name(err));
        return;
    }
    recovery_print_json_status(request_id, command);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

static void recovery_handle_restore_main(const char *line, const char *request_id, const char *command)
{
    char url[256] = "";
    recovery_get_json_string(line, "url", url, sizeof(url));
    if (url[0] == '\0') {
        recovery_print_error(request_id, command, "url is required");
        return;
    }

    const esp_partition_t *main_app = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, "ota_0");
    if (!main_app) {
        recovery_print_error(request_id, command, "ota_0 main partition not found");
        return;
    }

    // 从 recovery 覆盖 ota_0。这里依赖系统证书包校验 HTTPS 服务器证书；
    // 生产前还需要增加固件签名/哈希白名单，避免只凭 URL 下载未授权镜像。
    esp_http_client_config_t http_config = {
        .url = url,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
        .bulk_flash_erase = true,
        .partition = {
            .staging = main_app,
            .final = main_app,
            .finalize_with_copy = false,
        },
    };

    puts("{\"type\":\"event\",\"event\":\"recovery_restore_started\",\"target\":\"ota_0\"}");
    fflush(stdout);
    esp_err_t err = esp_https_ota(&ota_config);
    if (err != ESP_OK) {
        recovery_print_error(request_id, command, esp_err_to_name(err));
        return;
    }

    esp_err_t boot_err = esp_ota_set_boot_partition(main_app);
    if (boot_err != ESP_OK) {
        recovery_print_error(request_id, command, esp_err_to_name(boot_err));
        return;
    }

    recovery_print_json_status(request_id, command);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

static void recovery_shell_task(void *arg)
{
    (void)arg;
    char line[1024];

    ESP_LOGI(TAG, "recovery JSON Lines shell started");
    puts("{\"type\":\"event\",\"event\":\"recovery_ready\",\"mode\":\"recovery\",\"commands\":[\"get_status\",\"set_network\",\"restore_main\",\"boot_main\"]}");
    fflush(stdout);

    while (true) {
        if (!fgets(line, sizeof(line), stdin)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        char request_id[48] = "recovery";
        char command[32] = "";
        recovery_get_json_string(line, "request_id", request_id, sizeof(request_id));
        recovery_get_json_string(line, "command", command, sizeof(command));
        if (command[0] == '\0') {
            recovery_get_json_string(line, "cmd", command, sizeof(command));
        }

        if (strcmp(command, "get_status") == 0 || strcmp(command, "get_diagnostics") == 0) {
            recovery_print_json_status(request_id, command);
        } else if (strcmp(command, "set_network") == 0) {
            recovery_handle_set_network(line, request_id, command);
        } else if (strcmp(command, "boot_main") == 0) {
            recovery_handle_boot_main(request_id, command);
        } else if (strcmp(command, "restore_main") == 0) {
            recovery_handle_restore_main(line, request_id, command);
        } else {
            recovery_print_error(request_id, command[0] ? command : "unknown", "recovery command not available");
        }
    }
}
#elif CONFIG_FRIDGE_SCREEN_TEST
static void display_test_task(void *arg)
{
    (void)arg;
    // 独立屏幕测试任务：长期循环刷测试图案，避免阻塞 ESP-IDF 的 main_task 导致系统看门狗复位。
    fridge_display_test_run();
}
#endif

void app_main(void)
{
#if CONFIG_FRIDGE_RECOVERY_APP
    // 小型本地恢复模式：只启动 NVS、诊断、Wi-Fi 和串口 JSON Lines。
    // 注意：该固件应烧录到 ota_1 小分区，禁止在此模式中初始化屏幕、摄像头、语音或 AI 大组件。
    ESP_LOGW(TAG, "CONFIG_FRIDGE_RECOVERY_APP is enabled; local recovery mode");
    nvs_init_safe();
    fridge_diagnostics_init();
    ESP_ERROR_CHECK(fridge_network_init());

    esp_err_t saved_ret = fridge_network_connect_saved_async();
    if (saved_ret == ESP_OK) {
        ESP_LOGI(TAG, "recovery saved Wi-Fi background connect started");
    } else {
        ESP_LOGW(TAG, "recovery saved Wi-Fi not started: %s", esp_err_to_name(saved_ret));
    }

    BaseType_t ok = xTaskCreate(recovery_shell_task, "recovery_shell", 6144, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "create recovery_shell task failed");
    }
#elif CONFIG_FRIDGE_SCREEN_TEST
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
    ESP_ERROR_CHECK(fridge_sensors_init());

#if CONFIG_FRIDGE_UI_ENABLE
    // 屏幕 QSPI 刷新需要一块连续的 DMA 内部 SRAM。UI 先启动可避免音频、
    // 扬声器和唤醒词组件占用/碎片化内部内存后导致屏幕初始化失败。
    esp_err_t ui_ret = fridge_ui_init();
    if (ui_ret == ESP_OK) {
        ESP_LOGI(TAG, "local LVGL touch UI started");
        for (int i = 0; i < 30 && !fridge_display_is_ready(); i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!fridge_display_is_ready()) {
            ESP_LOGW(TAG, "display not ready after early UI wait; keep booting with USB diagnostics");
        }
    } else {
        ESP_LOGW(TAG, "local LVGL touch UI skipped: %s", esp_err_to_name(ui_ret));
    }
#endif

    esp_err_t usb_ret = fridge_usb_protocol_start();
    if (usb_ret == ESP_OK) {
        ESP_LOGI(TAG, "USB provisioning console is ready");
    } else {
        ESP_LOGW(TAG, "USB provisioning console skipped: %s", esp_err_to_name(usb_ret));
    }

    start_saved_network_and_mqtt_early();

    ESP_ERROR_CHECK(fridge_asr_init());
    ESP_ERROR_CHECK(fridge_radar_init());
    ESP_ERROR_CHECK(fridge_audio_init());
    ESP_ERROR_CHECK(fridge_state_machine_init());
    ESP_ERROR_CHECK(fridge_speaker_init());
    ESP_ERROR_CHECK(fridge_kitchen_tools_init());
    // 比赛同步演示优先保证 Wi-Fi/MQTT、库存、配置、ASR/TTS 和屏幕 UI 稳定在线。
    // 当前 ESP-SR 热词模型与 MQTT/UI 同时运行时会触碰内部锁/映射资源边界，先禁用热词唤醒；
    // 手动按钮语音、ASR、TTS 和云端双向同步仍然可用。
    ESP_LOGW(TAG, "wake word disabled in cloud sync demo firmware");
#endif
#endif
}
