#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_AI_CONTEXT_MAX_TASK_LEN 32
#define FRIDGE_AI_CONTEXT_MAX_TEXT_LEN 512
#define FRIDGE_AI_CONTEXT_MAX_JSON_LEN 8192
#define FRIDGE_AI_CONTEXT_MAX_RESULT_LEN 4096

// AI 任务请求：把用户输入、任务类型和本地上下文解耦，避免直接把所有数据塞进 systemPrompt。
typedef struct {
    char task_type[FRIDGE_AI_CONTEXT_MAX_TASK_LEN + 1];
    char user_text[FRIDGE_AI_CONTEXT_MAX_TEXT_LEN + 1];
    bool include_inventory;
    bool include_memory;
    bool include_reminders;
    bool include_preferences;
} fridge_ai_task_request_t;

// AI 上下文预览：供 Web 面板检查本次任务会注入哪些数据。
typedef struct {
    char task_type[FRIDGE_AI_CONTEXT_MAX_TASK_LEN + 1];
    char preview_json[FRIDGE_AI_CONTEXT_MAX_JSON_LEN + 1];
    uint32_t local_snapshot_version;
    bool needs_confirmation;
} fridge_ai_context_preview_t;

// 项目 AI Mock 任务结果：模拟云端 AI Adapter 返回的结构化结果，后续可替换为 HTTPS/MQTT 真实链路。
typedef struct {
    char task_type[FRIDGE_AI_CONTEXT_MAX_TASK_LEN + 1];
    char result_json[FRIDGE_AI_CONTEXT_MAX_RESULT_LEN + 1];
    uint8_t confidence_percent;
    bool needs_confirmation;
    char safety_note[192];
} fridge_ai_task_result_t;

// 生成本次 AI 任务的最小上下文预览。
esp_err_t fridge_ai_context_build_preview(const fridge_ai_task_request_t *request, fridge_ai_context_preview_t *out);

// 使用本地上下文生成 Mock 结构化结果；不调用第三方模型，不写入库存。
esp_err_t fridge_ai_context_test_task(const fridge_ai_task_request_t *request, fridge_ai_task_result_t *out);

// 判断任务类型是否为项目支持的 AI 任务。
bool fridge_ai_context_task_type_supported(const char *task_type);

#ifdef __cplusplus
}
#endif
