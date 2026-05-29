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

// 本地存储状态：首版先提供稳定接口，后续在该组件内替换为 LittleFS 文件读写。
// 注意：本组件只访问 Flash/NVS，不控制任何 GPIO；后续接入 LittleFS 时必须避免高频写入磨损 Flash。
typedef struct {
    bool cache_ready;
    bool assets_ready;
    uint32_t inventory_version;
    char cache_note[FRIDGE_STORAGE_MAX_STATUS_LEN];
} fridge_storage_status_t;

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
// 当前版本使用内置种子数据 + NVS 小型记忆摘要，避免在 LittleFS 组件未接入前阻塞 AI 架构落地。
esp_err_t fridge_storage_init(void);

// 读取库存快照 JSON；包含少量演示食材、位置、保质期和版本号。
esp_err_t fridge_storage_get_inventory_snapshot(char *out, size_t out_size);

// 读取屏幕 UI 的本地库存快照 JSON；用于 LVGL 九宫格、编辑、拍照确认等本地交互。
esp_err_t fridge_storage_get_ui_inventory_snapshot(char *out, size_t out_size);

// 保存屏幕 UI 的本地库存快照 JSON。
// 注意：该接口会写入 Flash，UI 应只在用户明确保存/确认时调用，避免高频磨损。
esp_err_t fridge_storage_set_ui_inventory_snapshot(const char *inventory_json);

// 读取提醒队列 JSON；用于 AI 上下文和开门时本地提醒。
esp_err_t fridge_storage_get_reminder_queue(char *out, size_t out_size);

// 读取用户偏好 JSON；用于菜谱、购物清单和语音语义解析。
esp_err_t fridge_storage_get_user_preferences(char *out, size_t out_size);

// 读取结构化记忆摘要；不会保存完整聊天记录。
esp_err_t fridge_storage_get_memory_summary(char *out, size_t out_size);

// 写入结构化测试记忆摘要；用于硬件链路测试，不自动保存完整聊天记录。
// 注意：该接口会写入 NVS Flash，Web 面板应由用户明确点击后再调用，避免高频磨损。
esp_err_t fridge_storage_set_memory_summary(const char *memory_json);

// 清除结构化记忆摘要，保留库存、提醒和偏好。
esp_err_t fridge_storage_clear_memory_summary(void);

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

// 读取离线队列摘要 JSON；只保存任务状态和必要元数据，不保存图片原图。
esp_err_t fridge_storage_get_offline_queue_summary(char *out, size_t out_size);

// 读取本地存储状态。
esp_err_t fridge_storage_get_status(fridge_storage_status_t *out);

#ifdef __cplusplus
}
#endif
