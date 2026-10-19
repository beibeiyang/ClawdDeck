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

// Level gravity — persistent mount-bias calibration plus a noise deadzone.
void imu_hal_gravity_cal_begin(void);
// Compatibility alias: cancels an in-progress calibration without clearing the
// last known-good persisted bias.
void imu_hal_gravity_cal_reset(void);
// Adds one fresh sample. Returns true only after a complete calibration was
// persisted. pct_out is 0..100, or -1 when a live sensor read fails.
bool imu_hal_gravity_cal_step(int* pct_out);
void imu_hal_gravity_cal_cancel(void);
bool imu_hal_gravity_ready(void);
// Calibrated horizontal accel in g; Z ignored. Used for rolling-ball motion.
bool imu_hal_read_gravity(float* gx, float* gy);
