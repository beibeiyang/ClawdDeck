#include "../../hal/imu_hal.h"
#include <lvgl.h>
#include <math.h>
#include <stdlib.h>

static bool  sim_ok = true;
static float sim_pitch = 0.0f;
static float sim_roll  = 0.0f;
static bool  sim_pitch_fixed = false;
static bool  sim_roll_fixed = false;

static bool env_degrees(const char* name, float* value) {
    const char* raw = getenv(name);
    if (!raw || !*raw) return false;
    char* end = nullptr;
    const float parsed = strtof(raw, &end);
    if (end == raw || *end != '\0') return false;
    *value = parsed;
    return true;
}

void imu_hal_init(void) {
    sim_pitch_fixed = env_degrees("SIM_IMU_PITCH_DEG", &sim_pitch);
    sim_roll_fixed = env_degrees("SIM_IMU_ROLL_DEG", &sim_roll);
}

void imu_hal_tick(void) {
    const uint32_t t = lv_tick_get();
    if (!sim_pitch_fixed) sim_pitch = sinf((float)t * 0.0007f) * 12.0f;
    if (!sim_roll_fixed) sim_roll = cosf((float)t * 0.0005f) * 10.0f;
}

uint8_t imu_hal_rotation_quadrant(void) { return 0; }

bool imu_hal_ready(void) { return sim_ok; }

bool imu_hal_read_accel(float* ax, float* ay, float* az) {
    if (!sim_ok || !ax || !ay || !az) return false;
    const float pr = sim_pitch * 0.0174533f;
    const float rr = sim_roll  * 0.0174533f;
    *ax = sinf(rr);
    *ay = sinf(pr);
    *az = cosf(pr) * cosf(rr);
    return true;
}

bool imu_hal_read_accel_live(float* ax, float* ay, float* az) {
    return imu_hal_read_accel(ax, ay, az);
}
