// 冰箱小精灵独立状态机公共接口。
// 状态机融合光照、IMU、毫米波雷达和网络状态；雷达只作为靠近/有人上下文，不单独判开门。

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FRIDGE_SM_STATE_SLEEP = 0,
    FRIDGE_SM_STATE_NIGHT_SAVE,
    FRIDGE_SM_STATE_APPROACH,
    FRIDGE_SM_STATE_INTERACTIVE,
    FRIDGE_SM_STATE_DOOR_MOVING,
    FRIDGE_SM_STATE_DOOR_OPEN,
    FRIDGE_SM_STATE_POST_CLOSE,
    FRIDGE_SM_STATE_OFFLINE,
} fridge_sm_state_t;

typedef enum {
    FRIDGE_SM_DOOR_UNKNOWN = 0,
    FRIDGE_SM_DOOR_CLOSED,
    FRIDGE_SM_DOOR_MOVING,
    FRIDGE_SM_DOOR_OPEN,
    FRIDGE_SM_DOOR_POST_CLOSE,
} fridge_sm_door_state_t;

typedef enum {
    FRIDGE_SM_AUTO_VOICE_IDLE = 0,
    FRIDGE_SM_AUTO_VOICE_RECORDING,
    FRIDGE_SM_AUTO_VOICE_PROCESSING,
    FRIDGE_SM_AUTO_VOICE_DONE,
    FRIDGE_SM_AUTO_VOICE_ERROR,
} fridge_sm_auto_voice_state_t;

typedef struct {
    uint16_t night_light_threshold;
    uint16_t day_light_threshold;
    uint16_t radar_two_meter_raw;
    uint8_t radar_two_meter_gate;
    bool sleep_enabled;
    bool auto_voice_after_close;
    uint32_t auto_voice_record_seconds;
    uint32_t close_stable_ms;
} fridge_sm_config_t;

typedef struct {
    fridge_sm_state_t state;
    fridge_sm_door_state_t door_state;
    bool offline;
    bool is_night;
    bool radar_software_paused;
    bool radar_presence_reliable;
    bool radar_within_2m;
    bool radar_within_1m;
    bool radar_approaching;
    float imu_motion_strength;
    uint16_t light_value_10bit;
    int16_t light_delta;
    uint16_t radar_distance_raw;
    uint8_t radar_gate;
    fridge_sm_auto_voice_state_t auto_voice_state;
    char last_reason[96];
    char auto_voice_error[160];
    int64_t updated_at_ms;
    int64_t state_since_ms;
} fridge_sm_snapshot_t;

esp_err_t fridge_state_machine_init(void);
esp_err_t fridge_state_machine_get_snapshot(fridge_sm_snapshot_t *out);
esp_err_t fridge_state_machine_get_config(fridge_sm_config_t *out);
esp_err_t fridge_state_machine_set_config(const fridge_sm_config_t *config);

const char *fridge_state_machine_state_to_string(fridge_sm_state_t state);
const char *fridge_state_machine_door_to_string(fridge_sm_door_state_t state);
const char *fridge_state_machine_auto_voice_to_string(fridge_sm_auto_voice_state_t state);

#ifdef __cplusplus
}
#endif
