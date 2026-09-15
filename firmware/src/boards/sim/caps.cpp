#include "../../hal/board_caps.h"
#include "board.h"

static const BoardCaps caps = {
    .name = BOARD_NAME,
    .width = LCD_WIDTH,
    .height = LCD_HEIGHT,
    .button_count = 2,      // B and N keys stand in for BOOT + GPIO18
    .has_rotation = false,
    .has_battery = true,    // fake battery, adjustable with -/=
    // Rotation stays off (no accelerometer to drive it), but the IMU is
    // advertised so the Level tile is reachable on the desktop; sim/imu.cpp
    // stands in for the sensor.
    .has_imu = true,
    // The sim claims sound/mic/wifi so every app tile is reachable on the
    // desktop; the HALs behind them are stubs that synthesize plausible data.
    .has_sound = true,
    .has_mic = true,
    .has_wifi = true,
};

const BoardCaps& board_caps(void) { return caps; }
