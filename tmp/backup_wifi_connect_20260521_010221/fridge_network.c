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
#include "nvs.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_DISCONNECTED_BIT BIT2
#define WIFI_CONNECT_TIMEOUT_MS 15000
#define WIFI_DISCONNECT_TIMEOUT_MS 3000
#define WIFI_MAXIMUM_RETRY 5
#define WIFI_NVS_NAMESPACE "fridge_net"
#define WIFI_NVS_KEY_SSID "ssid"
#define WIFI_NVS_KEY_PASSWORD "password"

static const char *TAG = "fridge_network";

static EventGroupHandle_t s_wifi_event_group;
static SemaphoreHandle_t s_connect_mutex;
static esp_netif_t *s_sta_netif;
static bool s_initialized;
static bool s_connected;
static bool s_connecting;
static bool s_manual_disconnect;
static bool s_saved;
static bool s_internet_ready;
static bool s_sntp_initialized;
static int s_retry_num;
static int8_t s_last_rssi;
static char s_current_ssid[FRIDGE_WIFI_MAX_SSID_LEN + 1];
static char s_current_ip[FRIDGE_WIFI_MAX_IP_LEN];
static char s_last_error[96];
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

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi STA started");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        s_connected = false;
        s_internet_ready = false;
        s_current_ip[0] = '\0';
        snprintf(s_last_error, sizeof(s_last_error), "disconnected reason=%d", event ? event->reason : -1);

        if (s_manual_disconnect || !s_connecting) {
            if (s_wifi_event_group) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_DISCONNECTED_BIT);
            }
            return;
        }

        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            s_retry_num++;
            ESP_LOGW(TAG, "Wi-Fi disconnected, retry %d/%d", s_retry_num, WIFI_MAXIMUM_RETRY);
            esp_err_t retry_err = esp_wifi_connect();
            if (retry_err != ESP_OK) {
                ESP_LOGW(TAG, "Wi-Fi retry connect skipped: %s", esp_err_to_name(retry_err));
            }
        } else if (s_wifi_event_group) {
            s_connecting = false;
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
    set_last_error("");
    strlcpy(s_current_ssid, config->ssid, sizeof(s_current_ssid));

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, config->ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, config->password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = config->password[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

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

    if (save) {
        err = save_config(config);
        if (err != ESP_OK) {
            xSemaphoreGive(s_connect_mutex);
            ESP_RETURN_ON_ERROR(err, TAG, "save Wi-Fi credential failed");
        }
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
        // SNTP 用于判断互联网是否可达；即使校时失败，也保留 Wi-Fi 已连接状态供前端诊断。
        esp_err_t sntp_err = fridge_network_sync_time();
        if (sntp_err != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi connected but SNTP sync failed: %s", esp_err_to_name(sntp_err));
        }
        xSemaphoreGive(s_connect_mutex);
        return ESP_OK;
    }

    set_last_error((bits & WIFI_FAIL_BIT) ? "connect failed, check SSID/password" : "connect timeout");
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

esp_err_t fridge_network_sync_time(void)
{
    if (!s_connected) {
        set_last_error("SNTP skipped: Wi-Fi not connected");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_sntp_initialized) {
        esp_netif_sntp_deinit();
        s_sntp_initialized = false;
    }

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_ntp_server);
    config.wait_for_sync = true;
    ESP_RETURN_ON_ERROR(esp_netif_sntp_init(&config), TAG, "SNTP init failed");
    s_sntp_initialized = true;

    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000));
    s_internet_ready = (err == ESP_OK);
    if (err != ESP_OK) {
        set_last_error("SNTP sync failed");
    }
    return err;
}

esp_err_t fridge_network_get_status(fridge_network_status_t *status)
{
    ESP_RETURN_ON_FALSE(status, ESP_ERR_INVALID_ARG, TAG, "status is NULL");
    memset(status, 0, sizeof(*status));
    status->initialized = s_initialized;
    status->connected = s_connected;
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
