// 冰箱小精灵 AI 上下文编排层。
// 负责按任务类型组合人格模板、任务模板和最小动态上下文；当前提供预览和 Mock 结构化结果，真实对话由 ai_client 设备直连 API 完成。
// 硬件注意：本组件不联网、不控制 GPIO；运行时缓冲由调用方放在堆上，避免占用 USB 协议任务栈。

#include "fridge_ai_context.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "fridge_storage.h"

static const char *TAG = "fridge_ai_context";

static const char *PERSONA_TEMPLATE =
    "你是冰箱小精灵的智能厨房助手。回答要实用、保守、可确认、少打扰；不要编造库存、日期、数量或识别结果。";

static const char *supported_tasks[] = {
    "chat_assist",
    "recognize_ingredients",
    "inventory_parse",
    "recipe_generate",
    "shopping_list_generate",
    "reminder_explain",
    "voice_intent_parse",
};

typedef struct {
    char inventory[FRIDGE_STORAGE_MAX_JSON_LEN];
    char reminders[FRIDGE_STORAGE_MAX_JSON_LEN];
    char preferences[FRIDGE_STORAGE_MAX_JSON_LEN];
    char memory[FRIDGE_STORAGE_MAX_MEMORY_LEN];
    char offline[FRIDGE_STORAGE_MAX_JSON_LEN];
    char history_json[FRIDGE_STORAGE_MAX_JSON_LEN];
    char escaped_user[FRIDGE_AI_CONTEXT_MAX_TEXT_LEN * 2 + 1];
    char escaped_persona[512];
    char escaped_template[512];
} ai_context_work_t;

static void truncate_history_content(const char *input, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!input) {
        return;
    }

    size_t input_len = strlen(input);
    if (input_len + 1 <= out_size) {
        strlcpy(out, input, out_size);
        return;
    }

    if (out_size <= 4) {
        strlcpy(out, input, out_size);
        return;
    }

    size_t keep = out_size - 4;
    memcpy(out, input, keep);
    memcpy(out + keep, "...", 4);
}

static const char *task_template_for(const char *task_type)
{
    if (strcmp(task_type, "recognize_ingredients") == 0) {
        return "识别候选必须包含名称、数量估计、置信度和疑点；低置信度必须要求用户确认或重新拍照。";
    }
    if (strcmp(task_type, "inventory_parse") == 0) {
        return "把用户表达整理成库存变更建议，不得直接新增、删除、消耗或移动库存。";
    }
    if (strcmp(task_type, "recipe_generate") == 0) {
        return "优先使用已有且临期食材，输出菜名、可用库存、缺少食材、预计耗时和简要步骤。";
    }
    if (strcmp(task_type, "shopping_list_generate") == 0) {
        return "根据库存缺口和菜谱计划生成清单，区分建议购买和可选补充，避免过度推荐。";
    }
    if (strcmp(task_type, "reminder_explain") == 0) {
        return "解释临期和过期提醒，说明食材、剩余天数、位置和保守处理建议。";
    }
    if (strcmp(task_type, "voice_intent_parse") == 0) {
        return "把语音文本解析成意图和槽位；信息不足时返回需要确认的字段。";
    }
    return "作为家庭厨房助手回答用户问题；必须优先依据系统提供的结构化数据。";
}

static const char *safe_task_type(const char *task_type)
{
    return fridge_ai_context_task_type_supported(task_type) ? task_type : "chat_assist";
}

static bool append_text(char *out, size_t out_size, size_t *used, const char *text)
{
    if (!out || !used || !text || *used >= out_size) {
        return false;
    }
    int written = snprintf(out + *used, out_size - *used, "%s", text);
    if (written < 0 || (size_t)written >= out_size - *used) {
        return false;
    }
    *used += (size_t)written;
    return true;
}

static void json_escape_into(const char *text, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    size_t used = 0;
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    while (*p && used + 1 < out_size) {
        if (*p == '"' || *p == '\\') {
            if (used + 2 >= out_size) {
                break;
            }
            out[used++] = '\\';
            out[used++] = (char)*p++;
        } else if (*p == '\n') {
            if (used + 2 >= out_size) {
                break;
            }
            out[used++] = '\\';
            out[used++] = 'n';
            p++;
        } else if (*p == '\r') {
            if (used + 2 >= out_size) {
                break;
            }
            out[used++] = '\\';
            out[used++] = 'r';
            p++;
        } else if (*p == '\t') {
            if (used + 2 >= out_size) {
                break;
            }
            out[used++] = '\\';
            out[used++] = 't';
            p++;
        } else {
            out[used++] = (char)*p++;
        }
    }
    out[used] = '\0';
}

static void build_history_preview_json(const fridge_storage_chat_history_t *history, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    if (!history) {
        strlcpy(out, "{\"time_ready\":false,\"count\":0,\"ttl_seconds\":172800,\"messages\":[]}", out_size);
        return;
    }

    size_t used = 0;
    int written = snprintf(out,
                           out_size,
                           "{\"time_ready\":%s,\"count\":%u,\"ttl_seconds\":%u,\"messages\":[",
                           history->time_ready ? "true" : "false",
                           (unsigned)history->count,
                           (unsigned)history->ttl_seconds);
    if (written < 0 || (size_t)written >= out_size) {
        out[0] = '\0';
        return;
    }
    used = (size_t)written;

    for (size_t i = 0; i < history->count && i < FRIDGE_STORAGE_MAX_CHAT_MESSAGES; i++) {
        char history_preview[161] = {0};
        char escaped_content[sizeof(history_preview) * 2] = {0};
        char escaped_role[FRIDGE_STORAGE_MAX_CHAT_ROLE_LEN * 2] = {0};
        char escaped_task_type[64] = {0};
        const fridge_storage_chat_message_t *message = &history->messages[i];
        truncate_history_content(message->content, history_preview, sizeof(history_preview));
        json_escape_into(history_preview, escaped_content, sizeof(escaped_content));
        json_escape_into(message->role, escaped_role, sizeof(escaped_role));
        json_escape_into(message->task_type, escaped_task_type, sizeof(escaped_task_type));
        written = snprintf(out + used,
                           out_size - used,
                           "%s{\"role\":\"%s\",\"content\":\"%s\",\"task_type\":\"%s\",\"created_at\":%" PRId64 "}",
                           i == 0 ? "" : ",",
                           escaped_role,
                           escaped_content,
                           escaped_task_type,
                           message->created_at);
        if (written < 0 || (size_t)written >= out_size - used) {
            out[0] = '\0';
            return;
        }
        used += (size_t)written;
    }

    written = snprintf(out + used, out_size - used, "]}");
    if (written < 0 || (size_t)written >= out_size - used) {
        out[0] = '\0';
    }
}

bool fridge_ai_context_task_type_supported(const char *task_type)
{
    if (!task_type || task_type[0] == '\0') {
        return false;
    }
    for (size_t i = 0; i < sizeof(supported_tasks) / sizeof(supported_tasks[0]); i++) {
        if (strcmp(task_type, supported_tasks[i]) == 0) {
            return true;
        }
    }
    return false;
}

esp_err_t fridge_ai_context_build_preview(const fridge_ai_task_request_t *request, fridge_ai_context_preview_t *out)
{
    ESP_RETURN_ON_FALSE(request && out, ESP_ERR_INVALID_ARG, TAG, "invalid context preview args");
    memset(out, 0, sizeof(*out));

    const char *task_type = safe_task_type(request->task_type);
    ai_context_work_t *work = calloc(1, sizeof(*work));
    ESP_RETURN_ON_FALSE(work, ESP_ERR_NO_MEM, TAG, "AI context work allocation failed");
    strlcpy(work->inventory, "{}", sizeof(work->inventory));
    strlcpy(work->reminders, "{}", sizeof(work->reminders));
    strlcpy(work->preferences, "{}", sizeof(work->preferences));
    strlcpy(work->memory, "{}", sizeof(work->memory));
    strlcpy(work->offline, "{}", sizeof(work->offline));
    strlcpy(work->history_json, "{\"time_ready\":false,\"count\":0,\"ttl_seconds\":172800,\"messages\":[]}", sizeof(work->history_json));

    fridge_storage_status_t status = {0};
    fridge_storage_chat_history_t *chat_history = calloc(1, sizeof(*chat_history));
    if (!chat_history) {
        free(work);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NO_MEM, TAG, "chat history allocation failed");
    }
    esp_err_t err = fridge_storage_get_status(&status);
    if (err != ESP_OK) {
        free(chat_history);
        free(work);
        ESP_RETURN_ON_ERROR(err, TAG, "get storage status failed");
    }
    if (request->include_inventory) {
        err = fridge_storage_get_inventory_snapshot(work->inventory, sizeof(work->inventory));
        if (err != ESP_OK) {
            free(chat_history);
            free(work);
            ESP_RETURN_ON_ERROR(err, TAG, "get inventory failed");
        }
    }
    if (request->include_reminders) {
        err = fridge_storage_get_reminder_queue(work->reminders, sizeof(work->reminders));
        if (err != ESP_OK) {
            free(chat_history);
            free(work);
            ESP_RETURN_ON_ERROR(err, TAG, "get reminders failed");
        }
    }
    if (request->include_preferences) {
        err = fridge_storage_get_user_preferences(work->preferences, sizeof(work->preferences));
        if (err != ESP_OK) {
            free(chat_history);
            free(work);
            ESP_RETURN_ON_ERROR(err, TAG, "get preferences failed");
        }
    }
    if (request->include_memory) {
        err = fridge_storage_get_memory_summary(work->memory, sizeof(work->memory));
        if (err != ESP_OK) {
            free(chat_history);
            free(work);
            ESP_RETURN_ON_ERROR(err, TAG, "get memory failed");
        }
    }
    err = fridge_storage_get_offline_queue_summary(work->offline, sizeof(work->offline));
    if (err != ESP_OK) {
        free(chat_history);
        free(work);
        ESP_RETURN_ON_ERROR(err, TAG, "get offline queue failed");
    }
    err = fridge_storage_get_chat_history(chat_history, NULL);
    if (err != ESP_OK) {
        free(chat_history);
        free(work);
        ESP_RETURN_ON_ERROR(err, TAG, "get chat history failed");
    }
    build_history_preview_json(chat_history, work->history_json, sizeof(work->history_json));
    free(chat_history);
    if (work->history_json[0] == '\0') {
        free(work);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NO_MEM, TAG, "build history preview failed");
    }

    json_escape_into(request->user_text, work->escaped_user, sizeof(work->escaped_user));
    json_escape_into(PERSONA_TEMPLATE, work->escaped_persona, sizeof(work->escaped_persona));
    json_escape_into(task_template_for(task_type), work->escaped_template, sizeof(work->escaped_template));

    size_t used = 0;
    bool ok = true;
    ok = ok && append_text(out->preview_json, sizeof(out->preview_json), &used, "{\"schema_version\":1,");
    char number_field[96];
    snprintf(number_field,
             sizeof(number_field),
             "\"task_type\":\"%s\",\"request_id\":\"local_preview\",\"local_snapshot_version\":%lu,",
             task_type,
             (unsigned long)status.inventory_version);
    ok = ok && append_text(out->preview_json, sizeof(out->preview_json), &used, number_field);
    ok = ok && append_text(out->preview_json, sizeof(out->preview_json), &used, "\"persona_template\":\"");
    ok = ok && append_text(out->preview_json, sizeof(out->preview_json), &used, work->escaped_persona);
    ok = ok && append_text(out->preview_json, sizeof(out->preview_json), &used, "\",\"task_template\":\"");
    ok = ok && append_text(out->preview_json, sizeof(out->preview_json), &used, work->escaped_template);
    ok = ok && append_text(out->preview_json, sizeof(out->preview_json), &used, "\",\"user_text\":\"");
    ok = ok && append_text(out->preview_json, sizeof(out->preview_json), &used, work->escaped_user);
    ok = ok && append_text(out->preview_json, sizeof(out->preview_json), &used, "\",");
    ok = ok && append_text(out->preview_json, sizeof(out->preview_json), &used, "\"dynamic_context\":{");
    ok = ok && append_text(out->preview_json, sizeof(out->preview_json), &used, "\"inventory\":");
    ok = ok && append_text(out->preview_json, sizeof(out->preview_json), &used, request->include_inventory ? work->inventory : "null");
    ok = ok && append_text(out->preview_json, sizeof(out->preview_json), &used, ",\"reminders\":");
    ok = ok && append_text(out->preview_json, sizeof(out->preview_json), &used, request->include_reminders ? work->reminders : "null");
    ok = ok && append_text(out->preview_json, sizeof(out->preview_json), &used, ",\"preferences\":");
    ok = ok && append_text(out->preview_json, sizeof(out->preview_json), &used, request->include_preferences ? work->preferences : "null");
    ok = ok && append_text(out->preview_json, sizeof(out->preview_json), &used, ",\"memory_summary\":");
    ok = ok && append_text(out->preview_json, sizeof(out->preview_json), &used, request->include_memory ? work->memory : "null");
    ok = ok && append_text(out->preview_json, sizeof(out->preview_json), &used, ",\"offline_queue\":");
    ok = ok && append_text(out->preview_json, sizeof(out->preview_json), &used, work->offline);
    ok = ok && append_text(out->preview_json, sizeof(out->preview_json), &used, ",\"conversation_history\":");
    // 语音对话通常连续触发，完整历史会让部分 OpenAI-compatible 服务请求体过大或重复上下文而返回 400。
    // 需要历史时仍通过 assistant_request->history 注入；上下文 JSON 里仅在 include_memory 打开时放预览。
    ok = ok && append_text(out->preview_json, sizeof(out->preview_json), &used, request->include_memory ? work->history_json : "null");
    ok = ok && append_text(out->preview_json, sizeof(out->preview_json), &used, "},\"output_policy\":{\"ai_result_must_be_confirmed\":true,\"do_not_fabricate_inventory\":true,\"do_not_store_full_chat\":true}}");
    if (!ok) {
        free(work);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NO_MEM, TAG, "context preview buffer too small");
    }

    strlcpy(out->task_type, task_type, sizeof(out->task_type));
    out->local_snapshot_version = status.inventory_version;
    out->needs_confirmation = strcmp(task_type, "chat_assist") != 0;
    free(work);
    return ESP_OK;
}

esp_err_t fridge_ai_context_test_task(const fridge_ai_task_request_t *request, fridge_ai_task_result_t *out)
{
    ESP_RETURN_ON_FALSE(request && out, ESP_ERR_INVALID_ARG, TAG, "invalid test task args");
    memset(out, 0, sizeof(*out));

    const char *task_type = safe_task_type(request->task_type);
    strlcpy(out->task_type, task_type, sizeof(out->task_type));
    out->needs_confirmation = strcmp(task_type, "chat_assist") != 0;
    out->confidence_percent = 82;
    strlcpy(out->safety_note,
            "Mock 结果仅用于验证上下文注入和结构化输出；不会直接写入库存，正式结果仍需用户确认。",
            sizeof(out->safety_note));

    if (strcmp(task_type, "recipe_generate") == 0) {
        strlcpy(out->result_json,
                "{\"schema_version\":1,\"type\":\"recipe_generate\",\"recipe\":{\"name\":\"番茄鸡蛋汤\",\"use_inventory\":[\"番茄\",\"鸡蛋\"],\"missing\":[\"葱花，可选\"],\"time_minutes\":15,\"steps\":[\"番茄切块，鸡蛋打散\",\"少油炒番茄出汁\",\"加水煮开后淋入蛋液\",\"按口味少量加盐\"]},\"needs_confirmation\":false}",
                sizeof(out->result_json));
    } else if (strcmp(task_type, "shopping_list_generate") == 0) {
        strlcpy(out->result_json,
                "{\"schema_version\":1,\"type\":\"shopping_list_generate\",\"suggested\":[\"绿叶菜\",\"面条\"],\"optional\":[\"葱花\",\"低脂酸奶\"],\"basis\":\"根据当前库存和快手晚餐偏好生成，避免过量购买\"}",
                sizeof(out->result_json));
    } else if (strcmp(task_type, "recognize_ingredients") == 0) {
        strlcpy(out->result_json,
                "{\"schema_version\":1,\"type\":\"recognize_ingredients\",\"candidates\":[{\"name\":\"番茄\",\"quantity\":\"约2-3个\",\"confidence\":0.82,\"doubt\":\"Mock 未接入真实图片\"}],\"needs_confirmation\":true,\"confirm_fields\":[\"名称\",\"数量\",\"保质期\",\"位置\"]}",
                sizeof(out->result_json));
    } else if (strcmp(task_type, "inventory_parse") == 0 || strcmp(task_type, "voice_intent_parse") == 0) {
        strlcpy(out->result_json,
                "{\"schema_version\":1,\"type\":\"inventory_change_suggestion\",\"intent\":\"consume_or_add\",\"slots\":{\"item_name\":\"待确认\",\"quantity\":\"待确认\",\"location\":\"待确认\"},\"needs_confirmation\":true,\"note\":\"信息不足时只生成建议，不直接修改库存\"}",
                sizeof(out->result_json));
    } else if (strcmp(task_type, "reminder_explain") == 0) {
        strlcpy(out->result_json,
                "{\"schema_version\":1,\"type\":\"reminder_explain\",\"items\":[{\"name\":\"牛奶\",\"days_left\":1,\"location\":\"门架中层\",\"suggestion\":\"优先饮用；若有异味或胀包请丢弃\"}],\"tone\":\"brief\"}",
                sizeof(out->result_json));
    } else {
        strlcpy(out->result_json,
                "{\"schema_version\":1,\"type\":\"chat_assist\",\"reply\":\"我会优先依据当前库存、临期提醒和你的偏好给出建议；信息不足时会请你确认，不会编造库存。\"}",
                sizeof(out->result_json));
        out->needs_confirmation = false;
        out->confidence_percent = 90;
    }

    return ESP_OK;
}
