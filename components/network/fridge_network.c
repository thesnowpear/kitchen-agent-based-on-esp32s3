// 冰箱小精灵网络组件。
// 负责 ESP32-S3 的 Wi-Fi STA、真实 AP 扫描、NVS 凭据保存、自动重连和 SNTP 联网验证。
// 注意：本组件不操作任何 GPIO；Wi-Fi 发射存在电流峰值，调试时需确认 USB/5V 供电稳定。

#include "fridge_network.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_DISCONNECTED_BIT BIT2
#define WIFI_CONNECT_TIMEOUT_MS 35000
#define WIFI_DISCONNECT_TIMEOUT_MS 3000
#define WIFI_MAXIMUM_RETRY 5
#define WIFI_SNTP_TASK_STACK 4096
#define WIFI_SAVED_CONNECT_TASK_STACK 4096
#define WIFI_NVS_NAMESPACE "fridge_net"
#define WIFI_NVS_KEY_SSID "ssid"
#define WIFI_NVS_KEY_PASSWORD "password"

static const char *TAG = "fridge_network";

static EventGroupHandle_t s_wifi_event_group;
static SemaphoreHandle_t s_connect_mutex;
static SemaphoreHandle_t s_sntp_mutex;
static TaskHandle_t s_sntp_task;
static TaskHandle_t s_saved_connect_task;
static esp_netif_t *s_sta_netif;
static bool s_initialized;
static bool s_connected;
static bool s_connecting;
static bool s_manual_disconnect;
static bool s_saved;
static bool s_internet_ready;
static bool s_sntp_initialized;
static int s_retry_num;
static int s_last_disconnect_reason;
static int8_t s_last_rssi;
static char s_current_ssid[FRIDGE_WIFI_MAX_SSID_LEN + 1];
static char s_current_ip[FRIDGE_WIFI_MAX_IP_LEN];
static char s_last_error[FRIDGE_WIFI_MAX_ERROR_LEN];
static char s_ntp_server[64] = FRIDGE_WIFI_DEFAULT_NTP;

static const char *wifi_authmode_to_string(wifi_auth_mode_t authmode)
{
    switch (authmode) {
    case WIFI_AUTH_OPEN:
        return "OPEN";
    case WIFI_AUTH_OWE:
        return "OWE";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA";
    case WIFI_AUTH_WPA2_PSK:
        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK:
        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA2/WPA3";
    case WIFI_AUTH_WAPI_PSK:
        return "WAPI";
    default:
        return "UNKNOWN";
    }
}

static void set_last_error(const char *message)
{
    strlcpy(s_last_error, message ? message : "", sizeof(s_last_error));
}

static const char *wifi_disconnect_reason_to_string(int reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
        return "认证超时，请检查密码或热点兼容性";
    case WIFI_REASON_AUTH_LEAVE:
    case WIFI_REASON_ASSOC_LEAVE:
    case WIFI_REASON_STA_LEAVING:
        return "设备主动断开旧连接";
    case WIFI_REASON_ASSOC_TOOMANY:
        return "热点连接设备数量已满";
    case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY:
        return "连接长时间无响应，请靠近热点或检查热点负载";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "握手超时，常见原因是密码错误、WPA3/PMF 兼容性或信号不稳";
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_802_1X_AUTH_FAILED:
    case WIFI_REASON_AUTH_FAIL:
        return "认证失败，请检查 Wi-Fi 密码";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "热点信号丢失，请靠近热点并确认供电稳定";
    case WIFI_REASON_NO_AP_FOUND:
        return "未找到该 SSID，请确认热点为 2.4GHz 且名称正确";
    case WIFI_REASON_ASSOC_FAIL:
        return "关联失败，请检查热点兼容性和连接设备数量";
    case WIFI_REASON_CONNECTION_FAIL:
        return "连接失败，请检查热点、密码和信号";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "找到热点但安全模式不兼容，请关闭 WPA3-only/强制 PMF 或开启兼容模式";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "热点信号过弱，请靠近后重试";
    default:
        return "连接被热点断开，请检查密码、2.4GHz、信号和供电";
    }
}

static bool wifi_disconnect_reason_is_terminal(int reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_802_1X_AUTH_FAILED:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return true;
    default:
        return false;
    }
}

static void stop_wifi_connecting(void)
{
    // 结束当前 STA 连接流程，避免失败后的后台重试继续占用 Wi-Fi 状态机。
    if (!s_wifi_event_group) {
        return;
    }
    s_manual_disconnect = true;
    esp_err_t err = esp_wifi_disconnect();
    if (err == ESP_OK) {
        xEventGroupWaitBits(s_wifi_event_group,
                            WIFI_DISCONNECTED_BIT,
                            pdTRUE,
                            pdFALSE,
                            pdMS_TO_TICKS(WIFI_DISCONNECT_TIMEOUT_MS));
    }
    s_manual_disconnect = false;
    s_connecting = false;
}

static esp_err_t ensure_event_loop(void)
{
    esp_err_t err = esp_event_loop_create_default();
    if (err == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    return err;
}

static void sntp_sync_task(void *arg)
{
    (void)arg;
    // 后台校时避免阻塞配网命令；HTTPS 请求前仍会再次确认系统时间是否可用。
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_err_t err = fridge_network_sync_time();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "background SNTP sync done");
    } else {
        ESP_LOGW(TAG, "background SNTP sync failed: %s", esp_err_to_name(err));
    }
    s_sntp_task = NULL;
    vTaskDelete(NULL);
}

static void start_sntp_sync_task(void)
{
    if (s_sntp_task) {
        return;
    }

    BaseType_t ok = xTaskCreate(sntp_sync_task, "sntp_sync", WIFI_SNTP_TASK_STACK, NULL, 3, &s_sntp_task);
    if (ok != pdPASS) {
        s_sntp_task = NULL;
        ESP_LOGW(TAG, "create SNTP sync task failed");
    }
}

static void saved_connect_task(void *arg)
{
    (void)arg;
    // 设备启动时先让 USB 调试面板可用，再后台尝试连接已保存 Wi-Fi。
    // 这样旧密码、热点关闭或信号问题不会阻塞 get_status/get_network。
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_err_t err = fridge_network_connect_saved();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "saved Wi-Fi connected in background");
    } else if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "no saved Wi-Fi credential, waiting for USB provisioning");
    } else {
        ESP_LOGW(TAG, "saved Wi-Fi background connect failed: %s", esp_err_to_name(err));
    }
    s_saved_connect_task = NULL;
    vTaskDelete(NULL);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi STA started");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        int reason = event ? event->reason : 0;
        s_connected = false;
        s_internet_ready = false;
        s_current_ip[0] = '\0';
        s_last_disconnect_reason = reason;
        if (s_manual_disconnect) {
            if (s_wifi_event_group) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_DISCONNECTED_BIT);
            }
            return;
        }
        snprintf(s_last_error,
                 sizeof(s_last_error),
                 "Wi-Fi 断开 reason=%d：%s",
                 reason,
                 wifi_disconnect_reason_to_string(reason));

        if (!s_connecting) {
            if (s_wifi_event_group) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_DISCONNECTED_BIT);
            }
            return;
        }

        if (wifi_disconnect_reason_is_terminal(reason)) {
            ESP_LOGW(TAG, "Wi-Fi disconnected, terminal reason=%d: %s", reason, wifi_disconnect_reason_to_string(reason));
            s_connecting = false;
            if (s_wifi_event_group) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
            return;
        }

        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            s_retry_num++;
            ESP_LOGW(TAG,
                     "Wi-Fi disconnected, retry %d/%d, reason=%d: %s",
                     s_retry_num,
                     WIFI_MAXIMUM_RETRY,
                     reason,
                     wifi_disconnect_reason_to_string(reason));
            esp_err_t retry_err = esp_wifi_connect();
            if (retry_err != ESP_OK) {
                ESP_LOGW(TAG, "Wi-Fi retry connect skipped: %s", esp_err_to_name(retry_err));
            }
        } else if (s_wifi_event_group) {
            s_connecting = false;
            ESP_LOGW(TAG,
                     "Wi-Fi connect failed after retries, last reason=%d: %s",
                     reason,
                     wifi_disconnect_reason_to_string(reason));
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_current_ip, sizeof(s_current_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_connected = true;
        s_connecting = false;
        set_last_error("");
        ESP_LOGI(TAG, "got IP: %s", s_current_ip);
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        start_sntp_sync_task();
    }
}

static esp_err_t read_saved_config(fridge_wifi_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        s_saved = false;
        return err;
    }

    size_t ssid_len = sizeof(config->ssid);
    size_t password_len = sizeof(config->password);
    err = nvs_get_str(handle, WIFI_NVS_KEY_SSID, config->ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, WIFI_NVS_KEY_PASSWORD, config->password, &password_len);
    }
    nvs_close(handle);
    s_saved = (err == ESP_OK && config->ssid[0] != '\0');
    return s_saved ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t save_config(const fridge_wifi_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle), TAG, "open network NVS failed");
    esp_err_t err = nvs_set_str(handle, WIFI_NVS_KEY_SSID, config->ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, WIFI_NVS_KEY_PASSWORD, config->password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    s_saved = (err == ESP_OK);
    return err;
}

esp_err_t fridge_network_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_wifi_event_group = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_wifi_event_group, ESP_ERR_NO_MEM, TAG, "create event group failed");
    s_connect_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_connect_mutex, ESP_ERR_NO_MEM, TAG, "create Wi-Fi connect mutex failed");
    s_sntp_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_sntp_mutex, ESP_ERR_NO_MEM, TAG, "create SNTP mutex failed");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
    ESP_RETURN_ON_ERROR(ensure_event_loop(), TAG, "event loop create failed");

    s_sta_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(s_sta_netif, ESP_FAIL, TAG, "create default STA failed");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init failed");
    // USB JSON Lines 与 ESP-IDF 日志共用同一串口，降低 Wi-Fi 底层 info 噪声，避免日志插入协议响应。
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL),
                        TAG, "register Wi-Fi event failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL),
                        TAG, "register IP event failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set Wi-Fi storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set Wi-Fi STA mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start Wi-Fi failed");
    // 配网和 AI API 测试阶段优先保证连接稳定；关闭省电会增加 Wi-Fi 峰值电流，测试时需要稳定 USB/5V 供电。
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "disable Wi-Fi power save failed");

    fridge_wifi_config_t saved = {0};
    s_saved = (read_saved_config(&saved) == ESP_OK);
    s_initialized = true;
    ESP_LOGI(TAG, "network initialized, saved credential=%s", s_saved ? "yes" : "no");
    return ESP_OK;
}

esp_err_t fridge_network_scan(fridge_wifi_ap_t *aps, size_t max_count, size_t *out_count)
{
    ESP_RETURN_ON_FALSE(aps && out_count, ESP_ERR_INVALID_ARG, TAG, "invalid scan args");
    ESP_RETURN_ON_FALSE(max_count > 0, ESP_ERR_INVALID_ARG, TAG, "max_count is zero");
    ESP_RETURN_ON_ERROR(fridge_network_init(), TAG, "network init failed before scan");

    *out_count = 0;
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 120,
        .scan_time.active.max = 360,
    };

    ESP_LOGI(TAG, "starting Wi-Fi scan");
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        set_last_error("wifi scan failed");
        return err;
    }

    uint16_t ap_num = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_num(&ap_num), TAG, "get AP num failed");
    if (ap_num == 0) {
        ESP_LOGW(TAG, "no Wi-Fi AP found");
        return ESP_OK;
    }

    uint16_t record_count = (ap_num > max_count) ? (uint16_t)max_count : ap_num;
    wifi_ap_record_t *records = calloc(record_count, sizeof(wifi_ap_record_t));
    ESP_RETURN_ON_FALSE(records, ESP_ERR_NO_MEM, TAG, "allocate AP records failed");
    esp_err_t records_err = esp_wifi_scan_get_ap_records(&record_count, records);
    if (records_err != ESP_OK) {
        free(records);
        ESP_RETURN_ON_ERROR(records_err, TAG, "get AP records failed");
    }

    for (uint16_t i = 0; i < record_count && *out_count < max_count; i++) {
        if (records[i].ssid[0] == '\0') {
            continue;
        }

        bool merged = false;
        for (size_t j = 0; j < *out_count; j++) {
            if (strncmp(aps[j].ssid, (const char *)records[i].ssid, sizeof(aps[j].ssid)) == 0) {
                if (records[i].rssi > aps[j].rssi) {
                    aps[j].rssi = records[i].rssi;
                    aps[j].channel = records[i].primary;
                    aps[j].secured = records[i].authmode != WIFI_AUTH_OPEN;
                    strlcpy(aps[j].authmode, wifi_authmode_to_string(records[i].authmode), sizeof(aps[j].authmode));
                }
                merged = true;
                break;
            }
        }
        if (merged) {
            continue;
        }

        fridge_wifi_ap_t *ap = &aps[*out_count];
        strlcpy(ap->ssid, (const char *)records[i].ssid, sizeof(ap->ssid));
        ap->rssi = records[i].rssi;
        ap->channel = records[i].primary;
        ap->secured = records[i].authmode != WIFI_AUTH_OPEN;
        strlcpy(ap->authmode, wifi_authmode_to_string(records[i].authmode), sizeof(ap->authmode));
        (*out_count)++;
    }

    free(records);
    ESP_LOGI(TAG, "Wi-Fi scan done, visible AP count=%u", (unsigned)*out_count);
    return ESP_OK;
}

esp_err_t fridge_network_connect(const fridge_wifi_config_t *config, bool save)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_FALSE(config->ssid[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "SSID is empty");
    ESP_RETURN_ON_ERROR(fridge_network_init(), TAG, "network init failed before connect");
    ESP_RETURN_ON_FALSE(s_connect_mutex, ESP_ERR_INVALID_STATE, TAG, "connect mutex is NULL");

    if (xSemaphoreTake(s_connect_mutex, pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS + WIFI_DISCONNECT_TIMEOUT_MS + 2000)) != pdTRUE) {
        set_last_error("已有 Wi-Fi 连接请求正在进行，请稍后再试");
        return ESP_ERR_TIMEOUT;
    }

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | WIFI_DISCONNECTED_BIT);
    s_retry_num = 0;
    s_connected = false;
    s_connecting = false;
    s_internet_ready = false;
    s_current_ip[0] = '\0';
    s_last_disconnect_reason = 0;
    set_last_error("");
    strlcpy(s_current_ssid, config->ssid, sizeof(s_current_ssid));

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, config->ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, config->password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = config->password[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
#ifdef CONFIG_ESP_WIFI_WPA3_COMPATIBLE_SUPPORT
    wifi_config.sta.disable_wpa3_compatible_mode = 0;
#endif

    // 切换 SSID/密码前先主动断开旧连接，并临时禁止事件回调自动重连。
    // 注意：reason=36 表示 STA 主动离开，在这里属于预期流程，不应当算作连接失败。
    s_manual_disconnect = true;
    esp_err_t disconnect_err = esp_wifi_disconnect();
    if (disconnect_err != ESP_OK && disconnect_err != ESP_ERR_WIFI_NOT_CONNECT) {
        s_manual_disconnect = false;
        xSemaphoreGive(s_connect_mutex);
        ESP_RETURN_ON_ERROR(disconnect_err, TAG, "disconnect before connect failed");
    }
    if (disconnect_err == ESP_OK) {
        xEventGroupWaitBits(s_wifi_event_group,
                            WIFI_DISCONNECTED_BIT,
                            pdTRUE,
                            pdFALSE,
                            pdMS_TO_TICKS(WIFI_DISCONNECT_TIMEOUT_MS));
    }
    s_manual_disconnect = false;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        xSemaphoreGive(s_connect_mutex);
        ESP_RETURN_ON_ERROR(err, TAG, "set Wi-Fi config failed");
    }

    ESP_LOGI(TAG, "connecting to SSID: %s", config->ssid);
    s_connecting = true;
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        stop_wifi_connecting();
        xSemaphoreGive(s_connect_mutex);
        ESP_RETURN_ON_ERROR(err, TAG, "esp_wifi_connect failed");
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        wifi_ap_record_t ap_info = {0};
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            s_last_rssi = ap_info.rssi;
        }
        if (save) {
            err = save_config(config);
            if (err != ESP_OK) {
                xSemaphoreGive(s_connect_mutex);
                ESP_RETURN_ON_ERROR(err, TAG, "save Wi-Fi credential failed");
            }
        }
        xSemaphoreGive(s_connect_mutex);
        return ESP_OK;
    }

    if (bits & WIFI_FAIL_BIT) {
        char message[sizeof(s_last_error)] = {0};
        snprintf(message,
                 sizeof(message),
                 "Wi-Fi 连接失败 reason=%d：%s",
                 s_last_disconnect_reason,
                 wifi_disconnect_reason_to_string(s_last_disconnect_reason));
        set_last_error(message);
    } else {
        set_last_error("Wi-Fi 连接超时：请检查密码、热点是否为 2.4GHz、信号强度和开发板供电");
    }
    stop_wifi_connecting();
    xSemaphoreGive(s_connect_mutex);
    return ESP_ERR_TIMEOUT;
}

esp_err_t fridge_network_connect_saved(void)
{
    fridge_wifi_config_t config = {0};
    esp_err_t err = read_saved_config(&config);
    if (err != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    return fridge_network_connect(&config, false);
}

esp_err_t fridge_network_connect_saved_async(void)
{
    ESP_RETURN_ON_ERROR(fridge_network_init(), TAG, "network init failed before saved async connect");
    if (s_saved_connect_task) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(saved_connect_task,
                                "wifi_saved_conn",
                                WIFI_SAVED_CONNECT_TASK_STACK,
                                NULL,
                                3,
                                &s_saved_connect_task);
    if (ok != pdPASS) {
        s_saved_connect_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t fridge_network_sync_time(void)
{
    if (!s_connected) {
        set_last_error("SNTP skipped: Wi-Fi not connected");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_sntp_mutex && xSemaphoreTake(s_sntp_mutex, pdMS_TO_TICKS(25000)) != pdTRUE) {
        set_last_error("SNTP sync busy");
        return ESP_ERR_TIMEOUT;
    }

    const char *servers[] = {
        s_ntp_server,
        "time.google.com",
        "ntp.aliyun.com",
        "cn.pool.ntp.org",
    };
    esp_err_t last_err = ESP_ERR_TIMEOUT;

    for (size_t i = 0; i < sizeof(servers) / sizeof(servers[0]); i++) {
        const char *server = servers[i];
        if (!server || server[0] == '\0') {
            continue;
        }

        bool duplicate = false;
        for (size_t j = 0; j < i; j++) {
            if (servers[j] && strcmp(server, servers[j]) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }

        if (s_sntp_initialized) {
            esp_netif_sntp_deinit();
            s_sntp_initialized = false;
        }

        // 设备端 HTTPS 证书校验依赖系统时间；配网阶段用多个常见 NTP 服务器兜底，避免单个 UDP/NTP 节点不可达。
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(server);
        config.wait_for_sync = true;
        last_err = esp_netif_sntp_init(&config);
        if (last_err != ESP_OK) {
            ESP_LOGW(TAG, "SNTP init failed for %s: %s", server, esp_err_to_name(last_err));
            continue;
        }
        s_sntp_initialized = true;

        last_err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(5000));
        if (last_err == ESP_OK) {
            s_internet_ready = true;
            strlcpy(s_ntp_server, server, sizeof(s_ntp_server));
            set_last_error("");
            if (s_sntp_mutex) {
                xSemaphoreGive(s_sntp_mutex);
            }
            return ESP_OK;
        }
        ESP_LOGW(TAG, "SNTP sync failed for %s: %s", server, esp_err_to_name(last_err));
    }

    s_internet_ready = false;
    set_last_error("SNTP sync failed");
    if (s_sntp_mutex) {
        xSemaphoreGive(s_sntp_mutex);
    }
    return last_err;
}

esp_err_t fridge_network_get_status(fridge_network_status_t *status)
{
    ESP_RETURN_ON_FALSE(status, ESP_ERR_INVALID_ARG, TAG, "status is NULL");
    memset(status, 0, sizeof(*status));
    status->initialized = s_initialized;
    status->connected = s_connected;
    status->connecting = s_connecting;
    status->saved = s_saved;
    status->internet_ready = s_internet_ready;
    status->rssi = s_last_rssi;
    strlcpy(status->ssid, s_current_ssid, sizeof(status->ssid));
    strlcpy(status->ip, s_current_ip, sizeof(status->ip));
    strlcpy(status->ntp_server, s_ntp_server, sizeof(status->ntp_server));
    strlcpy(status->last_error, s_last_error, sizeof(status->last_error));

    if (s_connected) {
        wifi_ap_record_t ap_info = {0};
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            status->rssi = ap_info.rssi;
            strlcpy(status->ssid, (const char *)ap_info.ssid, sizeof(status->ssid));
            s_last_rssi = ap_info.rssi;
            strlcpy(s_current_ssid, status->ssid, sizeof(s_current_ssid));
        }
    }
    return ESP_OK;
}
