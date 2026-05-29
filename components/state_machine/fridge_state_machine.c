// 冰箱小精灵独立状态机实现。
// 该任务周期读取传感器快照，不直接访问 GPIO/UART/ADC；夜间“关闭雷达”首版仅做软件暂停策略，不卸载 UART、不改供电。

#include "fridge_state_machine.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fridge_audio.h"
#include "fridge_network.h"
#include "fridge_sensors.h"
#include "fridge_voice_session.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#define SM_TASK_STACK 8192
#define SM_TASK_PRIORITY 4
#define SM_PERIOD_MS 300
#define SM_NVS_NAMESPACE "fridge_state"
#define SM_POST_CLOSE_HOLD_MS 8000
#define SM_IMU_ANGLE_MOVING_DEG 12.0f
#define SM_IMU_ANGLE_OPEN_DEG 24.0f
#define SM_IMU_VIB_MOVING_G 0.12f
#define SM_IMU_GYRO_MOVING_DPS 80.0f
#define SM_LIGHT_OPEN_DELTA 80
#define SM_VOICE_TASK_STACK 32768

static const char *TAG = "state_machine";

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static TaskHandle_t s_auto_voice_task;
static fridge_sm_config_t s_config;
static fridge_sm_snapshot_t s_snapshot;
static fridge_sm_door_state_t s_door_state = FRIDGE_SM_DOOR_UNKNOWN;
static bool s_is_night;
static int64_t s_state_since_ms;
static int64_t s_last_door_active_ms;
static int64_t s_post_close_enter_ms;
static uint32_t s_post_close_generation;

static fridge_sm_config_t default_config(void)
{
    return (fridge_sm_config_t) {
        .night_light_threshold = 250,
        .day_light_threshold = 450,
        .radar_two_meter_raw = 200,
        .radar_two_meter_gate = 8,
        .sleep_enabled = false,
        .auto_voice_after_close = true,
        .auto_voice_record_seconds = 6,
        .close_stable_ms = 2500,
    };
}

const char *fridge_state_machine_state_to_string(fridge_sm_state_t state)
{
    switch (state) {
    case FRIDGE_SM_STATE_SLEEP:
        return "SLEEP";
    case FRIDGE_SM_STATE_NIGHT_SAVE:
        return "NIGHT_SAVE";
    case FRIDGE_SM_STATE_APPROACH:
        return "APPROACH";
    case FRIDGE_SM_STATE_INTERACTIVE:
        return "INTERACTIVE";
    case FRIDGE_SM_STATE_DOOR_MOVING:
        return "DOOR_MOVING";
    case FRIDGE_SM_STATE_DOOR_OPEN:
        return "DOOR_OPEN";
    case FRIDGE_SM_STATE_POST_CLOSE:
        return "POST_CLOSE";
    case FRIDGE_SM_STATE_OFFLINE:
        return "OFFLINE";
    default:
        return "UNKNOWN";
    }
}

const char *fridge_state_machine_door_to_string(fridge_sm_door_state_t state)
{
    switch (state) {
    case FRIDGE_SM_DOOR_CLOSED:
        return "CLOSED";
    case FRIDGE_SM_DOOR_MOVING:
        return "MOVING";
    case FRIDGE_SM_DOOR_OPEN:
        return "OPEN";
    case FRIDGE_SM_DOOR_POST_CLOSE:
        return "POST_CLOSE";
    case FRIDGE_SM_DOOR_UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

const char *fridge_state_machine_auto_voice_to_string(fridge_sm_auto_voice_state_t state)
{
    switch (state) {
    case FRIDGE_SM_AUTO_VOICE_IDLE:
        return "IDLE";
    case FRIDGE_SM_AUTO_VOICE_RECORDING:
        return "RECORDING";
    case FRIDGE_SM_AUTO_VOICE_PROCESSING:
        return "PROCESSING";
    case FRIDGE_SM_AUTO_VOICE_DONE:
        return "DONE";
    case FRIDGE_SM_AUTO_VOICE_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

static void set_auto_voice_state(fridge_sm_auto_voice_state_t state, const char *error)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_snapshot.auto_voice_state = state;
    if (error) {
        strlcpy(s_snapshot.auto_voice_error, error, sizeof(s_snapshot.auto_voice_error));
    } else if (state != FRIDGE_SM_AUTO_VOICE_ERROR) {
        s_snapshot.auto_voice_error[0] = '\0';
    }
    xSemaphoreGive(s_lock);
}

static void auto_voice_task(void *arg)
{
    uint32_t generation = (uint32_t)(uintptr_t)arg;
    fridge_sm_config_t cfg = {0};
    (void)fridge_state_machine_get_config(&cfg);
    uint32_t seconds = cfg.auto_voice_record_seconds;
    if (seconds == 0 || seconds > FRIDGE_AUDIO_MAX_RECORD_SECONDS) {
        seconds = FRIDGE_AUDIO_MAX_RECORD_SECONDS;
    }

    // 关门后的自动语音在独立任务中运行，避免 ASR/AI 的 HTTPS 阻塞状态机周期判断。
    set_auto_voice_state(FRIDGE_SM_AUTO_VOICE_RECORDING, NULL);
    esp_err_t err = fridge_audio_start_recording();
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(seconds * 1000));
        err = fridge_audio_stop_recording();
    }
    if (err == ESP_OK) {
        set_auto_voice_state(FRIDGE_SM_AUTO_VOICE_PROCESSING, NULL);
        fridge_voice_session_result_t result = {0};
        err = fridge_voice_session_run_latest_recording(&result);
        set_auto_voice_state(err == ESP_OK ? FRIDGE_SM_AUTO_VOICE_DONE : FRIDGE_SM_AUTO_VOICE_ERROR,
                             err == ESP_OK ? NULL : result.error);
    } else {
        set_auto_voice_state(FRIDGE_SM_AUTO_VOICE_ERROR, esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "auto voice generation=%lu done err=%s", (unsigned long)generation, esp_err_to_name(err));
    s_auto_voice_task = NULL;
    vTaskDelete(NULL);
}

static void maybe_start_auto_voice(void)
{
    if (!s_config.auto_voice_after_close || s_auto_voice_task) {
        return;
    }
    s_post_close_generation++;
    BaseType_t ok = xTaskCreateWithCaps(auto_voice_task,
                                        "sm_auto_voice",
                                        SM_VOICE_TASK_STACK,
                                        (void *)(uintptr_t)s_post_close_generation,
                                        4,
                                        &s_auto_voice_task,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ok = xTaskCreate(auto_voice_task,
                         "sm_auto_voice",
                         SM_VOICE_TASK_STACK,
                         (void *)(uintptr_t)s_post_close_generation,
                         4,
                         &s_auto_voice_task);
    }
    if (ok != pdPASS) {
        set_auto_voice_state(FRIDGE_SM_AUTO_VOICE_ERROR, "auto voice task create failed");
    }
}

static void load_config_from_nvs(void)
{
    s_config = default_config();
    nvs_handle_t nvs = 0;
    if (nvs_open(SM_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    (void)nvs_get_u16(nvs, "night", &s_config.night_light_threshold);
    (void)nvs_get_u16(nvs, "day", &s_config.day_light_threshold);
    (void)nvs_get_u16(nvs, "radar_raw", &s_config.radar_two_meter_raw);
    (void)nvs_get_u8(nvs, "radar_gate", &s_config.radar_two_meter_gate);
    uint8_t sleep_enabled = s_config.sleep_enabled ? 1 : 0;
    (void)nvs_get_u8(nvs, "sleep_en", &sleep_enabled);
    s_config.sleep_enabled = sleep_enabled != 0;
    uint8_t auto_voice = s_config.auto_voice_after_close ? 1 : 0;
    (void)nvs_get_u8(nvs, "auto_voice", &auto_voice);
    s_config.auto_voice_after_close = auto_voice != 0;
    (void)nvs_get_u32(nvs, "voice_sec", &s_config.auto_voice_record_seconds);
    (void)nvs_get_u32(nvs, "close_ms", &s_config.close_stable_ms);
    nvs_close(nvs);
}

static esp_err_t save_config_to_nvs(const fridge_sm_config_t *config)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(SM_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u16(nvs, "night", config->night_light_threshold);
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, "day", config->day_light_threshold);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, "radar_raw", config->radar_two_meter_raw);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "radar_gate", config->radar_two_meter_gate);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "sleep_en", config->sleep_enabled ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "auto_voice", config->auto_voice_after_close ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, "voice_sec", config->auto_voice_record_seconds);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, "close_ms", config->close_stable_ms);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static bool normalize_config(fridge_sm_config_t *config)
{
    if (!config) {
        return false;
    }
    if (config->night_light_threshold >= config->day_light_threshold) {
        return false;
    }
    if (config->day_light_threshold > FRIDGE_LIGHT_VALUE_MAX_10BIT) {
        return false;
    }
    if (config->radar_two_meter_gate >= FRIDGE_RADAR_GATE_COUNT) {
        config->radar_two_meter_gate = FRIDGE_RADAR_GATE_COUNT - 1;
    }
    if (config->auto_voice_record_seconds == 0 || config->auto_voice_record_seconds > FRIDGE_AUDIO_MAX_RECORD_SECONDS) {
        config->auto_voice_record_seconds = FRIDGE_AUDIO_MAX_RECORD_SECONDS;
    }
    if (config->close_stable_ms < 800) {
        config->close_stable_ms = 800;
    }
    return true;
}

static float imu_motion_strength(const fridge_sensor_snapshot_t *sensors, bool *moving, bool *open_signal)
{
    float gyro_abs = fabsf(sensors->gyro_x_dps) + fabsf(sensors->gyro_y_dps) + fabsf(sensors->gyro_z_dps);
    float angle_score = sensors->angle_delta / SM_IMU_ANGLE_OPEN_DEG;
    float vib_score = sensors->vibration_peak / 0.25f;
    float gyro_score = gyro_abs / 240.0f;
    float strength = fmaxf(angle_score, fmaxf(vib_score, gyro_score));
    if (strength > 1.0f) {
        strength = 1.0f;
    }

    *moving = sensors->angle_delta >= SM_IMU_ANGLE_MOVING_DEG ||
              sensors->vibration_peak >= SM_IMU_VIB_MOVING_G ||
              gyro_abs >= SM_IMU_GYRO_MOVING_DPS;
    *open_signal = sensors->angle_delta >= SM_IMU_ANGLE_OPEN_DEG ||
                   (*moving && sensors->light_delta >= SM_LIGHT_OPEN_DELTA);
    return strength;
}

static void update_door_state(const fridge_sensor_snapshot_t *sensors, bool imu_moving, bool door_open_signal, int64_t now_ms)
{
    if (imu_moving) {
        s_last_door_active_ms = now_ms;
        s_door_state = door_open_signal ? FRIDGE_SM_DOOR_OPEN : FRIDGE_SM_DOOR_MOVING;
        return;
    }

    bool was_active = s_door_state == FRIDGE_SM_DOOR_OPEN || s_door_state == FRIDGE_SM_DOOR_MOVING;
    bool quiet = sensors->angle_delta < 6.0f && sensors->vibration_peak < 0.04f;
    if (was_active && quiet && now_ms - s_last_door_active_ms >= (int64_t)s_config.close_stable_ms) {
        s_door_state = FRIDGE_SM_DOOR_POST_CLOSE;
        s_post_close_enter_ms = now_ms;
        maybe_start_auto_voice();
        return;
    }
    if (s_door_state == FRIDGE_SM_DOOR_POST_CLOSE && now_ms - s_post_close_enter_ms >= SM_POST_CLOSE_HOLD_MS) {
        s_door_state = FRIDGE_SM_DOOR_CLOSED;
        return;
    }
    if (s_door_state == FRIDGE_SM_DOOR_UNKNOWN) {
        s_door_state = FRIDGE_SM_DOOR_CLOSED;
    }
}

static fridge_sm_state_t decide_state(const fridge_sensor_snapshot_t *sensors,
                                      const fridge_network_status_t *net,
                                      bool radar_reliable,
                                      bool radar_within_2m,
                                      bool imu_moving,
                                      char *reason,
                                      size_t reason_size)
{
    (void)imu_moving;
    if (s_is_night) {
        strlcpy(reason, "light below night threshold; radar software paused", reason_size);
        return FRIDGE_SM_STATE_NIGHT_SAVE;
    }
    if (s_door_state == FRIDGE_SM_DOOR_POST_CLOSE) {
        strlcpy(reason, "door stabilized after movement", reason_size);
        return FRIDGE_SM_STATE_POST_CLOSE;
    }
    if (s_door_state == FRIDGE_SM_DOOR_OPEN) {
        strlcpy(reason, "IMU motion confirms door open", reason_size);
        return FRIDGE_SM_STATE_DOOR_OPEN;
    }
    if (s_door_state == FRIDGE_SM_DOOR_MOVING) {
        strlcpy(reason, "IMU angle/vibration indicates door moving", reason_size);
        return FRIDGE_SM_STATE_DOOR_MOVING;
    }
    if (radar_reliable && radar_within_2m) {
        strlcpy(reason, "reliable radar target within configured 2m boundary", reason_size);
        return FRIDGE_SM_STATE_INTERACTIVE;
    }
    if (radar_reliable || sensors->radar.approaching) {
        strlcpy(reason, "radar sees approaching/present target outside interaction boundary", reason_size);
        return FRIDGE_SM_STATE_APPROACH;
    }
    // 离线只作为快照标志上报，不能覆盖本地靠近/开门/首页流程。
    // 这样重启后 Wi-Fi 还没连上时，设备仍保持本地可用，而不是默认进入离线页。
    if (!net->connected) {
        strlcpy(reason, "network offline flag set; local idle state continues", reason_size);
        return FRIDGE_SM_STATE_SLEEP;
    }
    strlcpy(reason, "bright light, no reliable radar target, IMU quiet", reason_size);
    return FRIDGE_SM_STATE_SLEEP;
}

static void state_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    while (true) {
        fridge_sensor_snapshot_t sensors = {0};
        fridge_network_status_t net = {0};
        (void)fridge_sensors_get_snapshot(&sensors);
        (void)fridge_network_get_status(&net);

        int64_t now_ms = esp_timer_get_time() / 1000;
        if (s_is_night) {
            if (sensors.light_value_10bit > s_config.day_light_threshold) {
                s_is_night = false;
            }
        } else if (sensors.light_value_10bit < s_config.night_light_threshold) {
            s_is_night = true;
        }

        bool imu_moving = false;
        bool door_open_signal = false;
        float motion = imu_motion_strength(&sensors, &imu_moving, &door_open_signal);
        update_door_state(&sensors, imu_moving, door_open_signal, now_ms);

        bool radar_reliable = !s_is_night && sensors.radar.ready &&
                              (sensors.radar.stable_presence || sensors.radar.human_candidate || sensors.radar.threshold_presence);
        uint16_t distance = sensors.radar.smoothed_distance_raw ? sensors.radar.smoothed_distance_raw : sensors.radar.distance_raw;
        bool radar_within_2m = radar_reliable &&
                               (sensors.radar.within_1m ||
                                (distance > 0 && distance <= s_config.radar_two_meter_raw) ||
                                (sensors.radar.stable_gate > 0 && sensors.radar.stable_gate <= s_config.radar_two_meter_gate));

        fridge_sm_snapshot_t next = {0};
        next.door_state = s_door_state;
        next.offline = !net.connected;
        next.is_night = s_is_night;
        next.radar_software_paused = s_is_night;
        next.radar_presence_reliable = radar_reliable;
        next.radar_within_2m = radar_within_2m;
        next.radar_within_1m = sensors.radar.within_1m;
        next.radar_approaching = sensors.radar.approaching;
        next.imu_motion_strength = motion;
        next.light_value_10bit = sensors.light_value_10bit;
        next.light_delta = sensors.light_delta;
        next.radar_distance_raw = distance;
        next.radar_gate = sensors.radar.stable_gate;
        next.updated_at_ms = now_ms;
        next.auto_voice_state = s_snapshot.auto_voice_state;
        strlcpy(next.auto_voice_error, s_snapshot.auto_voice_error, sizeof(next.auto_voice_error));
        next.state = decide_state(&sensors, &net, radar_reliable, radar_within_2m, imu_moving, next.last_reason, sizeof(next.last_reason));

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (next.state != s_snapshot.state) {
            s_state_since_ms = now_ms;
            ESP_LOGI(TAG, "state %s -> %s, door=%s, reason=%s",
                     fridge_state_machine_state_to_string(s_snapshot.state),
                     fridge_state_machine_state_to_string(next.state),
                     fridge_state_machine_door_to_string(next.door_state),
                     next.last_reason);
        }
        next.state_since_ms = s_state_since_ms;
        s_snapshot = next;
        xSemaphoreGive(s_lock);

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SM_PERIOD_MS));
    }
}

esp_err_t fridge_state_machine_init(void)
{
    if (s_task) {
        return ESP_OK;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    load_config_from_nvs();
    if (!normalize_config(&s_config)) {
        s_config = default_config();
    }
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.state = FRIDGE_SM_STATE_SLEEP;
    s_snapshot.door_state = FRIDGE_SM_DOOR_UNKNOWN;
    s_snapshot.auto_voice_state = FRIDGE_SM_AUTO_VOICE_IDLE;
    s_state_since_ms = esp_timer_get_time() / 1000;
    BaseType_t ok = xTaskCreate(state_task, "fridge_sm", SM_TASK_STACK, NULL, SM_TASK_PRIORITY, &s_task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t fridge_state_machine_get_snapshot(fridge_sm_snapshot_t *out)
{
    if (!out || !s_lock) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_snapshot;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_state_machine_get_config(fridge_sm_config_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lock) {
        *out = default_config();
        return ESP_OK;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_config;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_state_machine_set_config(const fridge_sm_config_t *config)
{
    if (!config || !s_lock) {
        return ESP_ERR_INVALID_ARG;
    }
    fridge_sm_config_t next = *config;
    if (!normalize_config(&next)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = save_config_to_nvs(&next);
    if (err != ESP_OK) {
        return err;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_config = next;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
