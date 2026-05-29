#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_MQTT_MAX_URI_LEN 160
#define FRIDGE_MQTT_MAX_ID_LEN 64
#define FRIDGE_MQTT_MAX_TOKEN_LEN 160
#define FRIDGE_MQTT_MAX_STATUS_LEN 160
#define FRIDGE_MQTT_MAX_TOPIC_LEN 192
#define FRIDGE_MQTT_DEFAULT_KEEPALIVE_SECONDS 60

// 云端绑定配置：由 USB/Web Serial 面板或后续小程序绑定流程写入 NVS。
// 注意：password/token 只用于连接 MQTT Broker，绝不能输出到串口响应或日志。
typedef struct {
    char broker_uri[FRIDGE_MQTT_MAX_URI_LEN + 1];
    char home_id[FRIDGE_MQTT_MAX_ID_LEN + 1];
    char device_id[FRIDGE_MQTT_MAX_ID_LEN + 1];
    char username[FRIDGE_MQTT_MAX_ID_LEN + 1];
    char password[FRIDGE_MQTT_MAX_TOKEN_LEN + 1];
    uint16_t keepalive_seconds;
    bool use_tls;
    bool enabled;
} fridge_mqtt_config_t;

// 云端同步状态：只返回脱敏后的连接情况，供 Web 面板和诊断组件展示。
typedef struct {
    bool initialized;
    bool enabled;
    bool configured;
    bool connected;
    bool has_password;
    uint32_t reconnect_count;
    uint32_t published_count;
    uint32_t received_count;
    int last_error;
    char broker_uri[FRIDGE_MQTT_MAX_URI_LEN + 1];
    char home_id[FRIDGE_MQTT_MAX_ID_LEN + 1];
    char device_id[FRIDGE_MQTT_MAX_ID_LEN + 1];
    char username[FRIDGE_MQTT_MAX_ID_LEN + 1];
    char status_text[FRIDGE_MQTT_MAX_STATUS_LEN + 1];
} fridge_mqtt_status_t;

// 初始化 MQTT 协议组件；只创建内部状态，不会在未配置 broker 时发起连接。
esp_err_t fridge_mqtt_protocol_init(void);

// 读取云端绑定配置；password 会返回给调用方用于内部连接，Web 输出时必须自行脱敏。
esp_err_t fridge_mqtt_get_config(fridge_mqtt_config_t *out);

// 保存云端绑定配置。update_password=false 时保留旧密码，避免前端刷新误清凭证。
esp_err_t fridge_mqtt_set_config(const fridge_mqtt_config_t *config, bool update_password);

// 清除 MQTT 密码/token，保留 broker、home_id 和 device_id。
esp_err_t fridge_mqtt_clear_secret(void);

// 获取脱敏状态。
esp_err_t fridge_mqtt_get_status(fridge_mqtt_status_t *out);

// Wi-Fi 联网后启动 MQTT。未启用或配置不完整时返回 ESP_ERR_INVALID_STATE。
esp_err_t fridge_mqtt_start(void);

// 主动停止 MQTT 连接；用于切换配置或调试。
esp_err_t fridge_mqtt_stop(void);

// 发布设备状态快照。online=false 时可用于离线前遗嘱语义的手动上报。
esp_err_t fridge_mqtt_publish_state(bool online);

// 发布简短传感器聚合事件；v1 不发布原始高频 IMU/雷达流。
esp_err_t fridge_mqtt_publish_sensor_snapshot(void);

// 发布当前 UI 库存同步快照。
// 仅在用户确认保存、云端请求刷新或调试命令触发时调用；未连接时返回 ESP_ERR_INVALID_STATE。
esp_err_t fridge_mqtt_publish_inventory_snapshot(bool force_import);

// 发布命令回执；request_id 必须来自云端命令，便于后端幂等处理。
esp_err_t fridge_mqtt_publish_command_ack(const char *request_id, const char *command, bool ok, const char *message);

// 发布带执行阶段语义的命令回执。
// stage=received 表示设备已收到命令；stage=completed 表示本地执行已完成并携带版本结果。
esp_err_t fridge_mqtt_publish_command_ack_ex(const char *request_id,
                                             const char *command,
                                             const char *stage,
                                             bool ok,
                                             const char *error_code,
                                             uint32_t local_revision,
                                             uint32_t server_revision,
                                             bool applied,
                                             const char *message);

#ifdef __cplusplus
}
#endif
