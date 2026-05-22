// 冰箱小精灵本地存储抽象层。
// 负责给 AI 上下文提供库存、提醒、偏好、记忆摘要、短期会话历史和离线队列的统一读取接口。
// 硬件注意：结构化记忆摘要继续走 NVS；短期会话历史放在 cache LittleFS，避免高频写 NVS 造成额外磨损。
#include "fridge_storage.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#define STORAGE_NVS_NAMESPACE "fridge_store"
#define STORAGE_NVS_KEY_MEMORY "memory"
#define STORAGE_CACHE_PARTITION_LABEL "cache"
#define STORAGE_CACHE_BASE_PATH "/cache"
#define STORAGE_CHAT_HISTORY_PATH STORAGE_CACHE_BASE_PATH "/ai_history.json"
#define STORAGE_CHAT_HISTORY_SCHEMA_VERSION 1
#define STORAGE_CACHE_FORMAT_ON_FAIL true
#define STORAGE_HISTORY_FILE_BUFFER_SIZE 12288
#define STORAGE_EPOCH_READY_THRESHOLD 1735689600LL

static const char *TAG = "fridge_storage";

static bool s_initialized;
static uint32_t s_inventory_version = 1;

static const char *DEFAULT_MEMORY_JSON =
    "{\"schema_version\":1,"
    "\"memory_policy\":\"只保存结构化摘要，不保存完整聊天记录\","
    "\"family_size\":2,"
    "\"taste\":[\"清淡\",\"少油\"],"
    "\"avoid\":[\"不吃香菜\"],"
    "\"allergies\":[],"
    "\"recent_summary\":[\"用户希望优先处理临期食材\",\"早餐偏快手，晚餐偏家常\"]}";

static const char *INVENTORY_JSON =
    "{\"schema_version\":1,"
    "\"snapshot_version\":1,"
    "\"source\":\"seed_cache_until_littlefs_ready\","
    "\"items\":["
    "{\"item_id\":\"seed_tomato\",\"name\":\"番茄\",\"category\":\"蔬菜\",\"quantity\":\"3个\",\"expire_date\":\"2026-05-23\",\"days_left\":2,\"location\":\"冷藏主仓 B2\",\"confidence\":1.0,\"source\":\"user_confirmed\"},"
    "{\"item_id\":\"seed_egg\",\"name\":\"鸡蛋\",\"category\":\"蛋奶\",\"quantity\":\"6枚\",\"expire_date\":\"2026-05-28\",\"days_left\":7,\"location\":\"门架上层\",\"confidence\":1.0,\"source\":\"user_confirmed\"},"
    "{\"item_id\":\"seed_milk\",\"name\":\"牛奶\",\"category\":\"饮品\",\"quantity\":\"1盒\",\"expire_date\":\"2026-05-22\",\"days_left\":1,\"location\":\"门架中层\",\"confidence\":1.0,\"source\":\"user_confirmed\"}"
    "]}";

static const char *REMINDER_JSON =
    "{\"schema_version\":1,"
    "\"reminders\":["
    "{\"item_id\":\"seed_milk\",\"name\":\"牛奶\",\"days_left\":1,\"location\":\"门架中层\",\"suggestion\":\"今天或明天优先喝掉，若有异味请丢弃\"},"
    "{\"item_id\":\"seed_tomato\",\"name\":\"番茄\",\"days_left\":2,\"location\":\"冷藏主仓 B2\",\"suggestion\":\"可做番茄炒蛋或番茄汤\"}"
    "]}";

static const char *PREFERENCES_JSON =
    "{\"schema_version\":1,"
    "\"people\":2,"
    "\"taste\":[\"清淡\",\"少油\"],"
    "\"avoid\":[\"香菜\"],"
    "\"cooking_time_preference\":\"30分钟以内\","
    "\"kitchen_tools\":[\"电饭煲\",\"炒锅\",\"微波炉\"]}";

static const char *OFFLINE_QUEUE_JSON =
    "{\"schema_version\":1,"
    "\"pending_count\":0,"
    "\"policy\":\"离线时只保存业务事件和拍照任务状态，不长期保存图片原图\","
    "\"items\":[]}";

static esp_err_t copy_json(const char *source, char *out, size_t out_size)
{
    ESP_RETURN_ON_FALSE(source && out && out_size > 0, ESP_ERR_INVALID_ARG, TAG, "invalid json copy args");
    size_t len = strlen(source);
    ESP_RETURN_ON_FALSE(len + 1 <= out_size, ESP_ERR_NO_MEM, TAG, "json output buffer too small");
    strlcpy(out, source, out_size);
    return ESP_OK;
}

static esp_err_t open_storage_nvs(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "NVS handle is NULL");
    return nvs_open(STORAGE_NVS_NAMESPACE, mode, handle);
}

static bool storage_time_ready(void)
{
    time_t now = time(NULL);
    return now >= STORAGE_EPOCH_READY_THRESHOLD;
}

static int64_t storage_now_seconds(void)
{
    time_t now = time(NULL);
    return now > 0 ? (int64_t)now : 0;
}

static void storage_fill_default_history(fridge_storage_chat_history_t *history)
{
    if (!history) {
        return;
    }
    memset(history, 0, sizeof(*history));
    history->schema_version = STORAGE_CHAT_HISTORY_SCHEMA_VERSION;
    history->ttl_seconds = FRIDGE_STORAGE_CHAT_TTL_SECONDS;
    history->max_messages = FRIDGE_STORAGE_MAX_CHAT_MESSAGES;
    history->time_ready = storage_time_ready();
    history->updated_at = history->time_ready ? (uint32_t)storage_now_seconds() : 0;
}

static void storage_reset_message(fridge_storage_chat_message_t *message)
{
    if (!message) {
        return;
    }
    memset(message, 0, sizeof(*message));
}

static bool storage_role_allowed(const char *role)
{
    return role && (strcmp(role, "user") == 0 || strcmp(role, "assistant") == 0);
}

static bool storage_content_allowed(const char *content)
{
    return content && content[0] != '\0';
}

static size_t storage_utf8_safe_prefix_len(const char *text, size_t max_bytes)
{
    if (!text || max_bytes == 0) {
        return 0;
    }

    size_t i = 0;
    size_t last_good = 0;
    while (i < max_bytes && text[i] != '\0') {
        unsigned char ch = (unsigned char)text[i];
        size_t need = 0;
        if (ch < 0x80) {
            need = 1;
        } else if ((ch & 0xE0) == 0xC0) {
            need = 2;
        } else if ((ch & 0xF0) == 0xE0) {
            need = 3;
        } else if ((ch & 0xF8) == 0xF0) {
            need = 4;
        } else {
            break;
        }
        if (i + need > max_bytes) {
            break;
        }
        for (size_t j = 1; j < need; j++) {
            if (((unsigned char)text[i + j] & 0xC0) != 0x80) {
                return last_good;
            }
        }
        i += need;
        last_good = i;
    }
    return last_good;
}

static bool storage_utf8_is_valid(const char *text)
{
    if (!text) {
        return false;
    }
    size_t len = strlen(text);
    return storage_utf8_safe_prefix_len(text, len) == len;
}

static void storage_copy_utf8_safe(char *out, size_t out_size, const char *text)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!text) {
        return;
    }
    size_t copy_len = storage_utf8_safe_prefix_len(text, out_size - 1);
    memcpy(out, text, copy_len);
    out[copy_len] = '\0';
}

static size_t storage_prune_history_inplace(fridge_storage_chat_history_t *history, bool time_ready)
{
    if (!history) {
        return 0;
    }

    size_t write_index = 0;
    size_t pruned = 0;
    int64_t now = time_ready ? storage_now_seconds() : 0;
    int64_t cutoff = time_ready ? (now - FRIDGE_STORAGE_CHAT_TTL_SECONDS) : 0;

    for (size_t i = 0; i < history->count && i < FRIDGE_STORAGE_MAX_CHAT_MESSAGES; i++) {
        fridge_storage_chat_message_t *message = &history->messages[i];
        bool expired = time_ready && message->created_at > 0 && message->created_at < cutoff;
        bool invalid = !storage_role_allowed(message->role) ||
                       !storage_content_allowed(message->content) ||
                       !storage_utf8_is_valid(message->content);
        if (expired || invalid) {
            pruned++;
            continue;
        }
        if (write_index != i) {
            history->messages[write_index] = *message;
        }
        write_index++;
    }

    if (write_index > FRIDGE_STORAGE_MAX_CHAT_MESSAGES) {
        write_index = FRIDGE_STORAGE_MAX_CHAT_MESSAGES;
    }

    if (write_index > FRIDGE_STORAGE_MAX_CHAT_MESSAGES) {
        write_index = FRIDGE_STORAGE_MAX_CHAT_MESSAGES;
    }

    if (write_index > history->max_messages && history->max_messages > 0) {
        size_t drop_count = write_index - history->max_messages;
        memmove(history->messages,
                history->messages + drop_count,
                (write_index - drop_count) * sizeof(history->messages[0]));
        for (size_t i = write_index - drop_count; i < write_index; i++) {
            storage_reset_message(&history->messages[i]);
        }
        pruned += drop_count;
        write_index -= drop_count;
    }

    for (size_t i = write_index; i < FRIDGE_STORAGE_MAX_CHAT_MESSAGES; i++) {
        storage_reset_message(&history->messages[i]);
    }

    history->count = write_index;
    history->time_ready = time_ready;
    history->updated_at = time_ready ? (uint32_t)now : 0;
    history->schema_version = STORAGE_CHAT_HISTORY_SCHEMA_VERSION;
    history->ttl_seconds = FRIDGE_STORAGE_CHAT_TTL_SECONDS;
    history->max_messages = FRIDGE_STORAGE_MAX_CHAT_MESSAGES;
    return pruned;
}

static esp_err_t storage_mount_cache_fs(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = STORAGE_CACHE_BASE_PATH,
        .partition_label = STORAGE_CACHE_PARTITION_LABEL,
        .format_if_mount_failed = STORAGE_CACHE_FORMAT_ON_FAIL,
        .dont_mount = false,
        .grow_on_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    return err;
}

static esp_err_t storage_history_from_json(const char *json, fridge_storage_chat_history_t *history)
{
    ESP_RETURN_ON_FALSE(json && history, ESP_ERR_INVALID_ARG, TAG, "invalid history json args");
    storage_fill_default_history(history);

    cJSON *root = cJSON_Parse(json);
    ESP_RETURN_ON_FALSE(root, ESP_FAIL, TAG, "parse history json failed");

    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    const cJSON *updated_at = cJSON_GetObjectItemCaseSensitive(root, "updated_at");
    const cJSON *ttl_seconds = cJSON_GetObjectItemCaseSensitive(root, "ttl_seconds");
    const cJSON *max_messages = cJSON_GetObjectItemCaseSensitive(root, "max_messages");
    const cJSON *messages = cJSON_GetObjectItemCaseSensitive(root, "messages");

    if (cJSON_IsNumber(schema) && schema->valueint > 0) {
        history->schema_version = (uint32_t)schema->valueint;
    }
    if (cJSON_IsNumber(updated_at) && updated_at->valuedouble >= 0) {
        history->updated_at = (uint32_t)updated_at->valuedouble;
    }
    if (cJSON_IsNumber(ttl_seconds) && ttl_seconds->valueint > 0) {
        history->ttl_seconds = (uint32_t)ttl_seconds->valueint;
    }
    if (cJSON_IsNumber(max_messages) && max_messages->valueint > 0 && max_messages->valueint <= FRIDGE_STORAGE_MAX_CHAT_MESSAGES) {
        history->max_messages = (uint32_t)max_messages->valueint;
    }

    if (cJSON_IsArray(messages)) {
        size_t count = 0;
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, messages) {
            if (count >= FRIDGE_STORAGE_MAX_CHAT_MESSAGES) {
                break;
            }
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
            const cJSON *role = cJSON_GetObjectItemCaseSensitive(item, "role");
            const cJSON *content = cJSON_GetObjectItemCaseSensitive(item, "content");
            const cJSON *task_type = cJSON_GetObjectItemCaseSensitive(item, "task_type");
            const cJSON *created_at = cJSON_GetObjectItemCaseSensitive(item, "created_at");
            if (!cJSON_IsString(role) || !cJSON_IsString(content)) {
                continue;
            }

            fridge_storage_chat_message_t *message = &history->messages[count];
            storage_reset_message(message);
            if (cJSON_IsString(id) && id->valuestring) {
                strlcpy(message->id, id->valuestring, sizeof(message->id));
            }
            strlcpy(message->role, role->valuestring, sizeof(message->role));
            storage_copy_utf8_safe(message->content, sizeof(message->content), content->valuestring);
            if (cJSON_IsString(task_type) && task_type->valuestring) {
                strlcpy(message->task_type, task_type->valuestring, sizeof(message->task_type));
            }
            if (cJSON_IsNumber(created_at)) {
                message->created_at = (int64_t)created_at->valuedouble;
            }
            count++;
        }
        history->count = count;
    }

    history->time_ready = storage_time_ready();
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t storage_history_to_json(const fridge_storage_chat_history_t *history, char *out, size_t out_size)
{
    ESP_RETURN_ON_FALSE(history && out && out_size > 0, ESP_ERR_INVALID_ARG, TAG, "invalid history output args");

    esp_err_t ret = ESP_OK;
    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root, ESP_ERR_NO_MEM, TAG, "create history root failed");

    cJSON_AddNumberToObject(root, "schema_version", history->schema_version);
    cJSON_AddNumberToObject(root, "updated_at", history->updated_at);
    cJSON_AddNumberToObject(root, "ttl_seconds", history->ttl_seconds);
    cJSON_AddNumberToObject(root, "max_messages", history->max_messages);

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    ESP_GOTO_ON_FALSE(messages, ESP_ERR_NO_MEM, cleanup, TAG, "create history messages failed");

    for (size_t i = 0; i < history->count && i < FRIDGE_STORAGE_MAX_CHAT_MESSAGES; i++) {
        const fridge_storage_chat_message_t *message = &history->messages[i];
        cJSON *item = cJSON_CreateObject();
        ESP_GOTO_ON_FALSE(item, ESP_ERR_NO_MEM, cleanup, TAG, "create history item failed");
        cJSON_AddStringToObject(item, "id", message->id);
        cJSON_AddStringToObject(item, "role", message->role);
        cJSON_AddStringToObject(item, "content", message->content);
        cJSON_AddStringToObject(item, "task_type", message->task_type);
        cJSON_AddNumberToObject(item, "created_at", (double)message->created_at);
        cJSON_AddItemToArray(messages, item);
    }

    char *printed = cJSON_PrintUnformatted(root);
    ESP_GOTO_ON_FALSE(printed, ESP_ERR_NO_MEM, cleanup, TAG, "print history json failed");
    ESP_GOTO_ON_FALSE(strlen(printed) + 1 <= out_size, ESP_ERR_NO_MEM, cleanup_printed, TAG, "history json buffer too small");
    strlcpy(out, printed, out_size);
    cJSON_free(printed);
    cJSON_Delete(root);
    return ESP_OK;

cleanup_printed:
    cJSON_free(printed);
cleanup:
    cJSON_Delete(root);
    return ret;
}

static esp_err_t storage_read_history_file(fridge_storage_chat_history_t *history)
{
    ESP_RETURN_ON_FALSE(history, ESP_ERR_INVALID_ARG, TAG, "history is NULL");
    storage_fill_default_history(history);

    FILE *file = fopen(STORAGE_CHAT_HISTORY_PATH, "rb");
    if (!file) {
        return ESP_ERR_NOT_FOUND;
    }

    char *buffer = calloc(1, STORAGE_HISTORY_FILE_BUFFER_SIZE);
    if (!buffer) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    size_t bytes = fread(buffer, 1, STORAGE_HISTORY_FILE_BUFFER_SIZE - 1, file);
    fclose(file);
    buffer[bytes] = '\0';

    esp_err_t err = storage_history_from_json(buffer, history);
    free(buffer);
    return err;
}

static esp_err_t storage_write_history_file(const fridge_storage_chat_history_t *history)
{
    ESP_RETURN_ON_FALSE(history, ESP_ERR_INVALID_ARG, TAG, "history is NULL");

    char *buffer = calloc(1, STORAGE_HISTORY_FILE_BUFFER_SIZE);
    if (!buffer) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = storage_history_to_json(history, buffer, STORAGE_HISTORY_FILE_BUFFER_SIZE);
    if (err != ESP_OK) {
        free(buffer);
        return err;
    }

    FILE *file = fopen(STORAGE_CHAT_HISTORY_PATH, "wb");
    if (!file) {
        free(buffer);
        return ESP_FAIL;
    }

    size_t written = fwrite(buffer, 1, strlen(buffer), file);
    fclose(file);
    free(buffer);
    return (written > 0 || history->count == 0) ? ESP_OK : ESP_FAIL;
}

static esp_err_t storage_generate_message_id(char *out, size_t out_size)
{
    ESP_RETURN_ON_FALSE(out && out_size > 0, ESP_ERR_INVALID_ARG, TAG, "invalid message id args");
    int64_t now_us = esp_timer_get_time();
    snprintf(out, out_size, "local-%" PRId64, now_us);
    return ESP_OK;
}

static esp_err_t storage_prepare_message(const fridge_storage_chat_message_t *input, fridge_storage_chat_message_t *out)
{
    ESP_RETURN_ON_FALSE(input && out, ESP_ERR_INVALID_ARG, TAG, "invalid message args");
    ESP_RETURN_ON_FALSE(storage_role_allowed(input->role), ESP_ERR_INVALID_ARG, TAG, "chat message role invalid");
    ESP_RETURN_ON_FALSE(storage_content_allowed(input->content), ESP_ERR_INVALID_ARG, TAG, "chat message content invalid");

    *out = *input;
    storage_copy_utf8_safe(out->content, sizeof(out->content), input->content);
    if (out->id[0] == '\0') {
        ESP_RETURN_ON_ERROR(storage_generate_message_id(out->id, sizeof(out->id)), TAG, "generate chat message id failed");
    }
    if (out->created_at <= 0) {
        out->created_at = storage_now_seconds();
    }
    return ESP_OK;
}

esp_err_t fridge_storage_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    nvs_handle_t handle;
    esp_err_t err = open_storage_nvs(NVS_READWRITE, &handle);
    ESP_RETURN_ON_ERROR(err, TAG, "open storage NVS failed");

    size_t len = 0;
    err = nvs_get_str(handle, STORAGE_NVS_KEY_MEMORY, NULL, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_set_str(handle, STORAGE_NVS_KEY_MEMORY, DEFAULT_MEMORY_JSON);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
    }
    nvs_close(handle);
    ESP_RETURN_ON_ERROR(err, TAG, "init storage NVS failed");

    err = storage_mount_cache_fs();
    ESP_RETURN_ON_ERROR(err, TAG, "mount cache littlefs failed");

    fridge_storage_chat_history_t *history = calloc(1, sizeof(*history));
    ESP_RETURN_ON_FALSE(history, ESP_ERR_NO_MEM, TAG, "allocate init history failed");

    err = storage_read_history_file(history);
    if (err == ESP_ERR_NOT_FOUND) {
        storage_fill_default_history(history);
        err = storage_write_history_file(history);
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "chat history file invalid, recreating: %s", esp_err_to_name(err));
        storage_fill_default_history(history);
        err = storage_write_history_file(history);
    }
    free(history);
    ESP_RETURN_ON_ERROR(err, TAG, "init history file failed");

    s_initialized = true;
    ESP_LOGI(TAG, "storage facade initialized, inventory_version=%lu", (unsigned long)s_inventory_version);
    return ESP_OK;
}

esp_err_t fridge_storage_get_inventory_snapshot(char *out, size_t out_size)
{
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");
    return copy_json(INVENTORY_JSON, out, out_size);
}

esp_err_t fridge_storage_get_reminder_queue(char *out, size_t out_size)
{
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");
    return copy_json(REMINDER_JSON, out, out_size);
}

esp_err_t fridge_storage_get_user_preferences(char *out, size_t out_size)
{
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");
    return copy_json(PREFERENCES_JSON, out, out_size);
}

esp_err_t fridge_storage_get_memory_summary(char *out, size_t out_size)
{
    ESP_RETURN_ON_FALSE(out && out_size > 0, ESP_ERR_INVALID_ARG, TAG, "invalid memory output args");
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");

    nvs_handle_t handle;
    esp_err_t err = open_storage_nvs(NVS_READONLY, &handle);
    ESP_RETURN_ON_ERROR(err, TAG, "open storage NVS failed");

    size_t len = out_size;
    err = nvs_get_str(handle, STORAGE_NVS_KEY_MEMORY, out, &len);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return copy_json(DEFAULT_MEMORY_JSON, out, out_size);
    }
    return err;
}

esp_err_t fridge_storage_set_memory_summary(const char *memory_json)
{
    ESP_RETURN_ON_FALSE(memory_json && memory_json[0] == '{', ESP_ERR_INVALID_ARG, TAG, "memory summary must be a JSON object");
    ESP_RETURN_ON_FALSE(strlen(memory_json) < FRIDGE_STORAGE_MAX_MEMORY_LEN, ESP_ERR_INVALID_SIZE, TAG, "memory summary too large");
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(open_storage_nvs(NVS_READWRITE, &handle), TAG, "open storage NVS failed");
    // 结构化记忆只保存用户确认后的测试摘要，不能把完整聊天记录自动写入 Flash。
    esp_err_t err = nvs_set_str(handle, STORAGE_NVS_KEY_MEMORY, memory_json);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t fridge_storage_clear_memory_summary(void)
{
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(open_storage_nvs(NVS_READWRITE, &handle), TAG, "open storage NVS failed");
    esp_err_t err = nvs_set_str(handle,
                                STORAGE_NVS_KEY_MEMORY,
                                "{\"schema_version\":1,\"memory_policy\":\"已清空结构化记忆摘要，不保存完整聊天记录\",\"family_size\":0,\"taste\":[],\"avoid\":[],\"allergies\":[],\"recent_summary\":[]}");
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t fridge_storage_get_chat_history(fridge_storage_chat_history_t *out, size_t *pruned_count)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "chat history output is NULL");
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");

    fridge_storage_chat_history_t *history = calloc(1, sizeof(*history));
    ESP_RETURN_ON_FALSE(history, ESP_ERR_NO_MEM, TAG, "allocate chat history failed");

    esp_err_t err = storage_read_history_file(history);
    if (err == ESP_ERR_NOT_FOUND) {
        storage_fill_default_history(history);
        err = storage_write_history_file(history);
    }
    if (err != ESP_OK) {
        free(history);
        ESP_RETURN_ON_ERROR(err, TAG, "read chat history failed");
    }

    size_t pruned = storage_prune_history_inplace(history, storage_time_ready());
    if (pruned > 0) {
        err = storage_write_history_file(history);
        if (err != ESP_OK) {
            free(history);
            ESP_RETURN_ON_ERROR(err, TAG, "persist pruned chat history failed");
        }
    }

    *out = *history;
    free(history);
    if (pruned_count) {
        *pruned_count = pruned;
    }
    return ESP_OK;
}

esp_err_t fridge_storage_append_chat_messages(const fridge_storage_chat_message_t *messages,
                                              size_t message_count,
                                              size_t *pruned_count)
{
    ESP_RETURN_ON_FALSE(messages && message_count > 0, ESP_ERR_INVALID_ARG, TAG, "chat messages are invalid");
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");

    if (!storage_time_ready()) {
        if (pruned_count) {
            *pruned_count = 0;
        }
        return ESP_ERR_INVALID_STATE;
    }

    fridge_storage_chat_history_t *history = calloc(1, sizeof(*history));
    ESP_RETURN_ON_FALSE(history, ESP_ERR_NO_MEM, TAG, "allocate append history failed");

    size_t pruned = 0;
    esp_err_t err = fridge_storage_get_chat_history(history, &pruned);
    if (err != ESP_OK) {
        free(history);
        ESP_RETURN_ON_ERROR(err, TAG, "get chat history before append failed");
    }

    for (size_t i = 0; i < message_count; i++) {
        fridge_storage_chat_message_t next = {0};
        err = storage_prepare_message(&messages[i], &next);
        if (err != ESP_OK) {
            free(history);
            ESP_RETURN_ON_ERROR(err, TAG, "prepare chat message failed");
        }

        if (history->count < FRIDGE_STORAGE_MAX_CHAT_MESSAGES) {
            history->messages[history->count++] = next;
        } else {
            memmove(history->messages,
                    history->messages + 1,
                    (FRIDGE_STORAGE_MAX_CHAT_MESSAGES - 1) * sizeof(history->messages[0]));
            history->messages[FRIDGE_STORAGE_MAX_CHAT_MESSAGES - 1] = next;
            history->count = FRIDGE_STORAGE_MAX_CHAT_MESSAGES;
        }
    }

    pruned += storage_prune_history_inplace(history, true);
    history->updated_at = (uint32_t)storage_now_seconds();
    history->time_ready = true;
    err = storage_write_history_file(history);
    free(history);
    ESP_RETURN_ON_ERROR(err, TAG, "write chat history failed");

    if (pruned_count) {
        *pruned_count = pruned;
    }
    return ESP_OK;
}

esp_err_t fridge_storage_append_chat_message(const fridge_storage_chat_message_t *message, size_t *pruned_count)
{
    return fridge_storage_append_chat_messages(message, 1, pruned_count);
}

esp_err_t fridge_storage_clear_chat_history(void)
{
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");
    fridge_storage_chat_history_t *history = calloc(1, sizeof(*history));
    ESP_RETURN_ON_FALSE(history, ESP_ERR_NO_MEM, TAG, "allocate clear history failed");
    storage_fill_default_history(history);
    esp_err_t err = storage_write_history_file(history);
    free(history);
    return err;
}

esp_err_t fridge_storage_get_offline_queue_summary(char *out, size_t out_size)
{
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");
    return copy_json(OFFLINE_QUEUE_JSON, out, out_size);
}

esp_err_t fridge_storage_get_status(fridge_storage_status_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "storage status output is NULL");
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");

    memset(out, 0, sizeof(*out));
    out->cache_ready = true;
    out->assets_ready = false;
    out->inventory_version = s_inventory_version;
    strlcpy(out->cache_note,
            "当前 storage facade：NVS 保存结构化记忆摘要，cache LittleFS 保存 48 小时/15 轮（30 条）设备端短期会话历史。",
            sizeof(out->cache_note));
    return ESP_OK;
}
