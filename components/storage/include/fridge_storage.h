#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_STORAGE_MAX_JSON_LEN 8192
#define FRIDGE_STORAGE_MAX_MEMORY_LEN 1024
#define FRIDGE_STORAGE_MAX_STATUS_LEN 384
#define FRIDGE_STORAGE_MAX_CHAT_ROUNDS 15
#define FRIDGE_STORAGE_MAX_CHAT_MESSAGES (FRIDGE_STORAGE_MAX_CHAT_ROUNDS * 2)
#define FRIDGE_STORAGE_MAX_CHAT_ROLE_LEN 12
#define FRIDGE_STORAGE_MAX_CHAT_ID_LEN 24
#define FRIDGE_STORAGE_MAX_CHAT_CONTENT_LEN 768
#define FRIDGE_STORAGE_CHAT_TTL_SECONDS (48 * 60 * 60)

// 本地存储状态：生产数据和结构化记忆摘要都从 cache LittleFS 读取。
// 注意：本组件只访问 Flash/NVS，不控制任何 GPIO；写入必须由用户确认或业务事件触发，避免高频磨损 Flash。
typedef struct {
    bool cache_ready;
    bool assets_ready;
    uint32_t inventory_version;
    uint32_t inventory_server_revision;
    int64_t inventory_last_sync_at_ms;
    bool inventory_dirty;
    char cache_note[FRIDGE_STORAGE_MAX_STATUS_LEN];
} fridge_storage_status_t;

// UI 库存同步状态：用于 MQTT state 轻量上报和云端命令 ACK。
// 注意：这里只读取本地 JSON 元数据，不发布完整库存，避免 retained state 过大或泄露食材隐私。
typedef struct {
    uint32_t snapshot_version;
    uint32_t server_revision;
    int64_t updated_at_ms;
    int64_t last_sync_at_ms;
    bool dirty;
    char home_id[65];
    char device_id[65];
} fridge_storage_inventory_sync_status_t;

// 设备端会话历史消息：用于 48 小时内的多轮上下文注入，不等同于长期记忆摘要。
typedef struct {
    char id[FRIDGE_STORAGE_MAX_CHAT_ID_LEN];
    char role[FRIDGE_STORAGE_MAX_CHAT_ROLE_LEN];
    char content[FRIDGE_STORAGE_MAX_CHAT_CONTENT_LEN + 1];
    char task_type[32];
    int64_t created_at;
} fridge_storage_chat_message_t;

// 设备端会话历史快照：仅保存短期消息窗口，首版最多保留 15 轮（30 条消息）。
// 超时和超量会在读写前自动裁剪。
typedef struct {
    uint32_t schema_version;
    uint32_t updated_at;
    uint32_t ttl_seconds;
    uint32_t max_messages;
    bool time_ready;
    size_t count;
    fridge_storage_chat_message_t messages[FRIDGE_STORAGE_MAX_CHAT_MESSAGES];
} fridge_storage_chat_history_t;

// 初始化本地存储抽象层。
// 首次启动会把默认业务快照写入 cache LittleFS；已存在的本地文件不会被默认内容覆盖。
esp_err_t fridge_storage_init(void);

// 从 cache LittleFS 读取库存快照 JSON；文件缺失时才创建默认快照。
esp_err_t fridge_storage_get_inventory_snapshot(char *out, size_t out_size);

// 读取屏幕 UI 的本地库存快照 JSON；用于 LVGL 九宫格、编辑、拍照确认等本地交互。
esp_err_t fridge_storage_get_ui_inventory_snapshot(char *out, size_t out_size);

// 保存屏幕 UI 的本地库存快照 JSON。
// 注意：该接口会写入 Flash，UI 应只在用户明确保存/确认时调用，避免高频磨损。
esp_err_t fridge_storage_set_ui_inventory_snapshot(const char *inventory_json);

// 恢复出厂演示库存；用于联调时被空云端误覆盖后的受控恢复，恢复后会标记 dirty 等待上报。
esp_err_t fridge_storage_restore_seed_inventory(void);

// 读取 UI 库存同步状态；文件缺失时会先创建默认快照。
esp_err_t fridge_storage_get_inventory_sync_status(fridge_storage_inventory_sync_status_t *out);

// 生成可上报给云端的完整 UI 库存同步文档。
esp_err_t fridge_storage_get_inventory_sync_payload(char *out, size_t out_size);

// 应用云端下发的完整库存快照；server_revision 不更新时不会写 Flash。
esp_err_t fridge_storage_apply_inventory_cloud_snapshot(const char *inventory_json,
                                                        uint32_t server_revision,
                                                        const char *home_id,
                                                        const char *device_id,
                                                        bool *applied);

// 云端确认设备快照已落库后，标记本地库存为已同步。
esp_err_t fridge_storage_mark_inventory_synced(uint32_t server_revision);

// 从 cache LittleFS 读取提醒队列 JSON；用于 AI 上下文和开门时本地提醒。
esp_err_t fridge_storage_get_reminder_queue(char *out, size_t out_size);

// 写入提醒队列 JSON；用于云端同步提醒确认状态。
esp_err_t fridge_storage_set_reminder_queue(const char *reminder_json);

// 从 cache LittleFS 读取用户偏好 JSON；用于菜谱、购物清单和语音语义解析。
esp_err_t fridge_storage_get_user_preferences(char *out, size_t out_size);

// 写入用户偏好 JSON；用于云端同步隐私外的厨房偏好配置。
esp_err_t fridge_storage_set_user_preferences(const char *preferences_json);

// 读取购物清单 JSON；离线时也能展示最近一次云端/本地清单。
esp_err_t fridge_storage_get_shopping_list(char *out, size_t out_size);

// 写入购物清单 JSON；由 MQTT 同步命令或后续本地购物清单 UI 调用。
esp_err_t fridge_storage_set_shopping_list(const char *shopping_json);

// 读取 AI 菜谱缓存 JSON；避免断网时菜谱页完全不可用。
esp_err_t fridge_storage_get_recipe_cache(char *out, size_t out_size);

// 写入 AI 菜谱缓存 JSON；由 MQTT 同步命令或 AI 推荐确认后调用。
esp_err_t fridge_storage_set_recipe_cache(const char *recipe_json);

// 从 cache LittleFS 读取结构化记忆摘要；不会保存完整聊天记录。
esp_err_t fridge_storage_get_memory_summary(char *out, size_t out_size);

// 写入结构化记忆摘要；用于 Web/USB 调试或后续用户确认后的偏好同步。
// 注意：该接口会写入 cache LittleFS，Web 面板应由用户明确点击后再调用，避免高频磨损。
esp_err_t fridge_storage_set_memory_summary(const char *memory_json);

// 清除结构化记忆摘要，保留库存、提醒和偏好。
esp_err_t fridge_storage_clear_memory_summary(void);

// 执行 AI 回复末尾的 MEMORY_OP 隐藏指令；会从 assistant_text 中剥离指令后返回可展示文本。
esp_err_t fridge_storage_apply_memory_directive(const char *assistant_text, char *clean_reply, size_t clean_reply_size, bool *memory_updated);

// 读取设备端短期会话历史；返回前会自动删除过期消息并裁剪到最多 15 轮（30 条消息）。
esp_err_t fridge_storage_get_chat_history(fridge_storage_chat_history_t *out, size_t *pruned_count);

// 追加一条设备端短期会话历史消息；写入前会自动执行过期和数量裁剪。
esp_err_t fridge_storage_append_chat_message(const fridge_storage_chat_message_t *message, size_t *pruned_count);

// 原子追加多条设备端短期会话历史消息；用于一次性写入一轮 user/assistant 对话，避免半轮写入。
esp_err_t fridge_storage_append_chat_messages(const fridge_storage_chat_message_t *messages,
                                              size_t message_count,
                                              size_t *pruned_count);

// 清空设备端短期会话历史；不影响结构化记忆摘要。
esp_err_t fridge_storage_clear_chat_history(void);

// 从 cache LittleFS 读取离线队列摘要 JSON；只保存任务状态和必要元数据，不保存图片原图。
esp_err_t fridge_storage_get_offline_queue_summary(char *out, size_t out_size);

// 读取本地存储状态。
esp_err_t fridge_storage_get_status(fridge_storage_status_t *out);

#ifdef __cplusplus
}
#endif
