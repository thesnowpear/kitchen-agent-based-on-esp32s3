// 冰箱小精灵 ASR 客户端组件。
// 保存独立语音转文字配置，并把 INMP441 录音封装成 WAV multipart 上传到兼容 /audio/transcriptions 的 HTTPS API。
#include "fridge_asr.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fridge_audio.h"
#include "fridge_network.h"
#include "nvs.h"

#define ASR_NVS_NAMESPACE "fridge_asr"
#define ASR_NVS_KEY_URL "url"
#define ASR_NVS_KEY_MODEL "model"
#define ASR_NVS_KEY_KEY "key"
#define ASR_NVS_KEY_TIMEOUT "timeout"
#define ASR_RESPONSE_CAP 4096
#define WAV_HEADER_BYTES 44

static const char *TAG = "fridge_asr";
static bool s_initialized;
static char s_last_error[FRIDGE_ASR_MAX_ERROR_LEN + 1];

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool overflow;
} asr_http_buffer_t;

static void set_last_error(const char *message)
{
    strlcpy(s_last_error, message ? message : "", sizeof(s_last_error));
}

static uint32_t clamp_timeout_ms(uint32_t timeout_ms)
{
    if (timeout_ms < 10000) {
        return 10000;
    }
    if (timeout_ms > 90000) {
        return 90000;
    }
    return timeout_ms;
}

static bool starts_with(const char *text, const char *prefix)
{
    return text && prefix && strncmp(text, prefix, strlen(prefix)) == 0;
}

static esp_err_t open_asr_nvs(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "NVS handle is NULL");
    return nvs_open(ASR_NVS_NAMESPACE, mode, handle);
}

static void read_nvs_string(nvs_handle_t handle, const char *key, char *out, size_t out_size, const char *fallback)
{
    if (!out || out_size == 0) {
        return;
    }
    size_t len = out_size;
    if (nvs_get_str(handle, key, out, &len) != ESP_OK) {
        strlcpy(out, fallback ? fallback : "", out_size);
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
        strlcpy(out, "saved", out_size);
        return;
    }
    snprintf(out, out_size, "%.*s...%s", starts_with(key, "sk-") ? 3 : 4, key, key + len - 4);
}

static esp_err_t load_config(fridge_asr_config_update_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    memset(config, 0, sizeof(*config));
    strlcpy(config->api_base_url, FRIDGE_ASR_DEFAULT_URL, sizeof(config->api_base_url));
    strlcpy(config->model, FRIDGE_ASR_DEFAULT_MODEL, sizeof(config->model));
    config->timeout_ms = FRIDGE_ASR_DEFAULT_TIMEOUT_MS;

    nvs_handle_t handle;
    esp_err_t err = open_asr_nvs(NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open ASR NVS failed");

    read_nvs_string(handle, ASR_NVS_KEY_URL, config->api_base_url, sizeof(config->api_base_url), FRIDGE_ASR_DEFAULT_URL);
    read_nvs_string(handle, ASR_NVS_KEY_MODEL, config->model, sizeof(config->model), FRIDGE_ASR_DEFAULT_MODEL);
    size_t key_len = sizeof(config->api_key);
    config->update_api_key = nvs_get_str(handle, ASR_NVS_KEY_KEY, config->api_key, &key_len) == ESP_OK && config->api_key[0] != '\0';
    uint32_t timeout_ms = FRIDGE_ASR_DEFAULT_TIMEOUT_MS;
    if (nvs_get_u32(handle, ASR_NVS_KEY_TIMEOUT, &timeout_ms) == ESP_OK) {
        config->timeout_ms = clamp_timeout_ms(timeout_ms);
    }
    nvs_close(handle);
    return ESP_OK;
}

static esp_err_t asr_http_event_handler(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || !event->user_data || !event->data || event->data_len <= 0) {
        return ESP_OK;
    }
    asr_http_buffer_t *buffer = (asr_http_buffer_t *)event->user_data;
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
    } else {
        return false;
    }
    if (*written + count >= out_size) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        out[(*written)++] = (char)bytes[i];
    }
    return true;
}

static bool json_get_text_field(const char *json, char *out, size_t out_size)
{
    const char *pos = strstr(json ? json : "", "\"text\"");
    if (!pos || !out || out_size == 0) {
        return false;
    }
    pos = strchr(pos, ':');
    if (!pos) {
        return false;
    }
    pos++;
    while (*pos && isspace((unsigned char)*pos)) {
        pos++;
    }
    if (*pos != '"') {
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
                uint32_t cp = 0;
                bool valid = true;
                for (int i = 0; i < 4; i++) {
                    int value = hex_value(*pos++);
                    if (value < 0) {
                        valid = false;
                        break;
                    }
                    cp = (cp << 4) | (uint32_t)value;
                }
                if (valid) {
                    append_utf8(out, out_size, &written, cp);
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
    return written > 0;
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static void write_wav_header(uint8_t *out, size_t pcm_bytes)
{
    memcpy(out, "RIFF", 4);
    put_le32(out + 4, (uint32_t)(36 + pcm_bytes));
    memcpy(out + 8, "WAVEfmt ", 8);
    put_le32(out + 16, 16);
    put_le16(out + 20, 1);
    put_le16(out + 22, 1);
    put_le32(out + 24, FRIDGE_AUDIO_SAMPLE_RATE);
    put_le32(out + 28, FRIDGE_AUDIO_SAMPLE_RATE * 2);
    put_le16(out + 32, 2);
    put_le16(out + 34, 16);
    memcpy(out + 36, "data", 4);
    put_le32(out + 40, (uint32_t)pcm_bytes);
}

static char *build_multipart_body(const fridge_asr_config_update_t *config,
                                  const int16_t *pcm,
                                  size_t pcm_bytes,
                                  size_t *out_len)
{
    const char *boundary = "----fridge-spirit-asr-boundary";
    char prefix[256];
    char file_header[256];
    char suffix[96];
    int prefix_len = snprintf(prefix,
                              sizeof(prefix),
                              "--%s\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n%s\r\n",
                              boundary,
                              config->model);
    int file_header_len = snprintf(file_header,
                                   sizeof(file_header),
                                   "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"voice.wav\"\r\nContent-Type: audio/wav\r\n\r\n",
                                   boundary);
    int suffix_len = snprintf(suffix, sizeof(suffix), "\r\n--%s--\r\n", boundary);
    if (prefix_len <= 0 || file_header_len <= 0 || suffix_len <= 0) {
        return NULL;
    }

    size_t wav_bytes = WAV_HEADER_BYTES + pcm_bytes;
    size_t total = (size_t)prefix_len + (size_t)file_header_len + wav_bytes + (size_t)suffix_len;
    // 6 秒 16kHz/16-bit PCM 接近 192KB，multipart 上传包优先放 PSRAM，
    // 避免和 TLS/HTTP 内部缓冲争抢有限的内部 SRAM。
    char *body = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        body = heap_caps_malloc(total, MALLOC_CAP_8BIT);
    }
    if (!body) {
        return NULL;
    }

    size_t offset = 0;
    memcpy(body + offset, prefix, (size_t)prefix_len);
    offset += (size_t)prefix_len;
    memcpy(body + offset, file_header, (size_t)file_header_len);
    offset += (size_t)file_header_len;
    write_wav_header((uint8_t *)(body + offset), pcm_bytes);
    offset += WAV_HEADER_BYTES;
    memcpy(body + offset, pcm, pcm_bytes);
    offset += pcm_bytes;
    memcpy(body + offset, suffix, (size_t)suffix_len);
    offset += (size_t)suffix_len;

    *out_len = offset;
    return body;
}

esp_err_t fridge_asr_init(void)
{
    s_initialized = true;
    return ESP_OK;
}

esp_err_t fridge_asr_get_config(fridge_asr_config_view_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    memset(out, 0, sizeof(*out));
    fridge_asr_config_update_t config = {0};
    ESP_RETURN_ON_ERROR(load_config(&config), TAG, "load ASR config failed");
    strlcpy(out->api_base_url, config.api_base_url, sizeof(out->api_base_url));
    strlcpy(out->model, config.model, sizeof(out->model));
    out->timeout_ms = clamp_timeout_ms(config.timeout_ms);
    out->has_api_key = config.api_key[0] != '\0';
    out->ready = out->api_base_url[0] != '\0' && out->model[0] != '\0' && out->has_api_key;
    make_key_preview(config.api_key, out->api_key_preview, sizeof(out->api_key_preview));
    strlcpy(out->last_error, s_last_error, sizeof(out->last_error));
    return ESP_OK;
}

esp_err_t fridge_asr_set_config(const fridge_asr_config_update_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_ERROR(fridge_asr_init(), TAG, "ASR init failed");
    ESP_RETURN_ON_FALSE(starts_with(config->api_base_url, "https://"), ESP_ERR_INVALID_ARG, TAG, "ASR URL must use https");

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(open_asr_nvs(NVS_READWRITE, &handle), TAG, "open ASR NVS failed");
    esp_err_t err = nvs_set_str(handle, ASR_NVS_KEY_URL, config->api_base_url[0] ? config->api_base_url : FRIDGE_ASR_DEFAULT_URL);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, ASR_NVS_KEY_MODEL, config->model[0] ? config->model : FRIDGE_ASR_DEFAULT_MODEL);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, ASR_NVS_KEY_TIMEOUT, clamp_timeout_ms(config->timeout_ms));
    }
    if (err == ESP_OK && config->update_api_key) {
        err = nvs_set_str(handle, ASR_NVS_KEY_KEY, config->api_key);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        set_last_error("");
    }
    return err;
}

esp_err_t fridge_asr_clear_key(void)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(open_asr_nvs(NVS_READWRITE, &handle), TAG, "open ASR NVS failed");
    esp_err_t err = nvs_erase_key(handle, ASR_NVS_KEY_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        set_last_error("ASR API Key cleared");
    }
    return err;
}

esp_err_t fridge_asr_transcribe_latest_recording(fridge_asr_result_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    memset(out, 0, sizeof(*out));
    int64_t start_us = esp_timer_get_time();

    fridge_network_status_t net = {0};
    fridge_network_get_status(&net);
    if (!net.connected) {
        strlcpy(out->error, "Wi-Fi is not connected", sizeof(out->error));
        set_last_error(out->error);
        return ESP_ERR_INVALID_STATE;
    }
    if (!net.internet_ready) {
        esp_err_t sync_err = fridge_network_sync_time();
        fridge_network_get_status(&net);
        if (sync_err != ESP_OK || !net.internet_ready) {
            strlcpy(out->error, "network time/internet is not ready", sizeof(out->error));
            set_last_error(out->error);
            return ESP_ERR_INVALID_STATE;
        }
    }

    fridge_asr_config_update_t config = {0};
    ESP_RETURN_ON_ERROR(load_config(&config), TAG, "load ASR config failed");
    if (config.api_base_url[0] == '\0' || !starts_with(config.api_base_url, "https://")) {
        strlcpy(out->error, "ASR URL must use https://", sizeof(out->error));
        set_last_error(out->error);
        return ESP_ERR_INVALID_STATE;
    }
    if (config.api_key[0] == '\0') {
        strlcpy(out->error, "missing ASR API Key", sizeof(out->error));
        set_last_error(out->error);
        return ESP_ERR_INVALID_STATE;
    }

    const int16_t *pcm = NULL;
    size_t pcm_bytes = 0;
    uint32_t duration_ms = 0;
    esp_err_t err = fridge_audio_get_pcm(&pcm, &pcm_bytes, &duration_ms);
    if (err != ESP_OK || !pcm || pcm_bytes < 3200) {
        strlcpy(out->error, "recording is empty or too short", sizeof(out->error));
        set_last_error(out->error);
        return ESP_ERR_INVALID_STATE;
    }
    out->audio_bytes = pcm_bytes + WAV_HEADER_BYTES;

    size_t body_len = 0;
    char *body = build_multipart_body(&config, pcm, pcm_bytes, &body_len);
    if (!body) {
        strlcpy(out->error, "build ASR multipart body failed", sizeof(out->error));
        set_last_error(out->error);
        return ESP_ERR_NO_MEM;
    }

    char *response_body = calloc(1, ASR_RESPONSE_CAP);
    if (!response_body) {
        free(body);
        return ESP_ERR_NO_MEM;
    }
    asr_http_buffer_t rx = {
        .data = response_body,
        .len = 0,
        .cap = ASR_RESPONSE_CAP,
        .overflow = false,
    };

    esp_http_client_config_t http_config = {
        .url = config.api_base_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = (int)clamp_timeout_ms(config.timeout_ms),
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = asr_http_event_handler,
        .user_data = &rx,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        free(body);
        free(response_body);
        strlcpy(out->error, "init ASR HTTP client failed", sizeof(out->error));
        set_last_error(out->error);
        return ESP_FAIL;
    }

    char auth_header[FRIDGE_ASR_MAX_API_KEY_LEN + 16] = {0};
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", config.api_key);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_header(client, "Content-Type", "multipart/form-data; boundary=----fridge-spirit-asr-boundary");
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_post_field(client, body, (int)body_len);

    err = esp_http_client_perform(client);
    out->http_status = esp_http_client_get_status_code(client);
    out->latency_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
    strlcpy(out->model, config.model, sizeof(out->model));
    esp_http_client_cleanup(client);
    free(body);

    ESP_LOGI(TAG, "ASR HTTP done: status=%d, audio=%u bytes, response=%u bytes, latency=%lu ms",
             out->http_status,
             (unsigned)out->audio_bytes,
             (unsigned)rx.len,
             (unsigned long)out->latency_ms);

    if (err != ESP_OK) {
        snprintf(out->error, sizeof(out->error), "ASR HTTP failed: %s", esp_err_to_name(err));
        set_last_error(out->error);
        free(response_body);
        return err;
    }
    if (rx.overflow) {
        strlcpy(out->error, "ASR response too long", sizeof(out->error));
        set_last_error(out->error);
        free(response_body);
        return ESP_FAIL;
    }
    if (out->http_status < 200 || out->http_status >= 300) {
        snprintf(out->error, sizeof(out->error), "ASR HTTP status %d", out->http_status);
        set_last_error(out->error);
        free(response_body);
        return ESP_FAIL;
    }
    if (!json_get_text_field(response_body, out->text, sizeof(out->text))) {
        strlcpy(out->error, "ASR response missing text, please check microphone quality", sizeof(out->error));
        set_last_error(out->error);
        free(response_body);
        return ESP_FAIL;
    }

    strlcpy(out->status, "ok", sizeof(out->status));
    set_last_error("");
    free(response_body);
    return ESP_OK;
}
