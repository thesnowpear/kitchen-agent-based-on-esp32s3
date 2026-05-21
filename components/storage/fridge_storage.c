// 冰箱小精灵本地存储抽象层。
// 负责给 AI 上下文提供库存、提醒、偏好、记忆摘要和离线队列的统一读取接口。
// 硬件注意：当前版本不挂载 LittleFS，只使用只读种子数据和少量 NVS 记忆摘要；后续接入 cache 分区时需控制 Flash 写入频率。

#include "fridge_storage.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"

#define STORAGE_NVS_NAMESPACE "fridge_store"
#define STORAGE_NVS_KEY_MEMORY "memory"

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
    "{\"item_id\":\"seed_milk\",\"name\":\"牛奶\",\"days_left\":1,\"location\":\"门架中层\",\"suggestion\":\"今天或明早优先喝掉，若有异味请丢弃\"},"
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
    // 结构化记忆只保存用户确认的测试摘要，不能把完整聊天记录自动写入 Flash。
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
            "当前为 storage facade：NVS 保存小型记忆摘要，库存/提醒使用种子 JSON；后续在本组件内接入 LittleFS cache/assets 分区。",
            sizeof(out->cache_note));
    return ESP_OK;
}
