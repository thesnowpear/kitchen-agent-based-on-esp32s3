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
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fridge_ai_client.h"
#include "fridge_asr.h"
#include "fridge_diagnostics.h"
#include "fridge_network.h"
#include "fridge_sensors.h"
#include "fridge_speaker.h"
#include "fridge_storage.h"
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
#define MQTT_TOPIC_INVENTORY "fridge/%s/%s/inventory"
#define MQTT_TOPIC_SYNC "fridge/%s/%s/sync"
#define MQTT_SYNC_CONFIG_JSON_LEN (FRIDGE_STORAGE_MAX_JSON_LEN * 2)
#define FRIDGE_MQTT_TASK_STACK_BYTES 4096
#define FRIDGE_MQTT_BUFFER_BYTES 1024
#define FRIDGE_MQTT_OUTBOX_LIMIT_BYTES 12288

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

static const cJSON *json_object_payload_or_root(const cJSON *root)
{
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    return cJSON_IsObject(payload) ? payload : root;
}

static const char *json_pick_string(const cJSON *root, const char *camel_key, const char *snake_key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, camel_key);
    if (!cJSON_IsString(item) && snake_key) {
        item = cJSON_GetObjectItemCaseSensitive(root, snake_key);
    }
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : NULL;
}

static int64_t json_pick_i64(const cJSON *root, const char *camel_key, const char *snake_key, int64_t fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, camel_key);
    if (!cJSON_IsNumber(item) && snake_key) {
        item = cJSON_GetObjectItemCaseSensitive(root, snake_key);
    }
    if (!cJSON_IsNumber(item) || item->valuedouble < 0) {
        return fallback;
    }
    return (int64_t)item->valuedouble;
}

static uint32_t json_pick_u32(const cJSON *root, const char *camel_key, const char *snake_key, uint32_t fallback)
{
    int64_t value = json_pick_i64(root, camel_key, snake_key, fallback);
    if (value > UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)value;
}

static void copy_json_string(char *out, size_t out_size, const cJSON *root, const char *camel_key, const char *snake_key)
{
    if (!out || out_size == 0) {
        return;
    }
    const char *value = json_pick_string(root, camel_key, snake_key);
    if (value) {
        strlcpy(out, value, out_size);
    }
}

esp_err_t fridge_mqtt_publish_inventory_snapshot(bool force_import)
{
    ESP_RETURN_ON_FALSE(s_client && s_connected, ESP_ERR_INVALID_STATE, TAG, "MQTT is not connected");

    char *payload = calloc(1, FRIDGE_STORAGE_MAX_JSON_LEN);
    ESP_RETURN_ON_FALSE(payload, ESP_ERR_NO_MEM, TAG, "allocate inventory payload failed");
    esp_err_t err = fridge_storage_get_inventory_sync_payload(payload, FRIDGE_STORAGE_MAX_JSON_LEN);
    if (err == ESP_OK) {
        cJSON *root = cJSON_Parse(payload);
        if (!root) {
            free(payload);
            return ESP_ERR_INVALID_ARG;
        }
        if (force_import) {
            cJSON_DeleteItemFromObjectCaseSensitive(root, "forceImport");
            cJSON_AddBoolToObject(root, "forceImport", true);
        }
        char *published = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!published) {
            free(payload);
            return ESP_ERR_NO_MEM;
        }

        char topic[FRIDGE_MQTT_MAX_TOPIC_LEN + 1];
        err = make_topic(topic, sizeof(topic), MQTT_TOPIC_INVENTORY);
        if (err == ESP_OK) {
            // 库存整快照可能接近 8KB，使用 QoS0 避免占满 MQTT outbox；失败时 dirty 会保留到下次重连。
            int msg_id = esp_mqtt_client_publish(s_client, topic, published, 0, 0, 0);
            if (msg_id < 0) {
                ESP_LOGW(TAG, "publish inventory snapshot failed len=%u force=%s", (unsigned)strlen(published), force_import ? "true" : "false");
                err = ESP_FAIL;
            } else {
                ESP_LOGI(TAG, "publish inventory snapshot queued msg_id=%d len=%u force=%s", msg_id, (unsigned)strlen(published), force_import ? "true" : "false");
                s_published_count++;
            }
        }
        cJSON_free(published);
    }
    free(payload);
    return err;
}

static esp_err_t apply_inventory_replace_payload(const cJSON *payload, uint32_t *local_revision, uint32_t *server_revision, bool *applied)
{
    const cJSON *inventory = cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(payload, "inventory"))
                                 ? cJSON_GetObjectItemCaseSensitive(payload, "inventory")
                                 : payload;
    uint32_t revision = json_pick_u32(payload, "serverRevision", "server_revision", 0);
    if (revision == 0 && inventory != payload) {
        revision = json_pick_u32(inventory, "serverRevision", "server_revision", 0);
    }
    char *printed = cJSON_PrintUnformatted(inventory);
    ESP_RETURN_ON_FALSE(printed, ESP_ERR_NO_MEM, TAG, "print inventory replace payload failed");
    esp_err_t err = fridge_storage_apply_inventory_cloud_snapshot(printed,
                                                                  revision,
                                                                  s_config.home_id,
                                                                  s_config.device_id,
                                                                  applied);
    cJSON_free(printed);
    fridge_storage_inventory_sync_status_t status = {0};
    if (fridge_storage_get_inventory_sync_status(&status) == ESP_OK) {
        if (local_revision) {
            *local_revision = status.snapshot_version;
        }
        if (server_revision) {
            *server_revision = status.server_revision;
        }
    }
    return err;
}

static esp_err_t apply_fridge_zones_payload(const cJSON *payload, uint32_t *local_revision, uint32_t *server_revision, bool *applied)
{
    const cJSON *zones = cJSON_GetObjectItemCaseSensitive(payload, "zones");
    ESP_RETURN_ON_FALSE(cJSON_IsArray(zones), ESP_ERR_INVALID_ARG, TAG, "fridge_zones_update missing zones");

    char *current = calloc(1, FRIDGE_STORAGE_MAX_JSON_LEN);
    ESP_RETURN_ON_FALSE(current, ESP_ERR_NO_MEM, TAG, "allocate current inventory failed");
    esp_err_t err = fridge_storage_get_inventory_sync_payload(current, FRIDGE_STORAGE_MAX_JSON_LEN);
    cJSON *root = err == ESP_OK ? cJSON_Parse(current) : NULL;
    free(current);
    ESP_RETURN_ON_FALSE(root, ESP_ERR_INVALID_ARG, TAG, "parse current inventory failed");

    cJSON_DeleteItemFromObjectCaseSensitive(root, "zones");
    cJSON_AddItemToObject(root, "zones", cJSON_Duplicate(zones, true));
    uint32_t revision = json_pick_u32(payload, "serverRevision", "server_revision", 0);
    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ESP_RETURN_ON_FALSE(printed, ESP_ERR_NO_MEM, TAG, "print zones inventory failed");
    err = fridge_storage_apply_inventory_cloud_snapshot(printed,
                                                        revision,
                                                        s_config.home_id,
                                                        s_config.device_id,
                                                        applied);
    cJSON_free(printed);
    fridge_storage_inventory_sync_status_t status = {0};
    if (fridge_storage_get_inventory_sync_status(&status) == ESP_OK) {
        if (local_revision) {
            *local_revision = status.snapshot_version;
        }
        if (server_revision) {
            *server_revision = status.server_revision;
        }
    }
    return err;
}

static esp_err_t apply_ai_config_payload(const cJSON *payload)
{
    fridge_ai_config_update_t config = {0};
    fridge_ai_config_view_t current = {0};
    (void)fridge_ai_client_get_config(&current);
    strlcpy(config.api_base_url, current.api_base_url, sizeof(config.api_base_url));
    strlcpy(config.model, current.model, sizeof(config.model));
    strlcpy(config.system_prompt, current.system_prompt, sizeof(config.system_prompt));
    strlcpy(config.profile_name, current.profile_name, sizeof(config.profile_name));
    config.timeout_ms = current.timeout_ms ? current.timeout_ms : FRIDGE_AI_DEFAULT_TIMEOUT_MS;
    copy_json_string(config.api_base_url, sizeof(config.api_base_url), payload, "apiBaseUrl", "api_base_url");
    copy_json_string(config.model, sizeof(config.model), payload, "chatModel", "model");
    copy_json_string(config.system_prompt, sizeof(config.system_prompt), payload, "systemPrompt", "system_prompt");
    copy_json_string(config.profile_name, sizeof(config.profile_name), payload, "profileName", "profile_name");
    config.timeout_ms = json_pick_u32(payload, "timeoutMs", "timeout_ms", config.timeout_ms);
    config.config_updated_at_ms = json_pick_i64(payload, "configUpdatedAt", "config_updated_at", 0);
    const char *api_key = json_pick_string(payload, "apiKey", "api_key");
    if (api_key) {
        strlcpy(config.api_key, api_key, sizeof(config.api_key));
        config.update_api_key = true;
    }
    return fridge_ai_client_set_config(&config);
}

static esp_err_t apply_asr_config_payload(const cJSON *payload)
{
    fridge_asr_config_update_t config = {0};
    fridge_asr_config_view_t current = {0};
    (void)fridge_asr_get_config(&current);
    strlcpy(config.api_base_url, current.api_base_url, sizeof(config.api_base_url));
    strlcpy(config.model, current.model, sizeof(config.model));
    config.timeout_ms = current.timeout_ms ? current.timeout_ms : FRIDGE_ASR_DEFAULT_TIMEOUT_MS;
    copy_json_string(config.api_base_url, sizeof(config.api_base_url), payload, "asrApiBaseUrl", "apiBaseUrl");
    copy_json_string(config.model, sizeof(config.model), payload, "asrModel", "model");
    config.timeout_ms = json_pick_u32(payload, "asrTimeoutMs", "timeoutMs", config.timeout_ms);
    config.config_updated_at_ms = json_pick_i64(payload, "configUpdatedAt", "config_updated_at", 0);
    const char *api_key = json_pick_string(payload, "asrApiKey", "apiKey");
    if (api_key) {
        strlcpy(config.api_key, api_key, sizeof(config.api_key));
        config.update_api_key = true;
    }
    return fridge_asr_set_config(&config);
}

static esp_err_t apply_tts_config_payload(const cJSON *payload)
{
    fridge_tts_config_update_t config = {0};
    fridge_tts_config_view_t current = {0};
    (void)fridge_tts_get_config(&current);
    strlcpy(config.api_base_url, current.api_base_url, sizeof(config.api_base_url));
    strlcpy(config.model, current.model, sizeof(config.model));
    strlcpy(config.voice, current.voice, sizeof(config.voice));
    config.timeout_ms = current.timeout_ms ? current.timeout_ms : FRIDGE_TTS_DEFAULT_TIMEOUT_MS;
    copy_json_string(config.api_base_url, sizeof(config.api_base_url), payload, "ttsApiBaseUrl", "apiBaseUrl");
    copy_json_string(config.model, sizeof(config.model), payload, "ttsModel", "model");
    copy_json_string(config.voice, sizeof(config.voice), payload, "ttsVoice", "voice");
    config.timeout_ms = json_pick_u32(payload, "ttsTimeoutMs", "timeoutMs", config.timeout_ms);
    config.config_updated_at_ms = json_pick_i64(payload, "configUpdatedAt", "config_updated_at", 0);
    const char *api_key = json_pick_string(payload, "ttsApiKey", "apiKey");
    if (api_key) {
        strlcpy(config.api_key, api_key, sizeof(config.api_key));
        config.update_api_key = true;
    }
    return fridge_tts_set_config(&config);
}

static esp_err_t apply_storage_document_payload(const cJSON *payload, esp_err_t (*setter)(const char *json_text))
{
    ESP_RETURN_ON_FALSE(payload && setter, ESP_ERR_INVALID_ARG, TAG, "invalid storage document args");
    char *printed = cJSON_PrintUnformatted(payload);
    ESP_RETURN_ON_FALSE(printed, ESP_ERR_NO_MEM, TAG, "print storage document payload failed");
    esp_err_t err = setter(printed);
    cJSON_free(printed);
    return err;
}

static bool add_storage_doc_event(cJSON *events,
                                  const char *domain,
                                  const char *op,
                                  const char *json_text,
                                  const char *client_suffix)
{
    cJSON *payload = json_text ? cJSON_Parse(json_text) : NULL;
    if (!payload || !cJSON_IsObject(payload)) {
        cJSON_Delete(payload);
        return false;
    }
    cJSON *event = cJSON_CreateObject();
    if (!event) {
        cJSON_Delete(payload);
        return false;
    }
    char event_id[160];
    snprintf(event_id,
             sizeof(event_id),
             "device:%s:%s:%" PRId64,
             s_config.device_id,
             client_suffix,
             esp_timer_get_time() / 1000);
    cJSON_AddStringToObject(event, "clientEventId", event_id);
    cJSON_AddStringToObject(event, "domain", domain);
    cJSON_AddStringToObject(event, "op", op);
    cJSON_AddStringToObject(event, "source", "device");
    cJSON_AddStringToObject(event, "deviceSn", s_config.device_id);
    cJSON_AddItemToObject(event, "payload", payload);
    cJSON_AddItemToArray(events, event);
    return true;
}

static bool merge_json_object_text(cJSON *target, const char *json_text)
{
    if (!target || !json_text || json_text[0] == '\0') {
        return false;
    }
    cJSON *source = cJSON_Parse(json_text);
    if (!source || !cJSON_IsObject(source)) {
        cJSON_Delete(source);
        return false;
    }
    bool ok = true;
    for (cJSON *item = source->child; item; item = item->next) {
        if (!item->string) {
            continue;
        }
        if (strcmp(item->string, "configUpdatedAt") == 0) {
            cJSON *current = cJSON_GetObjectItemCaseSensitive(target, item->string);
            if (cJSON_IsNumber(current) && cJSON_IsNumber(item)) {
                if (item->valuedouble > current->valuedouble) {
                    cJSON_SetNumberValue(current, item->valuedouble);
                }
                continue;
            }
        }
        cJSON *copy = cJSON_Duplicate(item, true);
        if (!copy) {
            ok = false;
            break;
        }
        if (cJSON_GetObjectItemCaseSensitive(target, item->string)) {
            cJSON_ReplaceItemInObjectCaseSensitive(target, item->string, copy);
        } else {
            cJSON_AddItemToObject(target, item->string, copy);
        }
    }
    cJSON_Delete(source);
    return ok;
}

static bool add_ai_config_sync_event(cJSON *events)
{
    char *ai_doc = calloc(1, MQTT_SYNC_CONFIG_JSON_LEN);
    char *aux_doc = calloc(1, 1024);
    if (!ai_doc || !aux_doc) {
        free(ai_doc);
        free(aux_doc);
        return false;
    }
    if (fridge_ai_client_get_sync_payload(ai_doc, MQTT_SYNC_CONFIG_JSON_LEN) != ESP_OK) {
        free(ai_doc);
        free(aux_doc);
        return false;
    }

    cJSON *payload = cJSON_Parse(ai_doc);
    if (!payload || !cJSON_IsObject(payload)) {
        cJSON_Delete(payload);
        free(ai_doc);
        free(aux_doc);
        return false;
    }
    if (fridge_asr_get_sync_payload(aux_doc, 1024) == ESP_OK) {
        (void)merge_json_object_text(payload, aux_doc);
    }
    memset(aux_doc, 0, 1024);
    if (fridge_tts_get_sync_payload(aux_doc, 1024) == ESP_OK) {
        (void)merge_json_object_text(payload, aux_doc);
    }

    char *printed = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    free(ai_doc);
    free(aux_doc);
    if (!printed) {
        return false;
    }
    bool ok = add_storage_doc_event(events, "ai_config", "replace", printed, "ai_config");
    cJSON_free(printed);
    return ok;
}

static esp_err_t publish_device_sync_documents(void)
{
    ESP_RETURN_ON_FALSE(s_client && s_connected, ESP_ERR_INVALID_STATE, TAG, "MQTT is not connected");

    esp_err_t ret = ESP_OK;
    char *doc = NULL;
    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root, ESP_ERR_NO_MEM, TAG, "create device sync root failed");
    add_common_fields(root, "sync");
    cJSON *events = cJSON_AddArrayToObject(root, "events");
    ESP_GOTO_ON_FALSE(events, ESP_ERR_NO_MEM, cleanup, TAG, "create sync events failed");

    doc = calloc(1, FRIDGE_STORAGE_MAX_JSON_LEN);
    ESP_GOTO_ON_FALSE(doc, ESP_ERR_NO_MEM, cleanup, TAG, "allocate sync doc failed");

    if (fridge_storage_get_shopping_list(doc, FRIDGE_STORAGE_MAX_JSON_LEN) == ESP_OK) {
        (void)add_storage_doc_event(events, "shopping_list", "replace", doc, "shopping");
    }
    memset(doc, 0, FRIDGE_STORAGE_MAX_JSON_LEN);
    if (fridge_storage_get_recipe_cache(doc, FRIDGE_STORAGE_MAX_JSON_LEN) == ESP_OK) {
        (void)add_storage_doc_event(events, "recipe_cache", "replace", doc, "recipe");
    }
    memset(doc, 0, FRIDGE_STORAGE_MAX_JSON_LEN);
    if (fridge_storage_get_reminder_queue(doc, FRIDGE_STORAGE_MAX_JSON_LEN) == ESP_OK) {
        (void)add_storage_doc_event(events, "reminder", "replace", doc, "reminder");
    }
    memset(doc, 0, FRIDGE_STORAGE_MAX_JSON_LEN);
    if (fridge_storage_get_user_preferences(doc, FRIDGE_STORAGE_MAX_JSON_LEN) == ESP_OK) {
        (void)add_storage_doc_event(events, "settings", "replace", doc, "settings");
    }
    (void)add_ai_config_sync_event(events);
    free(doc);
    doc = NULL;

    char *payload = cJSON_PrintUnformatted(root);
    ESP_GOTO_ON_FALSE(payload, ESP_ERR_NO_MEM, cleanup, TAG, "print device sync docs failed");
    char topic[FRIDGE_MQTT_MAX_TOPIC_LEN + 1];
    esp_err_t err = make_topic(topic, sizeof(topic), MQTT_TOPIC_SYNC);
    if (err == ESP_OK) {
        // 同步文档可能包含 AI/ASR/TTS Key 和本地缓存，首版比赛链路优先在线立即发送。
        // 使用 QoS0 可以避免较大的配置文档占满 MQTT outbox；失败时下次重连还会再次上报。
        int msg_id = esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 0);
        err = msg_id >= 0 ? ESP_OK : ESP_FAIL;
        if (err == ESP_OK) {
            s_published_count++;
        }
    }
    cJSON_free(payload);
    cJSON_Delete(root);
    return err;

cleanup:
    free(doc);
    cJSON_Delete(root);
    return ret == ESP_OK ? ESP_FAIL : ret;
}

static void publish_reconnect_sync_seed(void)
{
    fridge_storage_inventory_sync_status_t inventory_sync = {0};
    if (fridge_storage_get_inventory_sync_status(&inventory_sync) == ESP_OK) {
        // 首次接入云端时，本地库存可能已经存在但未标脏；必须允许设备快照作为云端种子导入。
        bool force_import = (inventory_sync.server_revision == 0 || inventory_sync.last_sync_at_ms == 0);
        if (inventory_sync.dirty || force_import) {
            ESP_LOGI(TAG,
                     "publish inventory seed on reconnect: dirty=%s server_rev=%" PRIu32 " last_sync=%lld force=%s",
                     inventory_sync.dirty ? "true" : "false",
                     inventory_sync.server_revision,
                     (long long)inventory_sync.last_sync_at_ms,
                     force_import ? "true" : "false");
            esp_err_t inv_err = fridge_mqtt_publish_inventory_snapshot(force_import);
            if (inv_err != ESP_OK) {
                ESP_LOGW(TAG, "publish inventory seed on reconnect failed: %s", esp_err_to_name(inv_err));
            }
        }
    } else {
        esp_err_t inv_err = fridge_mqtt_publish_inventory_snapshot(true);
        if (inv_err != ESP_OK) {
            ESP_LOGW(TAG, "publish default inventory seed on reconnect failed: %s", esp_err_to_name(inv_err));
        }
    }
    esp_err_t docs_err = publish_device_sync_documents();
    if (docs_err != ESP_OK) {
        ESP_LOGW(TAG, "publish sync documents on reconnect failed: %s", esp_err_to_name(docs_err));
    }
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
        fridge_mqtt_publish_command_ack_ex("unknown", "invalid_json", "completed", false, "invalid_json", 0, 0, false, "invalid command JSON");
        return;
    }

    const cJSON *request_id = cJSON_GetObjectItem(root, "request_id");
    const cJSON *command = cJSON_GetObjectItem(root, "command");
    const char *request_text = cJSON_IsString(request_id) ? request_id->valuestring : "unknown";
    const char *command_text = cJSON_IsString(command) ? command->valuestring : "unknown";

    // 云端命令采用 received + completed 两阶段 ACK，便于后端区分「已投递」和「设备已写入本地」。
    ESP_LOGI(TAG, "received cloud command: %s request_id=%s", command_text, request_text);
    fridge_mqtt_publish_command_ack_ex(request_text, command_text, "received", true, NULL, 0, 0, false, "command received by device");

    const cJSON *command_payload = json_object_payload_or_root(root);
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    const char *error_code = "not_supported";
    bool applied = false;
    uint32_t local_revision = 0;
    uint32_t server_revision = 0;

    if (strcmp(command_text, "inventory_refresh") == 0) {
        bool force_import = false;
        const cJSON *force_import_json = cJSON_GetObjectItemCaseSensitive(command_payload, "acceptCleanSnapshot");
        if (!cJSON_IsBool(force_import_json)) {
            force_import_json = cJSON_GetObjectItemCaseSensitive(command_payload, "forceImport");
        }
        force_import = cJSON_IsBool(force_import_json) && cJSON_IsTrue(force_import_json);
        err = fridge_mqtt_publish_inventory_snapshot(force_import);
        error_code = "publish_inventory_failed";
        fridge_storage_inventory_sync_status_t status = {0};
        if (fridge_storage_get_inventory_sync_status(&status) == ESP_OK) {
            local_revision = status.snapshot_version;
            server_revision = status.server_revision;
        }
    } else if (strcmp(command_text, "inventory_replace") == 0) {
        err = apply_inventory_replace_payload(command_payload, &local_revision, &server_revision, &applied);
        error_code = "inventory_replace_failed";
    } else if (strcmp(command_text, "sync_documents_refresh") == 0) {
        err = publish_device_sync_documents();
        error_code = "sync_documents_refresh_failed";
    } else if (strcmp(command_text, "fridge_zones_update") == 0) {
        err = apply_fridge_zones_payload(command_payload, &local_revision, &server_revision, &applied);
        error_code = "fridge_zones_update_failed";
    } else if (strcmp(command_text, "ai_config_update") == 0) {
        err = apply_ai_config_payload(command_payload);
        error_code = "ai_config_update_failed";
    } else if (strcmp(command_text, "asr_config_update") == 0) {
        err = apply_asr_config_payload(command_payload);
        error_code = "asr_config_update_failed";
    } else if (strcmp(command_text, "tts_config_update") == 0) {
        err = apply_tts_config_payload(command_payload);
        error_code = "tts_config_update_failed";
    } else if (strcmp(command_text, "shopping_list_update") == 0) {
        err = apply_storage_document_payload(command_payload, fridge_storage_set_shopping_list);
        error_code = "shopping_list_update_failed";
        applied = err == ESP_OK;
    } else if (strcmp(command_text, "recipe_cache_update") == 0) {
        err = apply_storage_document_payload(command_payload, fridge_storage_set_recipe_cache);
        error_code = "recipe_cache_update_failed";
        applied = err == ESP_OK;
    } else if (strcmp(command_text, "reminder_update") == 0) {
        err = apply_storage_document_payload(command_payload, fridge_storage_set_reminder_queue);
        error_code = "reminder_update_failed";
        applied = err == ESP_OK;
    } else if (strcmp(command_text, "preferences_update") == 0) {
        err = apply_storage_document_payload(command_payload, fridge_storage_set_user_preferences);
        error_code = "preferences_update_failed";
        applied = err == ESP_OK;
    }

    fridge_mqtt_publish_command_ack_ex(request_text,
                                       command_text,
                                       "completed",
                                       err == ESP_OK,
                                       err == ESP_OK ? NULL : error_code,
                                       local_revision,
                                       server_revision,
                                       applied,
                                       err == ESP_OK ? "command completed" : esp_err_to_name(err));
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
        publish_reconnect_sync_seed();
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

    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG,
             "starting MQTT, internal=%u KB largest=%u KB psram=%u KB",
             (unsigned)(free_internal / 1024),
             (unsigned)(largest_internal / 1024),
             (unsigned)(free_psram / 1024));

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_config.broker_uri,
        .credentials.username = s_config.username,
        .credentials.authentication.password = s_config.password,
        .session.keepalive = s_config.keepalive_seconds ? s_config.keepalive_seconds : FRIDGE_MQTT_DEFAULT_KEEPALIVE_SECONDS,
        .network.disable_auto_reconnect = false,
        .task.stack_size = FRIDGE_MQTT_TASK_STACK_BYTES,
        .buffer.size = FRIDGE_MQTT_BUFFER_BYTES,
        .buffer.out_size = FRIDGE_MQTT_BUFFER_BYTES,
        .outbox.limit = FRIDGE_MQTT_OUTBOX_LIMIT_BYTES,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_RETURN_ON_FALSE(s_client, ESP_ERR_NO_MEM, TAG, "MQTT client init failed");
    esp_err_t err = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        ESP_RETURN_ON_ERROR(err, TAG, "register MQTT event failed");
    }
    err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        // 创建 MQTT 任务失败时必须释放半初始化 client，否则下一次自动重试会误判已经启动。
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        s_started = false;
        s_connected = false;
        set_status_text("start failed");
        ESP_RETURN_ON_ERROR(err, TAG, "start MQTT failed");
    }
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
    fridge_storage_inventory_sync_status_t inventory_sync = {0};
    if (fridge_storage_get_inventory_sync_status(&inventory_sync) == ESP_OK) {
        cJSON_AddNumberToObject(root, "inventory_revision", inventory_sync.snapshot_version);
        cJSON_AddNumberToObject(root, "server_revision", inventory_sync.server_revision);
        cJSON_AddBoolToObject(root, "inventory_dirty", inventory_sync.dirty);
        cJSON_AddNumberToObject(root, "last_sync_at_ms", (double)inventory_sync.last_sync_at_ms);
    }

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
    return fridge_mqtt_publish_command_ack_ex(request_id,
                                             command,
                                             "completed",
                                             ok,
                                             ok ? NULL : "command_failed",
                                             0,
                                             0,
                                             false,
                                             message);
}

esp_err_t fridge_mqtt_publish_command_ack_ex(const char *request_id,
                                             const char *command,
                                             const char *stage,
                                             bool ok,
                                             const char *error_code,
                                             uint32_t local_revision,
                                             uint32_t server_revision,
                                             bool applied,
                                             const char *message)
{
    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root, ESP_ERR_NO_MEM, TAG, "create ack JSON failed");
    add_common_fields(root, "cmd_ack");
    cJSON_AddStringToObject(root, "request_id", request_id ? request_id : "unknown");
    cJSON_AddStringToObject(root, "command", command ? command : "unknown");
    cJSON_AddStringToObject(root, "stage", stage ? stage : "completed");
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "error_code", error_code ? error_code : "");
    cJSON_AddNumberToObject(root, "local_revision", local_revision);
    cJSON_AddNumberToObject(root, "server_revision", server_revision);
    cJSON_AddBoolToObject(root, "applied", applied);
    cJSON_AddStringToObject(root, "message", message ? message : "");

    esp_err_t err = publish_json(MQTT_TOPIC_ACK, root, 1, false);
    cJSON_Delete(root);
    return err;
}
