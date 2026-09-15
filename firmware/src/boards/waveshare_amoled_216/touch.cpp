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

// Quadrant the setSwapXY/setMirrorXY calibration above was tuned against.
//
// Those calls are not a raw sensor->panel mapping: they were chosen so that
// reported points land directly in LVGL's frame for the orientation the board
// normally sits in, which the IMU reports as 3 (measured on hardware, not
// assumed). The rotation compensation is therefore already baked into them.
#define TOUCH_CAL_QUADRANT 3

// Bring a reported point into LVGL's frame across a rotation.
//
// This board can't rotate in hardware (the CO5300's MADCTL only flips axes),
// so display.cpp rotates pixels on the CPU in rotate_strip(), mapping LVGL
// (x,y) to panel (px,py). Touch reports its own pre-calibrated frame, so what
// we owe is the *delta* between the current quadrant and the calibration
// baseline. Applying rotate_strip()'s absolute inverse instead double-counts
// the baseline and rotates every tap by 90 degrees -- which is what hardware
// testing showed: the four launcher tiles resolved in a closed TL->TR->BR->BL
// cycle, in both orientations.
//
// Deriving it: with P the reported point, B the baseline quadrant and q the
// current one, P = F_B^-1(F_q(L)), so L = F_q^-1(F_B(P)) -- a rotation by
// (B - q) steps. At q == B that collapses to identity, preserving the
// known-good behaviour the old tap-anywhere UI depended on.
//
// F_k below are rotate_strip()'s forward maps, so the two stay in step:
//   k=1  90°: (x,y)->(S-1-y, x)
//   k=2 180°: (x,y)->(S-1-x, S-1-y)
//   k=3 270°: (x,y)->(y, S-1-x)
static void rotate_to_lvgl(uint16_t* x, uint16_t* y) {
    // rotate_strip() assumes a square panel too (it uses LCD_WIDTH for both
    // axes), which holds for this 480x480 board.
    const uint16_t S  = LCD_WIDTH;
    const uint16_t px = *x;
    const uint16_t py = *y;
    const uint8_t  k  =
        (uint8_t)((TOUCH_CAL_QUADRANT - imu_hal_rotation_quadrant()) & 3);

    switch (k) {
    case 1: *x = (uint16_t)(S - 1 - py); *y = px;                     break;
    case 2: *x = (uint16_t)(S - 1 - px); *y = (uint16_t)(S - 1 - py); break;
    case 3: *x = py;                     *y = (uint16_t)(S - 1 - px); break;
    default: break;   // aligned with the calibration, nothing to do
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
    rotate_to_lvgl(x, y);
}
