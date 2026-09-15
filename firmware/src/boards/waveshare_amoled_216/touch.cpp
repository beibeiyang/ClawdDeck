#include "../../hal/touch_hal.h"
#include "../../hal/imu_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <TouchDrvCSTXXX.hpp>

static TouchDrvCST92xx touch;

static volatile bool     touch_data_ready = false;
static volatile bool     touch_pressed = false;
static volatile uint16_t touch_x = 0;
static volatile uint16_t touch_y = 0;

static void IRAM_ATTR touch_isr(void) {
    touch_data_ready = true;
}

void touch_hal_init(void) {
    touch.setPins(TP_RST, TP_INT);
    if (!touch.begin(Wire, CST9220_ADDR, IIC_SDA, IIC_SCL)) {
        Serial.println("Touch init failed");
        return;
    }
    touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
    touch.setSwapXY(true);
    touch.setMirrorXY(true, false);
    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(TP_INT, touch_isr, FALLING);
    Serial.println("Touch init OK");
}

// Undo the software display rotation so LVGL gets coordinates in its own,
// unrotated frame.
//
// This board can't rotate in hardware (the CO5300's MADCTL only flips axes),
// so display.cpp rotates pixels on the CPU in rotate_strip(): it maps LVGL
// (x,y) to panel (px,py). The touch controller reports *panel* coordinates,
// so without the inverse map a rotated device sends taps to the wrong widget.
//
// Upstream never hit this because its only gesture was "tap anywhere to
// toggle" — with no spatially distinct targets a rotated touch frame is
// harmless. The launcher's tile grid is the first thing that exposes it.
//
// Inverting the forward transforms in rotate_strip(), keyed off the same
// imu_hal_rotation_quadrant() the renderer uses, keeps the two agreeing by
// construction rather than by guessing the panel's physical orientation:
//   q=1  90°: (x,y)->(S-1-y, x)        inverse: x=py,       y=S-1-px
//   q=2 180°: (x,y)->(S-1-x, S-1-y)    inverse: x=S-1-px,   y=S-1-py
//   q=3 270°: (x,y)->(y, S-1-x)        inverse: x=S-1-py,   y=px
static void unrotate_point(uint16_t* x, uint16_t* y) {
    // rotate_strip() assumes a square panel too (it uses LCD_WIDTH for both
    // axes), which holds for this 480x480 board.
    const uint16_t S  = LCD_WIDTH;
    const uint16_t px = *x;
    const uint16_t py = *y;

    switch (imu_hal_rotation_quadrant()) {
    case 1: *x = py;                    *y = (uint16_t)(S - 1 - px); break;
    case 2: *x = (uint16_t)(S - 1 - px); *y = (uint16_t)(S - 1 - py); break;
    case 3: *x = (uint16_t)(S - 1 - py); *y = px;                    break;
    default: break;   // q=0, panel and LVGL frames already agree
    }
}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    if (touch_data_ready) {
        touch_data_ready = false;
        int16_t tx[5], ty[5];
        uint8_t n = touch.getPoint(tx, ty, touch.getSupportTouchPoint());
        if (n > 0) {
            touch_pressed = true;
            touch_x = (uint16_t)tx[0];
            touch_y = (uint16_t)ty[0];
        } else {
            touch_pressed = false;
        }
    }
    // The cache stays in raw panel coordinates; the rotation is applied per
    // read so a rotation between reads is picked up immediately.
    *x = touch_x;
    *y = touch_y;
    *pressed = touch_pressed;
    unrotate_point(x, y);
}
