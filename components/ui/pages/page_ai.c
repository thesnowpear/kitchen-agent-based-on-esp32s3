// 冰箱小精灵 AI 语音对话页。
// 屏幕端只做轻量语音入口：录音和 HTTPS/AI/TTS 都放到后台任务，避免阻塞 LVGL 刷新和触摸响应。

#include "fridge_ui_internal.h"

#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "fridge_ai_actions.h"
#include "fridge_ai_client.h"
#include "fridge_ai_context.h"
#include "fridge_asr.h"
#include "fridge_audio.h"
#include "fridge_kitchen_tools.h"
#include "fridge_speaker.h"
#include "fridge_storage.h"
#include "fridge_wake_word.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define AI_VOICE_TASK_STACK 32768

static const char *TAG = "fridge_ui_ai";

static lv_obj_t *s_status;
static lv_obj_t *s_ai_bubble;
static lv_obj_t *s_ai_text;
static lv_obj_t *s_voice_btn;
static lv_obj_t *s_voice_label;
static lv_obj_t *s_voice_hint;
static bool s_recording;
static bool s_busy;
static bool s_wake_was_listening;
static uint32_t s_generation;

typedef struct {
    esp_err_t err;
    uint32_t generation;
    bool heap_allocated;
    char transcript[FRIDGE_ASR_MAX_TEXT_LEN + 1];
    char reply[FRIDGE_AI_MAX_REPLY_LEN + 1];
    char error[FRIDGE_AI_MAX_ERROR_LEN + 1];
} ai_voice_done_t;

static ai_voice_done_t s_emergency_done;

static size_t ui_ai_convert_history(const fridge_storage_chat_history_t *storage_history,
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
            // 历史顺序异常时丢弃已收集片段，避免兼容 API 因 role 顺序报错。
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

static void set_status(const char *text)
{
    fridge_ui_label_set_text_if_changed(s_status, text ? text : "");
}

static void set_button_state(const char *label, const char *hint)
{
    fridge_ui_label_set_text_if_changed(s_voice_label, label ? label : "开始说话");
    fridge_ui_label_set_text_if_changed(s_voice_hint, hint ? hint : "");
}

static void show_latest_texts(const char *transcript, const char *reply)
{
    (void)transcript;
    if (reply && reply[0] != '\0') {
        fridge_ui_label_set_text_if_changed(s_ai_text, reply);
    }
}

static void clear_chat_cb(lv_event_t *event)
{
    (void)event;
    if (s_recording || s_busy) {
        fridge_ui_toast("请先完成当前语音");
        return;
    }

    // 清空设备侧会话历史，同时避免后台任务改写当前页面。
    esp_err_t err = fridge_storage_clear_chat_history();
    if (err != ESP_OK) {
        set_status("清空对话失败");
        ESP_LOGW(TAG, "clear chat history failed: %s", esp_err_to_name(err));
        return;
    }
    s_generation++;
    fridge_ui_label_set_text_if_changed(s_ai_text, "你好，我可以帮你查临期、想晚餐、解释库存。");
    set_status("对话已清空");
    set_button_state("开始说话", "点一下录音，再点一下发送");
    fridge_ui_toast("对话已清空");
}

static void recipe_jump_cb(lv_event_t *event)
{
    (void)event;
    fridge_ui_show_page(FRIDGE_UI_PAGE_RECIPE);
}

static void finish_recording_visual(void)
{
    s_recording = false;
    s_busy = true;
    s_generation++;
    set_status("正在识别并思考...");
    set_button_state("处理中", "语音转文字、AI 回复和播报会自动完成");
}

static void restore_wake_if_needed(void)
{
    if (s_wake_was_listening) {
        esp_err_t err = fridge_wake_word_start();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "restart wake word failed: %s", esp_err_to_name(err));
        }
    }
    s_wake_was_listening = false;
}

static void ai_voice_done_async(void *user_data)
{
    ai_voice_done_t *done = (ai_voice_done_t *)user_data;
    if (!done) {
        return;
    }

    s_busy = false;
    if (done->generation != s_generation) {
        free(done);
        return;
    }

    if (done->err == ESP_OK) {
        show_latest_texts(done->transcript, done->reply);
        set_status(fridge_speaker_get_tts_enabled() ? "回复已播报" : "回复已生成");
        set_button_state("继续说话", "再点一次开始新的语音对话");
    } else {
        set_status(done->error[0] ? done->error : esp_err_to_name(done->err));
        if (done->transcript[0] != '\0') {
            show_latest_texts(done->transcript, NULL);
        }
        set_button_state("重新说话", "请靠近麦克风，录 1 到 6 秒");
    }
    restore_wake_if_needed();
    if (done->heap_allocated) {
        free(done);
    }
}

static void ai_voice_task(void *arg)
{
    uint32_t generation = (uint32_t)(uintptr_t)arg;
    ai_voice_done_t *done = calloc(1, sizeof(*done));
    fridge_asr_result_t *asr = calloc(1, sizeof(*asr));
    fridge_ai_assistant_result_t *ai = calloc(1, sizeof(*ai));
    fridge_ai_assistant_request_t *request = calloc(1, sizeof(*request));
    fridge_ai_chat_history_item_t *history = calloc(FRIDGE_AI_MAX_CHAT_HISTORY, sizeof(*history));
    fridge_storage_chat_history_t *storage_history = calloc(1, sizeof(*storage_history));
    fridge_ai_context_preview_t *preview = calloc(1, sizeof(*preview));
    fridge_storage_chat_message_t *persisted = calloc(2, sizeof(*persisted));
    size_t history_pruned = 0;

    if (!done) {
        memset(&s_emergency_done, 0, sizeof(s_emergency_done));
        done = &s_emergency_done;
    } else {
        done->heap_allocated = true;
    }
    if (!asr || !ai || !request || !history || !storage_history || !preview || !persisted) {
        done->err = ESP_ERR_NO_MEM;
        strlcpy(done->error, "语音对话内存不足", sizeof(done->error));
        goto finish;
    }
    done->generation = generation;

    done->err = fridge_asr_transcribe_latest_recording(asr);
    if (done->err != ESP_OK) {
        strlcpy(done->error, asr->error[0] ? asr->error : esp_err_to_name(done->err), sizeof(done->error));
        goto finish;
    }
    strlcpy(done->transcript, asr->text, sizeof(done->transcript));
    if (asr->text[0] == '\0' || strlen(asr->text) < 2) {
        done->err = ESP_ERR_INVALID_RESPONSE;
        strlcpy(done->error, "没有听清，请重新说一遍", sizeof(done->error));
        goto finish;
    }

    fridge_ai_task_request_t context_request = {
        .include_inventory = true,
        .include_memory = true,
        .include_reminders = true,
        .include_preferences = true,
    };
    strlcpy(context_request.task_type, "voice_intent_parse", sizeof(context_request.task_type));
    strlcpy(context_request.user_text, asr->text, sizeof(context_request.user_text));

    done->err = fridge_storage_get_chat_history(storage_history, &history_pruned);
    if (done->err != ESP_OK) {
        strlcpy(done->error, esp_err_to_name(done->err), sizeof(done->error));
        goto finish;
    }
    done->err = fridge_ai_context_build_preview(&context_request, preview);
    if (done->err != ESP_OK) {
        strlcpy(done->error, esp_err_to_name(done->err), sizeof(done->error));
        goto finish;
    }

    strlcpy(request->message, asr->text, sizeof(request->message));
    strlcpy(request->task_type, "voice_intent_parse", sizeof(request->task_type));
    strlcpy(request->context_json, preview->preview_json, sizeof(request->context_json));
    request->history = history;
    request->history_count = ui_ai_convert_history(storage_history, history, FRIDGE_AI_MAX_CHAT_HISTORY);
    request->context_injected = true;
    request->needs_confirmation = preview->needs_confirmation;
    request->local_snapshot_version = preview->local_snapshot_version;

    done->err = fridge_ai_client_assistant_chat(request, ai);
    if (done->err != ESP_OK) {
        strlcpy(done->error, ai->chat.error[0] ? ai->chat.error : esp_err_to_name(done->err), sizeof(done->error));
        goto finish;
    }
    bool memory_updated = false;
    done->err = fridge_storage_apply_memory_directive(ai->chat.reply, done->reply, sizeof(done->reply), &memory_updated);
    if (done->err != ESP_OK) {
        strlcpy(done->error, esp_err_to_name(done->err), sizeof(done->error));
        goto finish;
    }
    if (done->reply[0] == '\0') {
        strlcpy(done->reply, ai->chat.reply, sizeof(done->reply));
    }
    fridge_ai_action_result_t action_result = {0};
    char visible_reply[FRIDGE_AI_MAX_REPLY_LEN + 1] = {0};
    esp_err_t action_err = fridge_ai_actions_strip_directives(done->reply,
                                                              visible_reply,
                                                              sizeof(visible_reply),
                                                              &action_result);
    bool tool_feedback = action_result.tool[0] != '\0' && action_result.message[0] != '\0';
    if ((action_err == ESP_OK && action_result.executed) || tool_feedback) {
        strlcpy(done->reply, action_result.message, sizeof(done->reply));
    } else if (visible_reply[0] != '\0') {
        strlcpy(done->reply, visible_reply, sizeof(done->reply));
    }

    strlcpy(persisted[0].role, "user", sizeof(persisted[0].role));
    strlcpy(persisted[0].content, asr->text, sizeof(persisted[0].content));
    strlcpy(persisted[0].task_type, "voice_intent_parse", sizeof(persisted[0].task_type));
    strlcpy(persisted[1].role, "assistant", sizeof(persisted[1].role));
    strlcpy(persisted[1].content, done->reply, sizeof(persisted[1].content));
    strlcpy(persisted[1].task_type, "voice_intent_parse", sizeof(persisted[1].task_type));
    esp_err_t storage_err = fridge_storage_append_chat_messages(persisted, 2, &history_pruned);
    if (storage_err != ESP_OK && storage_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "persist UI voice history failed: %s", esp_err_to_name(storage_err));
    }

    if (fridge_speaker_get_tts_enabled()) {
        // TTS 播放使用扬声器 I2S TX；此时录音已经结束，避免和麦克风 RX 争用同一组 BCLK/WS。
        fridge_speaker_status_t speaker_status = {0};
        esp_err_t tts_err = fridge_speaker_synthesize_and_play(done->reply, &speaker_status);
        if (tts_err != ESP_OK) {
            ESP_LOGW(TAG, "TTS play failed after AI reply: %s", speaker_status.error[0] ? speaker_status.error : esp_err_to_name(tts_err));
        }
    }

finish:
    if (done) {
        done->generation = generation;
        if (lv_async_call(ai_voice_done_async, done) != LV_RESULT_OK) {
            free(done);
        }
    }
    free(asr);
    free(ai);
    free(request);
    free(history);
    free(storage_history);
    free(preview);
    free(persisted);
    ESP_LOGI(TAG, "UI voice task done, stack high watermark=%u words", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
}

static void voice_cb(lv_event_t *event)
{
    (void)event;
    if (s_busy) {
        fridge_ui_toast("正在处理上一句");
        return;
    }

    if (!s_recording) {
        fridge_speaker_status_t speaker = {0};
        if (fridge_speaker_get_status(&speaker) == ESP_OK &&
            (speaker.state == FRIDGE_SPEAKER_STATE_SYNTHESIZING || speaker.state == FRIDGE_SPEAKER_STATE_PLAYING)) {
            (void)fridge_speaker_stop();
        }

        fridge_wake_word_status_t wake = {0};
        if (fridge_wake_word_get_status(&wake) == ESP_OK && wake.state == FRIDGE_WAKE_WORD_STATE_LISTENING) {
            s_wake_was_listening = true;
            esp_err_t wake_err = fridge_wake_word_stop();
            if (wake_err != ESP_OK) {
                set_status("唤醒监听暂停失败");
                return;
            }
        }

        esp_err_t err = fridge_audio_start_recording();
        if (err != ESP_OK) {
            restore_wake_if_needed();
            set_status(err == ESP_ERR_INVALID_STATE ? "麦克风正被占用" : esp_err_to_name(err));
            return;
        }
        s_recording = true;
        set_status("正在听你说话");
        set_button_state("停止并发送", "最多录 6 秒，结束后自动回复并播报");
        return;
    }

    esp_err_t err = fridge_audio_stop_recording();
    if (err != ESP_OK) {
        s_recording = false;
        restore_wake_if_needed();
        set_status(esp_err_to_name(err));
        set_button_state("重新说话", "录音停止失败，请再试一次");
        return;
    }

    finish_recording_visual();
    BaseType_t ok = xTaskCreate(ai_voice_task, "ui_voice_ai", AI_VOICE_TASK_STACK, (void *)(uintptr_t)s_generation, 4, NULL);
    if (ok != pdPASS) {
        s_busy = false;
        restore_wake_if_needed();
        set_status("语音任务创建失败");
        set_button_state("重新说话", "系统繁忙，请稍后再试");
    }
}

static lv_obj_t *make_bubble(lv_obj_t *parent, const char *title, const char *body, bool me)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *bubble = lv_obj_create(parent);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(bubble, me ? 592 : 520, me ? 110 : 104);
    lv_obj_set_style_bg_color(bubble, me ? theme->accent : lv_color_hex(0xFFF6E5), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bubble, me ? theme->accent : lv_color_hex(0xF4C987), 0);
    lv_obj_set_style_border_width(bubble, 1, 0);
    lv_obj_set_style_radius(bubble, 16, 0);
    lv_obj_set_style_shadow_width(bubble, 0, 0);
    lv_obj_set_style_pad_all(bubble, 12, 0);

    lv_obj_t *title_label = lv_label_create(bubble);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_color(title_label, me ? lv_color_white() : theme->accent, 0);
    lv_obj_set_style_text_font(title_label, fridge_ui_font_small(), 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *body_label = lv_label_create(bubble);
    lv_label_set_text(body_label, body);
    lv_obj_set_width(body_label, me ? 548 : 496);
    lv_obj_set_style_text_color(body_label, me ? lv_color_white() : theme->text, 0);
    lv_obj_set_style_text_font(body_label, fridge_ui_font_ai_body(), 0);
    lv_obj_align(body_label, LV_ALIGN_TOP_LEFT, 0, 28);

    if (!me) {
        s_ai_text = body_label;
    }
    return bubble;
}

void fridge_ui_page_ai_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();

    lv_obj_t *kicker = lv_label_create(parent);
    lv_label_set_text(kicker, "设备语音优先");
    lv_obj_set_style_text_color(kicker, theme->muted, 0);
    lv_obj_set_style_text_font(kicker, fridge_ui_font_small(), 0);
    lv_obj_align(kicker, LV_ALIGN_TOP_LEFT, 36, 8);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "AI 助手");
    lv_obj_set_style_text_font(title, fridge_ui_font_title(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 36, 36);

    lv_obj_t *clear = lv_button_create(parent);
    lv_obj_set_size(clear, 154, 48);
    lv_obj_align(clear, LV_ALIGN_TOP_RIGHT, -224, 28);
    lv_obj_set_style_bg_color(clear, lv_color_hex(0xFFF8EA), 0);
    lv_obj_set_style_border_color(clear, lv_color_hex(0xE8CBA2), 0);
    lv_obj_set_style_border_width(clear, 1, 0);
    lv_obj_set_style_shadow_width(clear, 0, 0);
    lv_obj_set_style_radius(clear, 24, 0);
    lv_obj_add_event_cb(clear, clear_chat_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *clear_label = lv_label_create(clear);
    lv_label_set_text(clear_label, "清空对话");
    lv_obj_set_style_text_color(clear_label, theme->accent, 0);
    lv_obj_set_style_text_font(clear_label, fridge_ui_font_small(), 0);
    lv_obj_center(clear_label);

    lv_obj_t *recipe = lv_button_create(parent);
    lv_obj_set_size(recipe, 166, 48);
    lv_obj_align(recipe, LV_ALIGN_TOP_RIGHT, -38, 28);
    lv_obj_set_style_bg_color(recipe, lv_color_hex(0xFFF0D0), 0);
    lv_obj_set_style_border_color(recipe, theme->accent_2, 0);
    lv_obj_set_style_border_width(recipe, 1, 0);
    lv_obj_set_style_shadow_width(recipe, 0, 0);
    lv_obj_set_style_radius(recipe, 24, 0);
    lv_obj_add_event_cb(recipe, recipe_jump_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *recipe_label = lv_label_create(recipe);
    lv_label_set_text(recipe_label, "推荐菜谱");
    lv_obj_set_style_text_color(recipe_label, theme->accent_2, 0);
    lv_obj_set_style_text_font(recipe_label, fridge_ui_font_small(), 0);
    lv_obj_center(recipe_label);

    s_status = lv_label_create(parent);
    lv_label_set_text(s_status, "点下方按钮，和小精灵说一句");
    lv_obj_set_style_text_color(s_status, theme->muted, 0);
    lv_obj_set_style_text_font(s_status, fridge_ui_font_small(), 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 38, 82);

    s_ai_bubble = make_bubble(parent, "小精灵", "你好，我可以帮你查临期、想晚餐、解释库存。", false);
    lv_obj_set_pos(s_ai_bubble, 64, 140);

    s_voice_btn = lv_button_create(parent);
    lv_obj_set_size(s_voice_btn, 286, 76);
    lv_obj_align(s_voice_btn, LV_ALIGN_BOTTOM_MID, 0, -50);
    lv_obj_set_style_bg_color(s_voice_btn, theme->accent_2, 0);
    lv_obj_set_style_radius(s_voice_btn, 34, 0);
    lv_obj_set_style_shadow_width(s_voice_btn, 16, 0);
    lv_obj_set_style_shadow_color(s_voice_btn, lv_color_hex(0xD95745), 0);
    lv_obj_add_event_cb(s_voice_btn, voice_cb, LV_EVENT_CLICKED, NULL);
    s_voice_label = lv_label_create(s_voice_btn);
    lv_label_set_text(s_voice_label, "开始说话");
    lv_obj_set_style_text_color(s_voice_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_voice_label, fridge_ui_font_body(), 0);
    lv_obj_center(s_voice_label);

    s_voice_hint = lv_label_create(parent);
    lv_label_set_text(s_voice_hint, "点一下录音，再点一下发送");
    lv_obj_set_style_text_color(s_voice_hint, theme->muted, 0);
    lv_obj_set_style_text_font(s_voice_hint, fridge_ui_font_small(), 0);
    lv_obj_align_to(s_voice_hint, s_voice_btn, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
}

void fridge_ui_page_ai_update(void)
{
}
