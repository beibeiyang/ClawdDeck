#include "../../hal/imu_hal.h"

// No IMU on this kit. Orientation is fixed; rotation stays at 0.

void    imu_hal_init(void) {}
void    imu_hal_tick(void) {}
uint8_t imu_hal_rotation_quadrant(void) { return 0; }

bool imu_hal_ready(void) { return false; }

bool imu_hal_read_accel(float* ax, float* ay, float* az) {
    (void)ax; (void)ay; (void)az;
    return false;
}
