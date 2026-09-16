#pragma once
#include <stdint.h>
#include <stdbool.h>

// Optional accelerometer-driven orientation tracker. Returns 0..3 (quarter
// turns CW from default mounting). Boards without an IMU — or boards with
// rotation intentionally disabled — return 0
// from imu_hal_rotation_quadrant() and no-op on init/tick.

void    imu_hal_init(void);
void    imu_hal_tick(void);
uint8_t imu_hal_rotation_quadrant(void);
// Freeze/unfreeze the display-facing quadrant. Motion sampling continues.
// Boards without dynamic rotation implement this as a no-op.
void imu_hal_set_rotation_locked(bool locked);

// Raw tilt for the Level app (and future motion features). Returns false when
// the board has no working accelerometer path.
bool imu_hal_ready(void);
bool imu_hal_read_accel(float* ax, float* ay, float* az);
// Fresh sample (not the rotation cache). Falls back to read_accel when unimplemented.
bool imu_hal_read_accel_live(float* ax, float* ay, float* az);

// Convenience: pitch/roll in degrees from gravity (board frame).
bool imu_hal_read_tilt(float* pitch_deg, float* roll_deg);

// Gravity Ball — bias calibration + deadzone (ported from Waveshare 04_Immersive_block).
void imu_hal_gravity_cal_reset(void);
// Runs one calibration pass; returns 0..100 progress, true when finished OK.
bool imu_hal_gravity_cal_step(int* pct_out);
bool imu_hal_gravity_ready(void);
// Calibrated horizontal accel in g; Z ignored. Used for rolling-ball motion.
bool imu_hal_read_gravity(float* gx, float* gy);
