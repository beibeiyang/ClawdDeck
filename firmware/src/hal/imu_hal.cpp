#include "imu_hal.h"
#include <math.h>

__attribute__((weak)) void imu_hal_set_rotation_locked(bool locked) {
    (void)locked;
}

bool imu_hal_read_tilt(float* pitch_deg, float* roll_deg) {
    float ax, ay, az;
    if (!imu_hal_read_accel(&ax, &ay, &az)) return false;
    *roll_deg  = atan2f(ax, az) * 57.2958f;
    *pitch_deg = atan2f(ay, az) * 57.2958f;
    return true;
}

__attribute__((weak)) bool imu_hal_read_accel_live(float* ax, float* ay, float* az) {
    return imu_hal_read_accel(ax, ay, az);
}

static float g_bias_x = 0.0f;
static float g_bias_y = 0.0f;
static bool  g_cal    = false;
static int   g_cal_n  = 0;
static float g_sum_x  = 0.0f;
static float g_sum_y  = 0.0f;

void imu_hal_gravity_cal_reset(void) {
    g_cal   = false;
    g_cal_n = 0;
    g_sum_x = g_sum_y = 0.0f;
}

bool imu_hal_gravity_cal_step(int* pct_out) {
    float ax, ay, az;
    if (!imu_hal_read_accel_live(&ax, &ay, &az)) {
        if (pct_out) *pct_out = 0;
        return false;
    }
    g_sum_x += ax;
    g_sum_y += ay;
    g_cal_n++;
    if (pct_out) *pct_out = (g_cal_n * 100) / 200;
    if (g_cal_n < 200) return false;
    g_bias_x = g_sum_x / 200.0f;
    g_bias_y = g_sum_y / 200.0f;
    g_cal    = true;
    return true;
}

bool imu_hal_gravity_ready(void) { return g_cal; }

bool imu_hal_read_gravity(float* gx, float* gy) {
    float ax, ay, az;
    // Motion apps consume the board driver's rate-limited cache. Reading the
    // I2C sensor on every LVGL frame stalls animation and touch handling.
    if (!imu_hal_read_accel(&ax, &ay, &az)) return false;
    if (g_cal) {
        ax -= g_bias_x;
        ay -= g_bias_y;
    }
    // Suppress normal sensor offset/noise even when an app intentionally uses
    // uncalibrated gravity (calibration is only valid on a known-level surface).
    if (fabsf(ax) < 0.05f) ax = 0.0f;
    if (fabsf(ay) < 0.05f) ay = 0.0f;
    *gx = ax;
    *gy = ay;
    return true;
}
