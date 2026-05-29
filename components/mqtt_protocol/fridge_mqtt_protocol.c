// 冰箱小精灵 MQTT 云端同步组件。
// 负责设备状态、低频传感快照、云端命令和回执；图片、音频和长 AI 文本不走 MQTT。
// 硬件注意：本组件只使用 Wi-Fi/TLS 网络栈，不操作 GPIO；Wi-Fi 发射峰值仍要求稳定 5V/USB 供电。

#include "fridge_mqtt_protocol.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fridge_diagnostics.h"
#include "fridge_network.h"
#include "fridge_sensors.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "nvs_flash.h"

#define MQTT_NVS_NAMESPACE "fridge_mqtt"
#define MQTT_NVS_KEY_URI "uri"
#define MQTT_NVS_KEY_HOME "home_id"
#define MQTT_NVS_KEY_DEVICE "device_id"
#define MQTT_NVS_KEY_USER "username"
#define MQTT_NVS_KEY_PASS "password"
#define MQTT_NVS_KEY_KEEPALIVE "keepalive"
#define MQTT_NVS_KEY_ENABLED "enabled"
#define MQTT_TOPIC_STATE "fridge/%s/%s/state"
#define MQTT_TOPIC_SENSOR "fridge/%s/%s/sensor"
#define MQTT_TOPIC_CMD "fridge/%s/%s/cmd"
#define MQTT_TOPIC_ACK "fridge/%s/%s/cmd_ack"
#define MQTT_TOPIC_ERROR "fridge/%s/%s/error"

static const char *TAG = "fridge_mqtt";

static esp_mqtt_client_handle_t s_client;
static bool s_initialized;
static bool s_connected;
static bool s_started;
static uint32_t s_reconnect_count;
static uint32_t s_published_count;
static uint32_t s_received_count;
static int s_last_error;
static char s_status_text[FRIDGE_MQTT_MAX_STATUS_LEN + 1] = "not initialized";
static fridge_mqtt_config_t s_config;

static void set_status_text(const char *text)
{
    strlcpy(s_status_text, text ? text : "", sizeof(s_status_text));
}

static bool config_is_complete(const fridge_mqtt_config_t *config)
{
    return config && config->enabled && config->broker_uri[0] && config->home_id[0] && config->device_id[0] && config->username[0] &&
           config->password[0];
}

static esp_err_t read_nvs_string(nvs_handle_t handle, const char *key, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    size_t len = out_size;
    esp_err_t err = nvs_get_str(handle, key, out, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    return err;
}

static esp_err_t load_config(fridge_mqtt_config_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    memset(out, 0, sizeof(*out));
    out->keepalive_seconds = FRIDGE_MQTT_DEFAULT_KEEPALIVE_SECONDS;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(MQTT_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open MQTT NVS failed");

    bool enabled = false;
    uint16_t keepalive = FRIDGE_MQTT_DEFAULT_KEEPALIVE_SECONDS;
    read_nvs_string(handle, MQTT_NVS_KEY_URI, out->broker_uri, sizeof(out->broker_uri));
    read_nvs_string(handle, MQTT_NVS_KEY_HOME, out->home_id, sizeof(out->home_id));
    read_nvs_string(handle, MQTT_NVS_KEY_DEVICE, out->device_id, sizeof(out->device_id));
    read_nvs_string(handle, MQTT_NVS_KEY_USER, out->username, sizeof(out->username));
    read_nvs_string(handle, MQTT_NVS_KEY_PASS, out->password, sizeof(out->password));
    if (nvs_get_u16(handle, MQTT_NVS_KEY_KEEPALIVE, &keepalive) == ESP_OK && keepalive > 0) {
        out->keepalive_seconds = keepalive;
    }
    if (nvs_get_u8(handle, MQTT_NVS_KEY_ENABLED, (uint8_t *)&enabled) != ESP_OK) {
        enabled = false;
    }
    out->enabled = enabled;
    out->use_tls = strncmp(out->broker_uri, "mqtts://", 8) == 0 || strncmp(out->broker_uri, "wss://", 6) == 0;
    nvs_close(handle);
    return ESP_OK;
}

static esp_err_t save_config(const fridge_mqtt_config_t *config, bool update_password)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(MQTT_NVS_NAMESPACE, NVS_READWRITE, &handle), TAG, "open MQTT NVS failed");
    esp_err_t err = nvs_set_str(handle, MQTT_NVS_KEY_URI, config->broker_uri);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, MQTT_NVS_KEY_HOME, config->home_id);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, MQTT_NVS_KEY_DEVICE, config->device_id);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, MQTT_NVS_KEY_USER, config->username);
    }
    if (err == ESP_OK && update_password) {
        err = nvs_set_str(handle, MQTT_NVS_KEY_PASS, config->password);
    }
    if (err == ESP_OK) {
        uint16_t keepalive = config->keepalive_seconds ? config->keepalive_seconds : FRIDGE_MQTT_DEFAULT_KEEPALIVE_SECONDS;
        err = nvs_set_u16(handle, MQTT_NVS_KEY_KEEPALIVE, keepalive);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, MQTT_NVS_KEY_ENABLED, config->enabled ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t make_topic(char *out, size_t out_size, const char *pattern)
{
    ESP_RETURN_ON_FALSE(out && pattern, ESP_ERR_INVALID_ARG, TAG, "invalid topic args");
    int written = snprintf(out, out_size, pattern, s_config.home_id, s_config.device_id);
    ESP_RETURN_ON_FALSE(written > 0 && (size_t)written < out_size, ESP_ERR_INVALID_SIZE, TAG, "topic too long");
    return ESP_OK;
}

static void add_common_fields(cJSON *root, const char *kind)
{
    cJSON_AddNumberToObject(root, "schema_version", 1);
    cJSON_AddStringToObject(root, "device_id", s_config.device_id);
    cJSON_AddStringToObject(root, "home_id", s_config.home_id);
    cJSON_AddStringToObject(root, "kind", kind ? kind : "event");
    cJSON_AddNumberToObject(root, "timestamp_ms", (double)(esp_timer_get_time() / 1000));
}

static esp_err_t publish_json(const char *topic_pattern, cJSON *root, int qos, bool retain)
{
    ESP_RETURN_ON_FALSE(root, ESP_ERR_INVALID_ARG, TAG, "json root is NULL");
    ESP_RETURN_ON_FALSE(s_client && s_connected, ESP_ERR_INVALID_STATE, TAG, "MQTT is not connected");

    char topic[FRIDGE_MQTT_MAX_TOPIC_LEN + 1];
    ESP_RETURN_ON_ERROR(make_topic(topic, sizeof(topic), topic_pattern), TAG, "make topic failed");
    char *payload = cJSON_PrintUnformatted(root);
    ESP_RETURN_ON_FALSE(payload, ESP_ERR_NO_MEM, TAG, "print MQTT JSON failed");

    int msg_id = esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain ? 1 : 0);
    free(payload);
    if (msg_id < 0) {
        set_status_text("publish failed");
        return ESP_FAIL;
    }
    s_published_count++;
    return ESP_OK;
}

static esp_err_t subscribe_command_topic(void)
{
    if (!s_client || !s_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    char topic[FRIDGE_MQTT_MAX_TOPIC_LEN + 1];
    ESP_RETURN_ON_ERROR(make_topic(topic, sizeof(topic), MQTT_TOPIC_CMD), TAG, "make cmd topic failed");
    int msg_id = esp_mqtt_client_subscribe(s_client, topic, 1);
    ESP_RETURN_ON_FALSE(msg_id >= 0, ESP_FAIL, TAG, "subscribe command topic failed");
    ESP_LOGI(TAG, "subscribed command topic, msg_id=%d", msg_id);
    return ESP_OK;
}

static void handle_command_payload(const char *payload, int payload_len)
{
    s_received_count++;
    cJSON *root = cJSON_ParseWithLength(payload, payload_len);
    if (!root) {
        fridge_mqtt_publish_command_ack("unknown", "invalid_json", false, "invalid command JSON");
        return;
    }

    const cJSON *request_id = cJSON_GetObjectItem(root, "request_id");
    const cJSON *command = cJSON_GetObjectItem(root, "command");
    const char *request_text = cJSON_IsString(request_id) ? request_id->valuestring : "unknown";
    const char *command_text = cJSON_IsString(command) ? command->valuestring : "unknown";

    // v1 先完成云端命令通道与回执闭环；具体 capture、ota_check 等命令后续接入各业务组件。
    ESP_LOGI(TAG, "received cloud command: %s request_id=%s", command_text, request_text);
    fridge_mqtt_publish_command_ack(request_text, command_text, true, "command received by device");
    cJSON_Delete(root);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        set_status_text("connected");
        ESP_LOGI(TAG, "MQTT connected");
        subscribe_command_topic();
        fridge_mqtt_publish_state(true);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        s_reconnect_count++;
        set_status_text("disconnected");
        ESP_LOGW(TAG, "MQTT disconnected");
        break;
    case MQTT_EVENT_DATA:
        handle_command_payload(event->data, event->data_len);
        break;
    case MQTT_EVENT_ERROR:
        s_last_error = event->error_handle ? event->error_handle->esp_tls_last_esp_err : ESP_FAIL;
        set_status_text("error");
        ESP_LOGW(TAG, "MQTT error, esp_tls_last_esp_err=%d", s_last_error);
        break;
    default:
        break;
    }
}

esp_err_t fridge_mqtt_protocol_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(load_config(&s_config), TAG, "load MQTT config failed");
    s_initialized = true;
    set_status_text(config_is_complete(&s_config) ? "configured" : "not configured");
    ESP_LOGI(TAG, "MQTT protocol initialized, enabled=%s configured=%s",
             s_config.enabled ? "yes" : "no",
             config_is_complete(&s_config) ? "yes" : "no");
    return ESP_OK;
}

esp_err_t fridge_mqtt_get_config(fridge_mqtt_config_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    ESP_RETURN_ON_ERROR(fridge_mqtt_protocol_init(), TAG, "MQTT init failed");
    *out = s_config;
    return ESP_OK;
}

esp_err_t fridge_mqtt_set_config(const fridge_mqtt_config_t *config, bool update_password)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_ERROR(fridge_mqtt_protocol_init(), TAG, "MQTT init failed");

    fridge_mqtt_config_t next = *config;
    if (!update_password) {
        strlcpy(next.password, s_config.password, sizeof(next.password));
    }
    if (next.keepalive_seconds == 0) {
        next.keepalive_seconds = FRIDGE_MQTT_DEFAULT_KEEPALIVE_SECONDS;
    }
    next.use_tls = strncmp(next.broker_uri, "mqtts://", 8) == 0 || strncmp(next.broker_uri, "wss://", 6) == 0;
    ESP_RETURN_ON_ERROR(save_config(&next, update_password), TAG, "save MQTT config failed");
    s_config = next;
    set_status_text(config_is_complete(&s_config) ? "configured" : "not configured");

    if (s_started) {
        fridge_mqtt_stop();
        if (config_is_complete(&s_config)) {
            return fridge_mqtt_start();
        }
    }
    return ESP_OK;
}

esp_err_t fridge_mqtt_clear_secret(void)
{
    ESP_RETURN_ON_ERROR(fridge_mqtt_protocol_init(), TAG, "MQTT init failed");
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(MQTT_NVS_NAMESPACE, NVS_READWRITE, &handle), TAG, "open MQTT NVS failed");
    esp_err_t err = nvs_erase_key(handle, MQTT_NVS_KEY_PASS);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    s_config.password[0] = '\0';
    set_status_text("secret cleared");
    return err;
}

esp_err_t fridge_mqtt_get_status(fridge_mqtt_status_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    ESP_RETURN_ON_ERROR(fridge_mqtt_protocol_init(), TAG, "MQTT init failed");
    memset(out, 0, sizeof(*out));
    out->initialized = s_initialized;
    out->enabled = s_config.enabled;
    out->configured = config_is_complete(&s_config);
    out->connected = s_connected;
    out->has_password = s_config.password[0] != '\0';
    out->reconnect_count = s_reconnect_count;
    out->published_count = s_published_count;
    out->received_count = s_received_count;
    out->last_error = s_last_error;
    strlcpy(out->broker_uri, s_config.broker_uri, sizeof(out->broker_uri));
    strlcpy(out->home_id, s_config.home_id, sizeof(out->home_id));
    strlcpy(out->device_id, s_config.device_id, sizeof(out->device_id));
    strlcpy(out->username, s_config.username, sizeof(out->username));
    strlcpy(out->status_text, s_status_text, sizeof(out->status_text));
    return ESP_OK;
}

esp_err_t fridge_mqtt_start(void)
{
    ESP_RETURN_ON_ERROR(fridge_mqtt_protocol_init(), TAG, "MQTT init failed");
    ESP_RETURN_ON_FALSE(config_is_complete(&s_config), ESP_ERR_INVALID_STATE, TAG, "MQTT config incomplete");

    fridge_network_status_t network = {0};
    if (fridge_network_get_status(&network) == ESP_OK && !network.connected) {
        set_status_text("waiting for Wi-Fi");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_client) {
        return ESP_OK;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_config.broker_uri,
        .credentials.username = s_config.username,
        .credentials.authentication.password = s_config.password,
        .session.keepalive = s_config.keepalive_seconds ? s_config.keepalive_seconds : FRIDGE_MQTT_DEFAULT_KEEPALIVE_SECONDS,
        .network.disable_auto_reconnect = false,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_RETURN_ON_FALSE(s_client, ESP_ERR_NO_MEM, TAG, "MQTT client init failed");
    ESP_RETURN_ON_ERROR(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL), TAG, "register MQTT event failed");
    ESP_RETURN_ON_ERROR(esp_mqtt_client_start(s_client), TAG, "start MQTT failed");
    s_started = true;
    set_status_text("connecting");
    return ESP_OK;
}

esp_err_t fridge_mqtt_stop(void)
{
    if (!s_client) {
        s_started = false;
        s_connected = false;
        return ESP_OK;
    }
    esp_err_t err = esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    s_started = false;
    s_connected = false;
    set_status_text("stopped");
    return err;
}

esp_err_t fridge_mqtt_publish_state(bool online)
{
    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root, ESP_ERR_NO_MEM, TAG, "create state JSON failed");
    add_common_fields(root, "state");

    fridge_device_status_t diag = {0};
    fridge_network_status_t net = {0};
    fridge_diagnostics_get_status(&diag);
    fridge_network_get_status(&net);

    cJSON_AddBoolToObject(root, "online", online);
    cJSON_AddStringToObject(root, "firmware", "s3-alpha.0.1.0");
    cJSON_AddStringToObject(root, "ip", net.ip);
    cJSON_AddNumberToObject(root, "rssi", net.rssi);
    cJSON_AddStringToObject(root, "wifi_status", net.connected ? "ok" : "offline");
    cJSON_AddStringToObject(root, "mqtt_status", s_connected ? "ok" : "offline");
    cJSON_AddNumberToObject(root, "free_heap_kb", diag.free_heap_kb);
    cJSON_AddNumberToObject(root, "free_psram_kb", diag.free_psram_kb);

    esp_err_t err = publish_json(MQTT_TOPIC_STATE, root, 1, true);
    cJSON_Delete(root);
    return err;
}

esp_err_t fridge_mqtt_publish_sensor_snapshot(void)
{
    fridge_sensor_snapshot_t sensor = {0};
    ESP_RETURN_ON_ERROR(fridge_sensors_get_snapshot(&sensor), TAG, "get sensor snapshot failed");

    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root, ESP_ERR_NO_MEM, TAG, "create sensor JSON failed");
    add_common_fields(root, "sensor");
    cJSON_AddBoolToObject(root, "presence", sensor.radar.stable_presence);
    cJSON_AddNumberToObject(root, "lux", sensor.light_value_10bit);
    cJSON_AddNumberToObject(root, "light_percent", sensor.light_percent);
    cJSON_AddNumberToObject(root, "light_delta", sensor.light_delta);
    cJSON_AddNumberToObject(root, "angle_delta", sensor.angle_delta);
    cJSON_AddNumberToObject(root, "vibration_peak", sensor.vibration_peak);
    cJSON_AddNumberToObject(root, "presence_distance_cm", sensor.radar.smoothed_distance_raw);

    esp_err_t err = publish_json(MQTT_TOPIC_SENSOR, root, 1, false);
    cJSON_Delete(root);
    return err;
}

esp_err_t fridge_mqtt_publish_command_ack(const char *request_id, const char *command, bool ok, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root, ESP_ERR_NO_MEM, TAG, "create ack JSON failed");
    add_common_fields(root, "cmd_ack");
    cJSON_AddStringToObject(root, "request_id", request_id ? request_id : "unknown");
    cJSON_AddStringToObject(root, "command", command ? command : "unknown");
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "message", message ? message : "");

    esp_err_t err = publish_json(MQTT_TOPIC_ACK, root, 1, false);
    cJSON_Delete(root);
    return err;
}
