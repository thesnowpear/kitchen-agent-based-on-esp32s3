#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_AI_MAX_BASE_URL_LEN 160
#define FRIDGE_AI_MAX_API_KEY_LEN 256
#define FRIDGE_AI_MAX_MODEL_LEN 64
#define FRIDGE_AI_MAX_SYSTEM_PROMPT_LEN 256
#define FRIDGE_AI_MAX_CHAT_MESSAGE_LEN 768
#define FRIDGE_AI_MAX_REPLY_LEN 2048
#define FRIDGE_AI_MAX_ERROR_LEN 160
#define FRIDGE_AI_MAX_PROFILE_NAME_LEN 32
#define FRIDGE_AI_MAX_PROFILES 5

#define FRIDGE_AI_DEFAULT_MODEL "gpt-4o-mini"
#define FRIDGE_AI_DEFAULT_TIMEOUT_MS 30000

// AI 配置只返回可展示信息，不包含 API Key 明文。
// 注意：API Key 在开发模式下保存到 NVS，后续可替换为云端 AI Adapter 或 NVS 加密实现。
typedef struct {
    uint8_t profile_id;
    char profile_name[FRIDGE_AI_MAX_PROFILE_NAME_LEN + 1];
    char api_base_url[FRIDGE_AI_MAX_BASE_URL_LEN + 1];
    char model[FRIDGE_AI_MAX_MODEL_LEN + 1];
    char system_prompt[FRIDGE_AI_MAX_SYSTEM_PROMPT_LEN + 1];
    uint32_t timeout_ms;
    bool has_api_key;
    bool ready;
    char api_key_preview[32];
    char last_error[FRIDGE_AI_MAX_ERROR_LEN + 1];
} fridge_ai_config_view_t;

// AI 配置更新结构。
// update_api_key 为 true 时才覆盖旧 Key；空 Key 不会隐式清除，避免 WebUI 刷新时误删凭据。
typedef struct {
    uint8_t profile_id;
    char profile_name[FRIDGE_AI_MAX_PROFILE_NAME_LEN + 1];
    char api_base_url[FRIDGE_AI_MAX_BASE_URL_LEN + 1];
    char api_key[FRIDGE_AI_MAX_API_KEY_LEN + 1];
    char model[FRIDGE_AI_MAX_MODEL_LEN + 1];
    char system_prompt[FRIDGE_AI_MAX_SYSTEM_PROMPT_LEN + 1];
    uint32_t timeout_ms;
    bool update_api_key;
} fridge_ai_config_update_t;

// AI 配置列表：最多保存少量开发配置，便于在不同 OpenAI-compatible 服务之间切换。
typedef struct {
    uint8_t active_profile_id;
    size_t count;
    fridge_ai_config_view_t profiles[FRIDGE_AI_MAX_PROFILES];
} fridge_ai_profile_list_t;

// AI 聊天测试结果：用于 Web 面板的模拟聊天窗口。
// 首版只做文本请求，不接摄像头图片，也不把结果直接写入库存。
typedef struct {
    char reply[FRIDGE_AI_MAX_REPLY_LEN + 1];
    char model[FRIDGE_AI_MAX_MODEL_LEN + 1];
    char status[32];
    char error[FRIDGE_AI_MAX_ERROR_LEN + 1];
    int http_status;
    uint32_t latency_ms;
} fridge_ai_chat_result_t;

// 初始化 AI 客户端；该组件只使用 NVS 和 HTTPS，不控制 GPIO。
esp_err_t fridge_ai_client_init(void);

// 读取 AI 配置展示视图；不会返回 API Key 明文。
esp_err_t fridge_ai_client_get_config(fridge_ai_config_view_t *out);

// 保存 AI 配置；API Key 只有在 update_api_key 为 true 时写入 NVS。
esp_err_t fridge_ai_client_set_config(const fridge_ai_config_update_t *config);

// 读取 AI 配置槽列表；不会返回 API Key 明文。
esp_err_t fridge_ai_client_get_profiles(fridge_ai_profile_list_t *out);

// 创建一个新的 AI 配置槽，并切换为当前配置。
esp_err_t fridge_ai_client_create_profile(const char *profile_name, fridge_ai_config_view_t *out);

// 切换当前 AI 配置槽。
esp_err_t fridge_ai_client_select_profile(uint8_t profile_id, fridge_ai_config_view_t *out);

// 删除一个备用 AI 配置槽；默认 0 号配置不可删除，只能清空 Key。
esp_err_t fridge_ai_client_delete_profile(uint8_t profile_id, fridge_ai_config_view_t *out);

// 清除本地保存的 API Key，保留 Base URL、模型和系统提示词。
esp_err_t fridge_ai_client_clear_key(void);

// 使用 OpenAI-compatible /chat/completions 进行一次文本聊天测试。
esp_err_t fridge_ai_client_test_chat(const char *message, fridge_ai_chat_result_t *out);

#ifdef __cplusplus
}
#endif
