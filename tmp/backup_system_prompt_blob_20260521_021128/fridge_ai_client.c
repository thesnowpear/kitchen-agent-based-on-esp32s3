// 冰箱小精灵 AI API 客户端组件。
// 负责保存开发阶段的 OpenAI-compatible API 配置，并通过 HTTPS 发起文本聊天测试。
// 硬件注意：本组件不控制 GPIO；测试请求需要 Wi-Fi 与 TLS，Wi-Fi 发射峰值仍要求稳定 USB/5V 供电。

#include "fridge_ai_client.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fridge_network.h"
#include "nvs.h"

#define AI_NVS_NAMESPACE "fridge_ai"
#define AI_NVS_KEY_BASE_URL "base_url"
#define AI_NVS_KEY_API_KEY "api_key"
#define AI_NVS_KEY_MODEL "model"
#define AI_NVS_KEY_SYSTEM "system"
#define AI_NVS_KEY_TIMEOUT "timeout_ms"
#define AI_NVS_KEY_ACTIVE_PROFILE "active"
#define AI_HTTP_RESPONSE_CAP 4096
#define AI_TEST_MAX_TOKENS 128
#define AI_COMPLETIONS_PATH "/chat/completions"

static const char *TAG = "fridge_ai";

static bool s_initialized;
static char s_last_error[FRIDGE_AI_MAX_ERROR_LEN + 1];

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool overflow;
} ai_http_buffer_t;

static void set_last_error(const char *message)
{
    strlcpy(s_last_error, message ? message : "", sizeof(s_last_error));
}

static uint32_t clamp_timeout_ms(uint32_t timeout_ms)
{
    if (timeout_ms < 5000) {
        return 5000;
    }
    if (timeout_ms > 60000) {
        return 60000;
    }
    return timeout_ms;
}

static bool starts_with(const char *text, const char *prefix)
{
    return text && prefix && strncmp(text, prefix, strlen(prefix)) == 0;
}

static size_t json_escaped_len(const char *text)
{
    size_t len = 0;
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
        switch (*p) {
        case '"':
        case '\\':
        case '\n':
        case '\r':
        case '\t':
            len += 2;
            break;
        default:
            len += (*p < 0x20) ? 6 : 1;
            break;
        }
    }
    return len;
}

static char *json_escape_alloc(const char *text)
{
    size_t len = json_escaped_len(text);
    char *out = calloc(1, len + 1);
    if (!out) {
        return NULL;
    }

    char *w = out;
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
        switch (*p) {
        case '"':
            *w++ = '\\';
            *w++ = '"';
            break;
        case '\\':
            *w++ = '\\';
            *w++ = '\\';
            break;
        case '\n':
            *w++ = '\\';
            *w++ = 'n';
            break;
        case '\r':
            *w++ = '\\';
            *w++ = 'r';
            break;
        case '\t':
            *w++ = '\\';
            *w++ = 't';
            break;
        default:
            if (*p < 0x20) {
                snprintf(w, 7, "\\u%04x", *p);
                w += 6;
            } else {
                *w++ = (char)*p;
            }
            break;
        }
    }
    *w = '\0';
    return out;
}

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static bool append_utf8(char *out, size_t out_size, size_t *written, uint32_t codepoint)
{
    if (!out || !written || *written >= out_size) {
        return false;
    }

    unsigned char bytes[4];
    size_t count = 0;
    if (codepoint <= 0x7F) {
        bytes[count++] = (unsigned char)codepoint;
    } else if (codepoint <= 0x7FF) {
        bytes[count++] = (unsigned char)(0xC0 | (codepoint >> 6));
        bytes[count++] = (unsigned char)(0x80 | (codepoint & 0x3F));
    } else if (codepoint <= 0xFFFF) {
        bytes[count++] = (unsigned char)(0xE0 | (codepoint >> 12));
        bytes[count++] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3F));
        bytes[count++] = (unsigned char)(0x80 | (codepoint & 0x3F));
    } else if (codepoint <= 0x10FFFF) {
        bytes[count++] = (unsigned char)(0xF0 | (codepoint >> 18));
        bytes[count++] = (unsigned char)(0x80 | ((codepoint >> 12) & 0x3F));
        bytes[count++] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3F));
        bytes[count++] = (unsigned char)(0x80 | (codepoint & 0x3F));
    }

    if (*written + count >= out_size) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        out[(*written)++] = (char)bytes[i];
    }
    return true;
}

static const char *find_json_key(const char *json, const char *key)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *pos = strstr(json, pattern);
    if (!pos) {
        return NULL;
    }
    pos += strlen(pattern);
    while (*pos && isspace((unsigned char)*pos)) {
        pos++;
    }
    if (*pos != ':') {
        return NULL;
    }
    pos++;
    while (*pos && isspace((unsigned char)*pos)) {
        pos++;
    }
    return pos;
}

static bool json_get_string_value(const char *json, const char *key, char *out, size_t out_size)
{
    const char *pos = find_json_key(json, key);
    if (!pos || *pos != '"' || !out || out_size == 0) {
        return false;
    }
    pos++;

    size_t written = 0;
    while (*pos && *pos != '"') {
        unsigned char ch = (unsigned char)*pos++;
        if (ch == '\\' && *pos) {
            char esc = *pos++;
            if (esc == 'n') {
                ch = '\n';
            } else if (esc == 'r') {
                ch = '\r';
            } else if (esc == 't') {
                ch = '\t';
            } else if (esc == 'u') {
                uint32_t codepoint = 0;
                bool valid = true;
                for (int i = 0; i < 4; i++) {
                    int value = hex_value(*pos++);
                    if (value < 0) {
                        valid = false;
                        break;
                    }
                    codepoint = (codepoint << 4) | (uint32_t)value;
                }
                if (valid) {
                    append_utf8(out, out_size, &written, codepoint);
                    continue;
                }
                ch = '?';
            } else {
                ch = (unsigned char)esc;
            }
        }
        if (written + 1 < out_size) {
            out[written++] = (char)ch;
        }
    }
    out[written] = '\0';
    return true;
}

static esp_err_t open_ai_nvs(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "NVS handle is NULL");
    return nvs_open(AI_NVS_NAMESPACE, mode, handle);
}

static void read_nvs_string(nvs_handle_t handle, const char *key, char *out, size_t out_size, const char *fallback)
{
    if (!out || out_size == 0) {
        return;
    }

    size_t len = out_size;
    esp_err_t err = nvs_get_str(handle, key, out, &len);
    if (err != ESP_OK) {
        strlcpy(out, fallback ? fallback : "", out_size);
    }
}

static uint8_t clamp_profile_id(uint8_t profile_id)
{
    return profile_id < FRIDGE_AI_MAX_PROFILES ? profile_id : 0;
}

static void make_profile_key(uint8_t profile_id, const char *field, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    profile_id = clamp_profile_id(profile_id);

    // 0 号配置沿用旧 NVS key，保证已保存的开发 Key 不会因为升级丢失。
    if (profile_id == 0) {
        if (strcmp(field, "base") == 0) {
            strlcpy(out, AI_NVS_KEY_BASE_URL, out_size);
        } else if (strcmp(field, "key") == 0) {
            strlcpy(out, AI_NVS_KEY_API_KEY, out_size);
        } else if (strcmp(field, "model") == 0) {
            strlcpy(out, AI_NVS_KEY_MODEL, out_size);
        } else if (strcmp(field, "system") == 0) {
            strlcpy(out, AI_NVS_KEY_SYSTEM, out_size);
        } else if (strcmp(field, "timeout") == 0) {
            strlcpy(out, AI_NVS_KEY_TIMEOUT, out_size);
        } else {
            strlcpy(out, "name0", out_size);
        }
        return;
    }

    char prefix = 'n';
    if (strcmp(field, "base") == 0) {
        prefix = 'b';
    } else if (strcmp(field, "key") == 0) {
        prefix = 'k';
    } else if (strcmp(field, "model") == 0) {
        prefix = 'm';
    } else if (strcmp(field, "system") == 0) {
        prefix = 's';
    } else if (strcmp(field, "timeout") == 0) {
        prefix = 't';
    }
    snprintf(out, out_size, "%c%u", prefix, (unsigned)profile_id);
}

static uint8_t get_active_profile_id(void)
{
    nvs_handle_t handle;
    uint8_t active = 0;
    if (open_ai_nvs(NVS_READONLY, &handle) == ESP_OK) {
        uint8_t stored = 0;
        if (nvs_get_u8(handle, AI_NVS_KEY_ACTIVE_PROFILE, &stored) == ESP_OK) {
            active = clamp_profile_id(stored);
        }
        nvs_close(handle);
    }
    return active;
}

static bool profile_exists_in_handle(nvs_handle_t handle, uint8_t profile_id)
{
    if (profile_id == 0) {
        return true;
    }

    char key[16];
    size_t len = 0;
    make_profile_key(profile_id, "name", key, sizeof(key));
    if (nvs_get_str(handle, key, NULL, &len) == ESP_OK) {
        return true;
    }

    len = 0;
    make_profile_key(profile_id, "base", key, sizeof(key));
    if (nvs_get_str(handle, key, NULL, &len) == ESP_OK) {
        return true;
    }

    len = 0;
    make_profile_key(profile_id, "key", key, sizeof(key));
    return nvs_get_str(handle, key, NULL, &len) == ESP_OK;
}

static void default_profile_name(uint8_t profile_id, char *out, size_t out_size)
{
    if (profile_id == 0) {
        strlcpy(out, "默认配置", out_size);
    } else {
        snprintf(out, out_size, "备用配置 %u", (unsigned)profile_id);
    }
}

static void make_key_preview(const char *key, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!key || key[0] == '\0') {
        return;
    }

    size_t len = strlen(key);
    if (len <= 8) {
        strlcpy(out, "已保存", out_size);
        return;
    }

    const char *suffix = key + len - 4;
    int prefix_len = starts_with(key, "sk-") ? 3 : 4;
    snprintf(out, out_size, "%.*s...%s", prefix_len, key, suffix);
}

static esp_err_t load_profile_config(uint8_t profile_id, fridge_ai_config_update_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    memset(config, 0, sizeof(*config));
    profile_id = clamp_profile_id(profile_id);
    config->profile_id = profile_id;

    nvs_handle_t handle;
    esp_err_t err = open_ai_nvs(NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        default_profile_name(profile_id, config->profile_name, sizeof(config->profile_name));
        strlcpy(config->model, FRIDGE_AI_DEFAULT_MODEL, sizeof(config->model));
        strlcpy(config->system_prompt, "你是冰箱小精灵的开发测试助手，请用简短中文回答。", sizeof(config->system_prompt));
        config->timeout_ms = FRIDGE_AI_DEFAULT_TIMEOUT_MS;
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open AI NVS failed");

    char key[16];
    make_profile_key(profile_id, "name", key, sizeof(key));
    default_profile_name(profile_id, config->profile_name, sizeof(config->profile_name));
    read_nvs_string(handle, key, config->profile_name, sizeof(config->profile_name), config->profile_name);

    make_profile_key(profile_id, "base", key, sizeof(key));
    read_nvs_string(handle, key, config->api_base_url, sizeof(config->api_base_url), "");
    make_profile_key(profile_id, "model", key, sizeof(key));
    read_nvs_string(handle, key, config->model, sizeof(config->model), FRIDGE_AI_DEFAULT_MODEL);
    make_profile_key(profile_id, "system", key, sizeof(key));
    read_nvs_string(handle, key, config->system_prompt, sizeof(config->system_prompt),
                    "你是冰箱小精灵的开发测试助手，请用简短中文回答。");
    size_t key_len = sizeof(config->api_key);
    make_profile_key(profile_id, "key", key, sizeof(key));
    esp_err_t key_err = nvs_get_str(handle, key, config->api_key, &key_len);
    config->update_api_key = (key_err == ESP_OK && config->api_key[0] != '\0');
    uint32_t timeout_ms = FRIDGE_AI_DEFAULT_TIMEOUT_MS;
    make_profile_key(profile_id, "timeout", key, sizeof(key));
    if (nvs_get_u32(handle, key, &timeout_ms) != ESP_OK) {
        timeout_ms = FRIDGE_AI_DEFAULT_TIMEOUT_MS;
    }
    config->timeout_ms = clamp_timeout_ms(timeout_ms);
    nvs_close(handle);
    return ESP_OK;
}

static esp_err_t load_full_config(fridge_ai_config_update_t *config)
{
    return load_profile_config(get_active_profile_id(), config);
}

static esp_err_t compose_chat_url(const char *base_url, char *out, size_t out_size)
{
    ESP_RETURN_ON_FALSE(base_url && out && out_size > 0, ESP_ERR_INVALID_ARG, TAG, "invalid URL args");
    ESP_RETURN_ON_FALSE(starts_with(base_url, "https://"), ESP_ERR_INVALID_ARG, TAG, "AI base URL must use https");

    strlcpy(out, base_url, out_size);
    size_t len = strlen(out);
    while (len > 0 && out[len - 1] == '/') {
        out[--len] = '\0';
    }

    ESP_RETURN_ON_FALSE(strlen(out) + strlen(AI_COMPLETIONS_PATH) + 1 < out_size,
                        ESP_FAIL, TAG, "AI URL too long");
    strlcat(out, AI_COMPLETIONS_PATH, out_size);
    return ESP_OK;
}

static esp_err_t ai_http_event_handler(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || !event->user_data || !event->data || event->data_len <= 0) {
        return ESP_OK;
    }

    ai_http_buffer_t *buffer = (ai_http_buffer_t *)event->user_data;
    size_t available = buffer->cap > buffer->len ? buffer->cap - buffer->len - 1 : 0;
    size_t copy_len = (size_t)event->data_len;
    if (copy_len > available) {
        copy_len = available;
        buffer->overflow = true;
    }
    if (copy_len > 0) {
        memcpy(buffer->data + buffer->len, event->data, copy_len);
        buffer->len += copy_len;
        buffer->data[buffer->len] = '\0';
    }
    return ESP_OK;
}

static char *build_chat_request(const fridge_ai_config_update_t *config, const char *message)
{
    char *model = json_escape_alloc(config->model);
    char *system_prompt = json_escape_alloc(config->system_prompt);
    char *user_message = json_escape_alloc(message);
    if (!model || !system_prompt || !user_message) {
        free(model);
        free(system_prompt);
        free(user_message);
        return NULL;
    }

    size_t body_len = strlen(model) + strlen(system_prompt) + strlen(user_message) + 256;
    char *body = calloc(1, body_len);
    if (body) {
        snprintf(body, body_len,
                 "{\"model\":\"%s\",\"messages\":["
                 "{\"role\":\"system\",\"content\":\"%s\"},"
                 "{\"role\":\"user\",\"content\":\"%s\"}],"
                 "\"temperature\":0.2,\"max_tokens\":%d,\"stream\":false}",
                 model, system_prompt, user_message, AI_TEST_MAX_TOKENS);
    }
    free(model);
    free(system_prompt);
    free(user_message);
    return body;
}

static esp_err_t parse_chat_response(const char *response, fridge_ai_chat_result_t *out)
{
    if (!response || response[0] == '\0') {
        strlcpy(out->error, "AI 响应为空", sizeof(out->error));
        set_last_error(out->error);
        return ESP_FAIL;
    }

    char error_message[FRIDGE_AI_MAX_ERROR_LEN + 1] = {0};
    if (strstr(response, "\"error\"") && json_get_string_value(response, "message", error_message, sizeof(error_message))) {
        strlcpy(out->error, error_message, sizeof(out->error));
        set_last_error(out->error);
        return ESP_FAIL;
    }

    json_get_string_value(response, "model", out->model, sizeof(out->model));
    if (!json_get_string_value(response, "content", out->reply, sizeof(out->reply)) || out->reply[0] == '\0') {
        if (!json_get_string_value(response, "reasoning_content", out->reply, sizeof(out->reply)) || out->reply[0] == '\0') {
            if (!json_get_string_value(response, "text", out->reply, sizeof(out->reply)) || out->reply[0] == '\0') {
                strlcpy(out->error, "AI 响应缺少 choices[0].message.content", sizeof(out->error));
                set_last_error(out->error);
                return ESP_FAIL;
            }
        }
    }

    strlcpy(out->status, "ok", sizeof(out->status));
    set_last_error("");
    return ESP_OK;
}

static void set_http_status_error(const char *response, fridge_ai_chat_result_t *out)
{
    char service_message[FRIDGE_AI_MAX_ERROR_LEN + 1] = {0};
    if (response && json_get_string_value(response, "message", service_message, sizeof(service_message)) && service_message[0] != '\0') {
        snprintf(out->error, sizeof(out->error), "AI HTTP %d：", out->http_status);
        strlcat(out->error, service_message, sizeof(out->error));
    } else {
        snprintf(out->error, sizeof(out->error), "AI HTTP 状态异常：%d", out->http_status);
    }
    set_last_error(out->error);
}

esp_err_t fridge_ai_client_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    nvs_handle_t handle;
    esp_err_t err = open_ai_nvs(NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        nvs_close(handle);
        s_initialized = true;
        return ESP_OK;
    }
    set_last_error("AI NVS 初始化失败");
    return err;
}

esp_err_t fridge_ai_client_get_config(fridge_ai_config_view_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    memset(out, 0, sizeof(*out));

    fridge_ai_config_update_t config = {0};
    ESP_RETURN_ON_ERROR(load_full_config(&config), TAG, "load AI config failed");

    out->profile_id = config.profile_id;
    strlcpy(out->profile_name, config.profile_name, sizeof(out->profile_name));
    strlcpy(out->api_base_url, config.api_base_url, sizeof(out->api_base_url));
    strlcpy(out->model, config.model, sizeof(out->model));
    strlcpy(out->system_prompt, config.system_prompt, sizeof(out->system_prompt));
    out->timeout_ms = clamp_timeout_ms(config.timeout_ms);
    out->has_api_key = config.api_key[0] != '\0';
    out->ready = out->api_base_url[0] != '\0' && out->model[0] != '\0' && out->has_api_key;
    make_key_preview(config.api_key, out->api_key_preview, sizeof(out->api_key_preview));
    strlcpy(out->last_error, s_last_error, sizeof(out->last_error));
    return ESP_OK;
}

esp_err_t fridge_ai_client_set_config(const fridge_ai_config_update_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_ERROR(fridge_ai_client_init(), TAG, "AI client init failed");

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(open_ai_nvs(NVS_READWRITE, &handle), TAG, "open AI NVS failed");

    uint8_t profile_id = clamp_profile_id(config->profile_id);
    char key[16];
    char profile_name[FRIDGE_AI_MAX_PROFILE_NAME_LEN + 1] = {0};
    if (config->profile_name[0]) {
        strlcpy(profile_name, config->profile_name, sizeof(profile_name));
    } else {
        default_profile_name(profile_id, profile_name, sizeof(profile_name));
    }

    make_profile_key(profile_id, "name", key, sizeof(key));
    esp_err_t err = nvs_set_str(handle, key, profile_name);
    if (err == ESP_OK) {
        make_profile_key(profile_id, "base", key, sizeof(key));
        err = nvs_set_str(handle, key, config->api_base_url);
    }
    if (err == ESP_OK) {
        make_profile_key(profile_id, "model", key, sizeof(key));
        err = nvs_set_str(handle, key, config->model[0] ? config->model : FRIDGE_AI_DEFAULT_MODEL);
    }
    if (err == ESP_OK) {
        make_profile_key(profile_id, "system", key, sizeof(key));
        err = nvs_set_str(handle, key, config->system_prompt);
    }
    if (err == ESP_OK) {
        make_profile_key(profile_id, "timeout", key, sizeof(key));
        err = nvs_set_u32(handle, key, clamp_timeout_ms(config->timeout_ms));
    }
    if (err == ESP_OK && config->update_api_key) {
        make_profile_key(profile_id, "key", key, sizeof(key));
        err = nvs_set_str(handle, key, config->api_key);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, AI_NVS_KEY_ACTIVE_PROFILE, profile_id);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        set_last_error("");
        ESP_LOGI(TAG, "AI config profile %u saved, key_updated=%s", (unsigned)profile_id, config->update_api_key ? "yes" : "no");
    } else {
        set_last_error("AI 配置保存失败");
    }
    return err;
}

esp_err_t fridge_ai_client_clear_key(void)
{
    ESP_RETURN_ON_ERROR(fridge_ai_client_init(), TAG, "AI client init failed");

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(open_ai_nvs(NVS_READWRITE, &handle), TAG, "open AI NVS failed");
    char key[16];
    make_profile_key(get_active_profile_id(), "key", key, sizeof(key));
    esp_err_t err = nvs_erase_key(handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        set_last_error("API Key 已清除");
    }
    return err;
}

esp_err_t fridge_ai_client_get_profiles(fridge_ai_profile_list_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    memset(out, 0, sizeof(*out));

    out->active_profile_id = get_active_profile_id();
    nvs_handle_t handle = 0;
    bool has_handle = false;
    esp_err_t err = open_ai_nvs(NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    } else {
        ESP_RETURN_ON_ERROR(err, TAG, "open AI NVS failed");
        has_handle = true;
    }

    for (uint8_t id = 0; id < FRIDGE_AI_MAX_PROFILES; id++) {
        bool exists = (id == 0);
        if (has_handle) {
            exists = profile_exists_in_handle(handle, id);
        }
        if (!exists) {
            continue;
        }

        fridge_ai_config_update_t config = {0};
        ESP_RETURN_ON_ERROR(load_profile_config(id, &config), TAG, "load AI profile failed");
        fridge_ai_config_view_t *view = &out->profiles[out->count++];
        view->profile_id = config.profile_id;
        strlcpy(view->profile_name, config.profile_name, sizeof(view->profile_name));
        strlcpy(view->api_base_url, config.api_base_url, sizeof(view->api_base_url));
        strlcpy(view->model, config.model, sizeof(view->model));
        strlcpy(view->system_prompt, config.system_prompt, sizeof(view->system_prompt));
        view->timeout_ms = clamp_timeout_ms(config.timeout_ms);
        view->has_api_key = config.api_key[0] != '\0';
        view->ready = view->api_base_url[0] != '\0' && view->model[0] != '\0' && view->has_api_key;
        make_key_preview(config.api_key, view->api_key_preview, sizeof(view->api_key_preview));
        strlcpy(view->last_error, s_last_error, sizeof(view->last_error));
    }

    if (has_handle) {
        nvs_close(handle);
    }
    return ESP_OK;
}

esp_err_t fridge_ai_client_create_profile(const char *profile_name, fridge_ai_config_view_t *out)
{
    ESP_RETURN_ON_ERROR(fridge_ai_client_init(), TAG, "AI client init failed");

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(open_ai_nvs(NVS_READWRITE, &handle), TAG, "open AI NVS failed");

    uint8_t new_id = FRIDGE_AI_MAX_PROFILES;
    for (uint8_t id = 1; id < FRIDGE_AI_MAX_PROFILES; id++) {
        if (!profile_exists_in_handle(handle, id)) {
            new_id = id;
            break;
        }
    }
    nvs_close(handle);
    ESP_RETURN_ON_FALSE(new_id < FRIDGE_AI_MAX_PROFILES, ESP_ERR_NO_MEM, TAG, "AI profile slots are full");

    fridge_ai_config_update_t config = {0};
    config.profile_id = new_id;
    if (profile_name && profile_name[0]) {
        strlcpy(config.profile_name, profile_name, sizeof(config.profile_name));
    } else {
        default_profile_name(new_id, config.profile_name, sizeof(config.profile_name));
    }
    strlcpy(config.model, FRIDGE_AI_DEFAULT_MODEL, sizeof(config.model));
    strlcpy(config.system_prompt, "你是冰箱小精灵的开发测试助手，请用简短中文回答。", sizeof(config.system_prompt));
    config.timeout_ms = FRIDGE_AI_DEFAULT_TIMEOUT_MS;

    ESP_RETURN_ON_ERROR(fridge_ai_client_set_config(&config), TAG, "create AI profile failed");
    return out ? fridge_ai_client_get_config(out) : ESP_OK;
}

esp_err_t fridge_ai_client_select_profile(uint8_t profile_id, fridge_ai_config_view_t *out)
{
    ESP_RETURN_ON_ERROR(fridge_ai_client_init(), TAG, "AI client init failed");
    profile_id = clamp_profile_id(profile_id);

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(open_ai_nvs(NVS_READWRITE, &handle), TAG, "open AI NVS failed");
    bool exists = profile_exists_in_handle(handle, profile_id);
    esp_err_t err = exists ? nvs_set_u8(handle, AI_NVS_KEY_ACTIVE_PROFILE, profile_id) : ESP_ERR_NOT_FOUND;
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    ESP_RETURN_ON_ERROR(err, TAG, "select AI profile failed");
    return out ? fridge_ai_client_get_config(out) : ESP_OK;
}

esp_err_t fridge_ai_client_delete_profile(uint8_t profile_id, fridge_ai_config_view_t *out)
{
    ESP_RETURN_ON_ERROR(fridge_ai_client_init(), TAG, "AI client init failed");
    ESP_RETURN_ON_FALSE(profile_id > 0 && profile_id < FRIDGE_AI_MAX_PROFILES, ESP_ERR_INVALID_ARG, TAG, "default AI profile cannot be deleted");

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(open_ai_nvs(NVS_READWRITE, &handle), TAG, "open AI NVS failed");

    char key[16];
    const char *fields[] = {"name", "base", "key", "model", "system", "timeout"};
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        make_profile_key(profile_id, fields[i], key, sizeof(key));
        esp_err_t erase_err = nvs_erase_key(handle, key);
        if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
            err = erase_err;
            break;
        }
    }
    if (err == ESP_OK && get_active_profile_id() == profile_id) {
        err = nvs_set_u8(handle, AI_NVS_KEY_ACTIVE_PROFILE, 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    ESP_RETURN_ON_ERROR(err, TAG, "delete AI profile failed");
    return out ? fridge_ai_client_get_config(out) : ESP_OK;
}

esp_err_t fridge_ai_client_test_chat(const char *message, fridge_ai_chat_result_t *out)
{
    ESP_RETURN_ON_FALSE(message && message[0] != '\0' && out, ESP_ERR_INVALID_ARG, TAG, "invalid chat args");
    memset(out, 0, sizeof(*out));
    int64_t start_us = esp_timer_get_time();

    fridge_network_status_t net = {0};
    fridge_network_get_status(&net);
    if (!net.connected) {
        strlcpy(out->error, "Wi-Fi 未连接，请先在 Web 面板完成配网", sizeof(out->error));
        set_last_error(out->error);
        return ESP_ERR_INVALID_STATE;
    }
    if (!net.internet_ready) {
        esp_err_t sync_err = fridge_network_sync_time();
        fridge_network_get_status(&net);
        if (sync_err != ESP_OK || !net.internet_ready) {
            strlcpy(out->error, "网络未校时或外网不可用，请确认 SNTP/互联网连接", sizeof(out->error));
            set_last_error(out->error);
            return ESP_ERR_INVALID_STATE;
        }
    }

    fridge_ai_config_update_t config = {0};
    ESP_RETURN_ON_ERROR(load_full_config(&config), TAG, "load AI config failed");
    if (config.api_base_url[0] == '\0') {
        strlcpy(out->error, "缺少 API Base URL", sizeof(out->error));
        set_last_error(out->error);
        return ESP_ERR_INVALID_STATE;
    }
    if (config.api_key[0] == '\0') {
        strlcpy(out->error, "缺少 API Key，请先在 AI API 页面保存", sizeof(out->error));
        set_last_error(out->error);
        return ESP_ERR_INVALID_STATE;
    }

    char url[FRIDGE_AI_MAX_BASE_URL_LEN + sizeof(AI_COMPLETIONS_PATH) + 8] = {0};
    esp_err_t err = compose_chat_url(config.api_base_url, url, sizeof(url));
    if (err != ESP_OK) {
        strlcpy(out->error, "API Base URL 必须使用 https://，并建议以 /v1 结尾", sizeof(out->error));
        set_last_error(out->error);
        return err;
    }

    char *request_body = build_chat_request(&config, message);
    if (!request_body) {
        strlcpy(out->error, "构造 AI 请求 JSON 失败", sizeof(out->error));
        set_last_error(out->error);
        return ESP_ERR_NO_MEM;
    }

    char *response_body = calloc(1, AI_HTTP_RESPONSE_CAP);
    if (!response_body) {
        free(request_body);
        strlcpy(out->error, "分配 AI 响应缓冲失败", sizeof(out->error));
        set_last_error(out->error);
        return ESP_ERR_NO_MEM;
    }

    ai_http_buffer_t rx = {
        .data = response_body,
        .len = 0,
        .cap = AI_HTTP_RESPONSE_CAP,
        .overflow = false,
    };
    esp_http_client_config_t http_config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = (int)clamp_timeout_ms(config.timeout_ms),
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = ai_http_event_handler,
        .user_data = &rx,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        free(request_body);
        free(response_body);
        strlcpy(out->error, "初始化 HTTP 客户端失败", sizeof(out->error));
        set_last_error(out->error);
        return ESP_FAIL;
    }

    char auth_header[FRIDGE_AI_MAX_API_KEY_LEN + 16] = {0};
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", config.api_key);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_post_field(client, request_body, (int)strlen(request_body));

    err = esp_http_client_perform(client);
    out->http_status = esp_http_client_get_status_code(client);
    out->latency_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
    strlcpy(out->model, config.model, sizeof(out->model));
    esp_http_client_cleanup(client);
    free(request_body);

    if (err != ESP_OK) {
        snprintf(out->error, sizeof(out->error), "HTTP 请求失败：%s", esp_err_to_name(err));
        set_last_error(out->error);
        free(response_body);
        return err;
    }
    if (rx.overflow) {
        strlcpy(out->error, "AI 响应过长，串口测试缓冲已截断", sizeof(out->error));
        set_last_error(out->error);
        free(response_body);
        return ESP_FAIL;
    }
    if (out->http_status < 200 || out->http_status >= 300) {
        set_http_status_error(response_body, out);
        free(response_body);
        return ESP_FAIL;
    }

    err = parse_chat_response(response_body, out);
    free(response_body);
    return err;
}
