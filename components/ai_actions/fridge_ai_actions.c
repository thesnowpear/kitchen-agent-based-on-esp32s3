// 冰箱小精灵 AI 白名单动作执行器。
// 当前只允许低风险 UI 页面切换和已有厨房计时工具；不开放拍照、亮度、音量、配网、OTA 或 GPIO 控制。

#include "fridge_ai_actions.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "fridge_kitchen_tools.h"

static const char *TAG = "fridge_ai_actions";

static fridge_ai_action_ui_page_cb_t s_ui_page_cb;

static void action_result(fridge_ai_action_result_t *out,
                          bool executed,
                          bool needs_confirmation,
                          const char *tool,
                          const char *action,
                          const char *page,
                          const char *message)
{
    if (!out) {
        return;
    }
    out->executed = executed;
    out->needs_confirmation = needs_confirmation;
    strlcpy(out->tool, tool ? tool : "", sizeof(out->tool));
    strlcpy(out->action, action ? action : "", sizeof(out->action));
    strlcpy(out->page, page ? page : "", sizeof(out->page));
    strlcpy(out->message, message ? message : "", sizeof(out->message));
}

static const char *json_string(const cJSON *root, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static bool ui_json_key_allowed(const char *key)
{
    static const char *allowed[] = {
        "tool",
        "type",
        "action",
        "page",
        "message",
    };
    if (!key) {
        return false;
    }
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (strcmp(key, allowed[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool page_key_allowed(const char *page)
{
    static const char *allowed[] = {
        "home",
        "standby",
        "zone",
        "door",
        "recipe",
        "nutrition",
        "shopping",
        "settings",
        "wifi",
        "more",
        "offline",
        "ai",
        "timer",
        "stopwatch",
        "alarm",
    };
    if (!page) {
        return false;
    }
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (strcmp(page, allowed[i]) == 0) {
            return true;
        }
    }
    return false;
}

static const char *page_label(const char *page)
{
    if (strcmp(page, "home") == 0) {
        return "主页";
    }
    if (strcmp(page, "standby") == 0) {
        return "待机页";
    }
    if (strcmp(page, "zone") == 0) {
        return "空间页";
    }
    if (strcmp(page, "door") == 0) {
        return "开门提醒页";
    }
    if (strcmp(page, "recipe") == 0) {
        return "菜谱页";
    }
    if (strcmp(page, "nutrition") == 0) {
        return "营养页";
    }
    if (strcmp(page, "shopping") == 0) {
        return "购物清单页";
    }
    if (strcmp(page, "settings") == 0) {
        return "设置页";
    }
    if (strcmp(page, "wifi") == 0) {
        return "Wi-Fi 设置页";
    }
    if (strcmp(page, "more") == 0) {
        return "更多页";
    }
    if (strcmp(page, "offline") == 0) {
        return "离线页";
    }
    if (strcmp(page, "ai") == 0) {
        return "AI 助手页";
    }
    if (strcmp(page, "timer") == 0) {
        return "定时器页";
    }
    if (strcmp(page, "stopwatch") == 0) {
        return "秒表页";
    }
    if (strcmp(page, "alarm") == 0) {
        return "闹钟页";
    }
    return "页面";
}

static char *json_slice_alloc(const char *reply, const char **out_start, const char **out_end)
{
    if (out_start) {
        *out_start = NULL;
    }
    if (out_end) {
        *out_end = NULL;
    }
    const char *start = strchr(reply ? reply : "", '{');
    const char *end = strrchr(reply ? reply : "", '}');
    if (!start || !end || end <= start) {
        return NULL;
    }
    size_t len = (size_t)(end - start + 1);
    char *slice = calloc(1, len + 1);
    if (!slice) {
        return NULL;
    }
    memcpy(slice, start, len);
    if (out_start) {
        *out_start = start;
    }
    if (out_end) {
        *out_end = end + 1;
    }
    return slice;
}

static esp_err_t execute_ui_json(const cJSON *root, fridge_ai_action_result_t *out)
{
    const char *action = json_string(root, "action");
    const char *page = json_string(root, "page");
    const char *message = json_string(root, "message");
    if (!action) {
        action_result(out, false, true, "ui", "", "", "UI 指令缺少 action");
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(action, "home") == 0) {
        page = "home";
        action = "switch_page";
    } else if (strcmp(action, "standby") == 0) {
        page = "standby";
        action = "switch_page";
    }
    if (strcmp(action, "switch_page") != 0 && strcmp(action, "show_page") != 0) {
        action_result(out, false, true, "ui", action, "", "不支持这个 UI 动作");
        return ESP_ERR_INVALID_ARG;
    }
    if (!page || page[0] == '\0') {
        action_result(out, false, true, "ui", action, "", "UI 指令缺少页面名称");
        return ESP_ERR_INVALID_ARG;
    }
    if (!page_key_allowed(page)) {
        if (strcmp(page, "camera") == 0 || strcmp(page, "camera_result") == 0) {
            action_result(out, false, true, "ui", action, page, "拍照页面会启动摄像头，暂需用户手动确认进入");
            return ESP_ERR_INVALID_STATE;
        }
        action_result(out, false, true, "ui", action, page, "不支持切换到这个页面");
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ui_page_cb) {
        action_result(out, false, true, "ui", action, page, "屏幕 UI 未启动，无法切换页面");
        return ESP_ERR_INVALID_STATE;
    }

    char feedback[FRIDGE_AI_ACTION_MAX_MESSAGE_LEN + 1] = {0};
    snprintf(feedback, sizeof(feedback), "已打开%s", page_label(page));
    esp_err_t err = s_ui_page_cb(page, message && message[0] ? message : feedback);
    if (err != ESP_OK) {
        action_result(out, false, true, "ui", action, page, "页面切换投递失败");
        return err;
    }
    action_result(out, true, false, "ui", action, page, feedback);
    return ESP_OK;
}

void fridge_ai_actions_register_ui_page_handler(fridge_ai_action_ui_page_cb_t cb)
{
    s_ui_page_cb = cb;
}

esp_err_t fridge_ai_actions_execute_json(const char *reply, fridge_ai_action_result_t *out)
{
    ESP_RETURN_ON_FALSE(reply && out, ESP_ERR_INVALID_ARG, TAG, "invalid action args");
    memset(out, 0, sizeof(*out));

    const char *slice_start = NULL;
    const char *slice_end = NULL;
    char *slice = json_slice_alloc(reply, &slice_start, &slice_end);
    (void)slice_start;
    (void)slice_end;
    if (!slice) {
        action_result(out, false, true, "", "", "", "没有找到可执行的 AI 动作指令");
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *root = cJSON_Parse(slice);
    free(slice);
    if (!root) {
        action_result(out, false, true, "", "", "", "AI 动作指令不是有效 JSON");
        return ESP_ERR_INVALID_ARG;
    }

    const char *tool = json_string(root, "tool");
    const char *type = json_string(root, "type");
    const char *effective_tool = tool && tool[0] ? tool : type;
    if (effective_tool && (strcmp(effective_tool, "ui") == 0 || strcmp(effective_tool, "ui_control") == 0)) {
        const cJSON *field = NULL;
        cJSON_ArrayForEach(field, root) {
            if (!ui_json_key_allowed(field->string)) {
                cJSON_Delete(root);
                action_result(out, false, true, "ui", "", "", "UI 指令包含不支持的字段");
                return ESP_ERR_INVALID_ARG;
            }
        }
        esp_err_t err = execute_ui_json(root, out);
        cJSON_Delete(root);
        return err;
    }

    cJSON_Delete(root);
    fridge_kitchen_tools_ai_result_t kitchen = {0};
    esp_err_t err = fridge_kitchen_tools_execute_ai_json(reply, &kitchen);
    action_result(out,
                  err == ESP_OK && kitchen.executed,
                  kitchen.needs_confirmation,
                  kitchen.tool,
                  kitchen.action,
                  "",
                  kitchen.message);
    return err;
}

esp_err_t fridge_ai_actions_strip_directives(const char *reply,
                                             char *visible,
                                             size_t visible_size,
                                             fridge_ai_action_result_t *out)
{
    ESP_RETURN_ON_FALSE(reply && visible && visible_size > 0 && out, ESP_ERR_INVALID_ARG, TAG, "invalid strip args");
    visible[0] = '\0';
    memset(out, 0, sizeof(*out));

    const char *slice_start = NULL;
    const char *slice_end = NULL;
    char *slice = json_slice_alloc(reply, &slice_start, &slice_end);
    if (!slice) {
        strlcpy(visible, reply, visible_size);
        action_result(out, false, false, "", "", "", "");
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = fridge_ai_actions_execute_json(reply, out);
    free(slice);
    if (err == ESP_OK && out->executed && slice_start && slice_end) {
        size_t prefix_len = (size_t)(slice_start - reply);
        while (prefix_len > 0 && isspace((unsigned char)reply[prefix_len - 1])) {
            prefix_len--;
        }
        if (prefix_len >= visible_size) {
            prefix_len = visible_size - 1;
        }
        memcpy(visible, reply, prefix_len);
        visible[prefix_len] = '\0';

        const char *suffix = slice_end;
        while (*suffix && isspace((unsigned char)*suffix)) {
            suffix++;
        }
        if (*suffix) {
            if (visible[0] && strlen(visible) + 1 < visible_size) {
                strlcat(visible, "\n", visible_size);
            }
            strlcat(visible, suffix, visible_size);
        }
        if (visible[0] == '\0' && out->message[0] != '\0') {
            strlcpy(visible, out->message, visible_size);
        }
        return ESP_OK;
    }

    strlcpy(visible, reply, visible_size);
    return err;
}
