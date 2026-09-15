#include "../../hal/board_caps.h"
#include "board.h"

static const BoardCaps caps = {
    .name = BOARD_NAME,
    .width = LCD_WIDTH,
    .height = LCD_HEIGHT,
    .button_count = 2,
    .has_rotation = true,
    .has_battery = true,
    .has_imu = true,
    .has_sound = true,
    .has_mic = true,     // ES7210 dual-mic array (I2C 0x40), I2S ASDOUT on GPIO10
    .has_wifi = true,
};

const BoardCaps& board_caps(void) { return caps; }
