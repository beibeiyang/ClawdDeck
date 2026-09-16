#include "../../hal/imu_hal.h"

// No IMU on this template. If your board ships an accelerometer (e.g.
// QMI8658), copy a motion-enabled board implementation and adapt its pins
// here, then set BOARD_HAS_IMU=1 in board.h. Enable BOARD_HAS_ROTATION only
// when the display driver implements rotation without harming performance.

void    imu_hal_init(void) {}
void    imu_hal_tick(void) {}
uint8_t imu_hal_rotation_quadrant(void) { return 0; }

bool imu_hal_ready(void) { return false; }

bool imu_hal_read_accel(float* ax, float* ay, float* az) {
    (void)ax; (void)ay; (void)az;
    return false;
}
