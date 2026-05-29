#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_types.h"
#include "fridge_radar.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_LIGHT_ADC_GPIO 1
#define FRIDGE_LIGHT_ADC_MAX_RAW 4095
#define FRIDGE_LIGHT_VALUE_MAX_10BIT 1023

#define FRIDGE_IMU_I2C_PORT I2C_NUM_0
#define FRIDGE_IMU_I2C_SDA_GPIO 4
#define FRIDGE_IMU_I2C_SCL_GPIO 5
#define FRIDGE_IMU_DEFAULT_ADDR 0x68

// 传感器快照：给 Web 面板和后续状态机复用。
// 注意：当前光敏模块 AO 为反向模拟量，ADC 原始值越高表示越暗，越低表示越亮。
// light_value_10bit、light_percent 和 light_delta 已换算为“亮度语义”，数值越高表示越亮。
// 光敏模块 AO 只能接 3.3V 供电后的模拟输出，禁止把 5V 模拟信号接入 GPIO1。
typedef struct {
    bool ready;
    uint16_t light_raw_12bit;
    uint16_t light_value_10bit;
    uint8_t light_percent;
    int16_t light_delta;
    bool imu_ready;
    uint8_t imu_address;
    uint8_t imu_who_am_i;
    int imu_error;
    float accel_x_g;
    float accel_y_g;
    float accel_z_g;
    float gyro_x_dps;
    float gyro_y_dps;
    float gyro_z_dps;
    float imu_temperature_c;
    float pitch_deg;
    float roll_deg;
    float angle_delta;
    float vibration_peak;
    fridge_radar_snapshot_t radar;
    int64_t updated_at_ms;
} fridge_sensor_snapshot_t;

// 初始化光敏 ADC 采样任务。
esp_err_t fridge_sensors_init(void);

// 读取最近一次采样快照；该函数不访问硬件，适合 USB/Web Serial 高频轮询。
esp_err_t fridge_sensors_get_snapshot(fridge_sensor_snapshot_t *out);

#ifdef __cplusplus
}
#endif
