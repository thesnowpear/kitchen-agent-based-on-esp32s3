// 冰箱小精灵公共语音会话实现。
// 负责把 audio 组件保留的最近一段 PCM 录音送入 ASR，再携带项目上下文调用 AI，并写回本地会话历史。

#include "fridge_voice_session.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "fridge_ai_actions.h"
#include "fridge_ai_client.h"
#include "fridge_ai_context.h"
#include "fridge_asr.h"
#include "fridge_storage.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "voice_session";

static size_t convert_storage_history_to_ai_history(const fridge_storage_chat_history_t *storage_history,
                                                    fridge_ai_chat_history_item_t *history,
                                                    size_t max_history)
{
    if (!storage_history || !history || max_history == 0) {
        return 0;
    }

    size_t source_count = storage_history->count;
    if (source_count > FRIDGE_STORAGE_MAX_CHAT_MESSAGES) {
        source_count = FRIDGE_STORAGE_MAX_CHAT_MESSAGES;
    }
    if (source_count > 0 && strcmp(storage_history->messages[source_count - 1].role, "user") == 0) {
        // 历史注入只使用已经完成的 user/assistant 轮次，避免异常中断遗留的 user 单边消息破坏请求格式。
        source_count--;
    }
    if (source_count % 2 != 0) {
        source_count--;
    }

    size_t pair_limit = (max_history / 2) * 2;
    if (pair_limit == 0) {
        return 0;
    }
    size_t start = source_count > pair_limit ? (source_count - pair_limit) : 0;
    if (start % 2 != 0) {
        start++;
    }

    size_t count = 0;
    for (size_t i = start; i < source_count && count < max_history; i++) {
        const fridge_storage_chat_message_t *message = &storage_history->messages[i];
        if (message->content[0] == '\0') {
            continue;
        }
        bool expect_user = (count % 2) == 0;
        if ((expect_user && strcmp(message->role, "user") != 0) ||
            (!expect_user && strcmp(message->role, "assistant") != 0)) {
            // 真实 OpenAI-compatible 服务对 role 顺序更敏感，遇到坏轮次时宁可丢弃前缀。
            count = 0;
            continue;
        }
        strlcpy(history[count].role,
                strcmp(message->role, "assistant") == 0 ? "assistant" : "user",
                sizeof(history[count].role));
        strlcpy(history[count].content, message->content, sizeof(history[count].content));
        count++;
    }
    return (count / 2) * 2;
}

esp_err_t fridge_voice_session_run_latest_recording(fridge_voice_session_result_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    fridge_asr_result_t *asr = calloc(1, sizeof(*asr));
    fridge_ai_assistant_result_t *ai = calloc(1, sizeof(*ai));
    fridge_ai_assistant_request_t *assistant_request = calloc(1, sizeof(*assistant_request));
    fridge_ai_chat_history_item_t *history = calloc(FRIDGE_AI_MAX_CHAT_HISTORY, sizeof(*history));
    fridge_storage_chat_history_t *storage_history = calloc(1, sizeof(*storage_history));
    fridge_storage_chat_message_t *persisted_messages = calloc(2, sizeof(*persisted_messages));
    fridge_ai_context_preview_t *preview = calloc(1, sizeof(*preview));
    if (!asr || !ai || !assistant_request || !history || !storage_history || !persisted_messages || !preview) {
        free(asr);
        free(ai);
        free(assistant_request);
        free(history);
        free(storage_history);
        free(persisted_messages);
        free(preview);
        strlcpy(out->error, "voice session allocation failed", sizeof(out->error));
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = fridge_asr_transcribe_latest_recording(asr);
    if (err == ESP_OK && (asr->text[0] == '\0' || strlen(asr->text) < 2)) {
        // ASR 空文本不继续请求 AI，避免兼容 API 因空 user content 返回 400。
        strlcpy(asr->error, "ASR 转写为空或过短，请重新录音", sizeof(asr->error));
        err = ESP_ERR_INVALID_RESPONSE;
    }

    size_t history_pruned_count = 0;
    if (err == ESP_OK) {
        fridge_ai_task_request_t context_request = {
            .include_inventory = true,
            .include_memory = true,
            .include_reminders = true,
            .include_preferences = true,
        };
        strlcpy(context_request.task_type, "voice_intent_parse", sizeof(context_request.task_type));
        strlcpy(context_request.user_text, asr->text, sizeof(context_request.user_text));

        err = fridge_storage_get_chat_history(storage_history, &history_pruned_count);
        if (err == ESP_OK) {
            err = fridge_ai_context_build_preview(&context_request, preview);
        }
        if (err == ESP_OK) {
            strlcpy(assistant_request->message, asr->text, sizeof(assistant_request->message));
            strlcpy(assistant_request->task_type, "voice_intent_parse", sizeof(assistant_request->task_type));
            strlcpy(assistant_request->context_json, preview->preview_json, sizeof(assistant_request->context_json));
            assistant_request->history = history;
            assistant_request->history_count = convert_storage_history_to_ai_history(storage_history, history, FRIDGE_AI_MAX_CHAT_HISTORY);
            assistant_request->context_injected = true;
            assistant_request->needs_confirmation = preview->needs_confirmation;
            assistant_request->local_snapshot_version = preview->local_snapshot_version;
            err = fridge_ai_client_assistant_chat(assistant_request, ai);
        }
    }

    if (err == ESP_OK) {
        char clean_reply[FRIDGE_STORAGE_MAX_CHAT_CONTENT_LEN + 1] = {0};
        bool memory_updated = false;
        (void)fridge_storage_apply_memory_directive(ai->chat.reply,
                                                    clean_reply,
                                                    sizeof(clean_reply),
                                                    &memory_updated);
        if (clean_reply[0] != '\0') {
            strlcpy(ai->chat.reply, clean_reply, sizeof(ai->chat.reply));
        }
        fridge_ai_action_result_t action_result = {0};
        char visible_reply[FRIDGE_AI_MAX_REPLY_LEN + 1] = {0};
        esp_err_t action_err = fridge_ai_actions_strip_directives(ai->chat.reply,
                                                                  visible_reply,
                                                                  sizeof(visible_reply),
                                                                  &action_result);
        bool action_feedback = action_result.tool[0] != '\0' && action_result.message[0] != '\0';
        if ((action_err == ESP_OK && action_result.executed) || action_feedback) {
            strlcpy(ai->chat.reply, action_result.message, sizeof(ai->chat.reply));
        } else if (visible_reply[0] != '\0') {
            strlcpy(ai->chat.reply, visible_reply, sizeof(ai->chat.reply));
        }

        snprintf(persisted_messages[0].id, sizeof(persisted_messages[0].id), "msg_%" PRIu32 "_u", (uint32_t)(xTaskGetTickCount() & 0xFFFFFF));
        strlcpy(persisted_messages[0].role, "user", sizeof(persisted_messages[0].role));
        strlcpy(persisted_messages[0].content, asr->text, sizeof(persisted_messages[0].content));
        strlcpy(persisted_messages[0].task_type, "voice_intent_parse", sizeof(persisted_messages[0].task_type));
        snprintf(persisted_messages[1].id, sizeof(persisted_messages[1].id), "msg_%" PRIu32 "_a", (uint32_t)((xTaskGetTickCount() + 1) & 0xFFFFFF));
        strlcpy(persisted_messages[1].role, "assistant", sizeof(persisted_messages[1].role));
        strlcpy(persisted_messages[1].content, ai->chat.reply, sizeof(persisted_messages[1].content));
        strlcpy(persisted_messages[1].task_type, "voice_intent_parse", sizeof(persisted_messages[1].task_type));
        (void)fridge_storage_append_chat_messages(persisted_messages, 2, &history_pruned_count);
    }

    strlcpy(out->transcript, asr->text, sizeof(out->transcript));
    strlcpy(out->reply, ai->chat.reply, sizeof(out->reply));
    strlcpy(out->asr_model, asr->model, sizeof(out->asr_model));
    strlcpy(out->ai_model, ai->chat.model, sizeof(out->ai_model));
    out->asr_http_status = asr->http_status;
    out->ai_http_status = ai->chat.http_status;
    out->asr_latency_ms = asr->latency_ms;
    out->ai_latency_ms = ai->chat.latency_ms;
    out->audio_bytes = asr->audio_bytes;
    out->history_pruned_count = history_pruned_count;
    if (err != ESP_OK) {
        strlcpy(out->error,
                asr->error[0] ? asr->error : (ai->chat.error[0] ? ai->chat.error : esp_err_to_name(err)),
                sizeof(out->error));
    }

    ESP_LOGI(TAG, "voice session done err=%s, asr_http=%d, ai_http=%d", esp_err_to_name(err), asr->http_status, ai->chat.http_status);
    free(asr);
    free(ai);
    free(assistant_request);
    free(history);
    free(storage_history);
    free(persisted_messages);
    free(preview);
    return err;
}
