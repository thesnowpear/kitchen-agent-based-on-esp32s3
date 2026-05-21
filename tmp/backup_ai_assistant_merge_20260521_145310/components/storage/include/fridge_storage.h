#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_STORAGE_MAX_JSON_LEN 4096
#define FRIDGE_STORAGE_MAX_MEMORY_LEN 1024
#define FRIDGE_STORAGE_MAX_STATUS_LEN 384

// 本地存储状态：首版先提供稳定接口，后续在该组件内替换为 LittleFS 文件读写。
// 注意：本组件只访问 Flash/NVS，不控制任何 GPIO；后续接入 LittleFS 时必须避免高频写入磨损 Flash。
typedef struct {
    bool cache_ready;
    bool assets_ready;
    uint32_t inventory_version;
    char cache_note[FRIDGE_STORAGE_MAX_STATUS_LEN];
} fridge_storage_status_t;

// 初始化本地存储抽象层。
// 当前版本使用内置种子数据 + NVS 小型记忆摘要，避免在 LittleFS 组件未接入前阻塞 AI 架构落地。
esp_err_t fridge_storage_init(void);

// 读取库存快照 JSON；包含少量演示食材、位置、保质期和版本号。
esp_err_t fridge_storage_get_inventory_snapshot(char *out, size_t out_size);

// 读取提醒队列 JSON；用于 AI 上下文和开门时本地提醒。
esp_err_t fridge_storage_get_reminder_queue(char *out, size_t out_size);

// 读取用户偏好 JSON；用于菜谱、购物清单和语音语义解析。
esp_err_t fridge_storage_get_user_preferences(char *out, size_t out_size);

// 读取结构化记忆摘要；不会保存完整聊天记录。
esp_err_t fridge_storage_get_memory_summary(char *out, size_t out_size);

// 清除结构化记忆摘要，保留库存、提醒和偏好。
esp_err_t fridge_storage_clear_memory_summary(void);

// 读取离线队列摘要 JSON；只保存任务状态和必要元数据，不保存图片原图。
esp_err_t fridge_storage_get_offline_queue_summary(char *out, size_t out_size);

// 读取本地存储状态。
esp_err_t fridge_storage_get_status(fridge_storage_status_t *out);

#ifdef __cplusplus
}
#endif
