#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#include "data.h"
#include "ble.h"
#include "shell/shell.h"
#include "shell/launcher.h"
#include "shell/clock_src.h"
#include "idle.h"
#include "idle_cfg.h"
#include "brightness.h"
#include "ui.h"

#include "hal/board_caps.h"
#include "hal/display_hal.h"
#include "hal/touch_hal.h"
#include "hal/input_hal.h"
#include "hal/power_hal.h"
#include "hal/imu_hal.h"
#include "hal/sound_hal.h"

static UsageData usage = {};

// ---- LVGL draw buffers (partial render mode) ----
// PSRAM-equipped boards (S3) can comfortably hold larger strips. PSRAM-free
// boards (e.g. ESP32-C6) allocate from internal SRAM, so we shrink the strip
// — 480×20 RGB565 = 19 KB × 2 buffers = 38 KB, fits beside everything else.
#ifdef BOARD_HAS_PSRAM
#define BUF_LINES 40
#define LV_BUF_CAPS (MALLOC_CAP_SPIRAM)
#else
#define BUF_LINES 20
#define LV_BUF_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif
static uint16_t* buf1 = nullptr;
static uint16_t* buf2 = nullptr;

static uint32_t my_tick(void) { return millis(); }

static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    display_hal_draw_bitmap(area->x1, area->y1, w, h, (uint16_t*)px_map);
    lv_display_flush_ready(disp);
}

static void rounder_cb(lv_event_t* e) {
    lv_area_t* area = (lv_area_t*)lv_event_get_param(e);
    display_hal_round_area(&area->x1, &area->y1, &area->x2, &area->y2);
}

// Touch policy is driven by IDLE_WAKE_ON_TOUCH:
//   true  → a press edge while asleep wakes the device and the first touch is
//           swallowed (mirrors the button wake-consumption); a press while
//           awake counts as activity.
//   false → touch never counts as activity and is fully swallowed while the
//           panel is dark, so pets/sleeves can't wake it overnight and LVGL
//           can't quietly toggle splash<->usage on a black panel.
// `touchdbg` toggles a press-edge dump of the point LVGL is about to act on,
// plus the rotation quadrant. Tapping a known tile and comparing the reported
// point against that tile's centre is the only way to pin down where a
// rotated-frame mismatch actually enters the chain.
// Press-edge dump of the point LVGL is about to act on, plus the rotation
// quadrant. Toggled by `touchdbg`; `imu` reads the quadrant without needing a
// tap at all, which is what makes a four-orientation check controlled instead
// of guesswork.
//
// Note when arming this over serial: opening the port resets the board, so a
// command sent immediately after connecting lands mid-boot and is dropped.
// Wait for the ready banner, or read the echo back and retry.
static bool touch_debug = false;

static void my_touch_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    uint16_t x, y;
    bool pressed;
    touch_hal_read(&x, &y, &pressed);
    const bool raw_pressed = pressed;

    if (touch_debug) {
        static bool dbg_was = false;
        if (raw_pressed && !dbg_was)
            Serial.printf("TOUCH q=%u x=%u y=%u\n",
                          imu_hal_rotation_quadrant(), x, y);
        dbg_was = raw_pressed;
    }

    if (IDLE_WAKE_ON_TOUCH) {
        static bool touch_was = false;
        static bool touch_wake_swallowed = false;
        if (raw_pressed && !touch_was) {
            // Press edge — consume as wake if asleep.
            if (idle_consume_wake_press()) {
                touch_wake_swallowed = true;
                pressed = false;
            }
        } else if (!raw_pressed && touch_was) {
            // Release edge.
            if (touch_wake_swallowed) {
                touch_wake_swallowed = false;
                pressed = false;
            }
        } else if (raw_pressed && touch_wake_swallowed) {
            // Held finger through wake — keep hiding until release.
            pressed = false;
        }
        touch_was = raw_pressed;
    } else if (idle_is_asleep()) {
        pressed = false;
    }

    if (pressed) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// Parse a JSON line into UsageData.
static bool parse_json(const char* json, UsageData* out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return false;
    }

    out->session_pct = doc["s"] | 0.0f;
    out->session_reset_mins = doc["sr"] | -1;
    out->weekly_pct = doc["w"] | 0.0f;
    out->weekly_reset_mins = doc["wr"] | -1;
    strlcpy(out->status, doc["st"] | "unknown", sizeof(out->status));
    out->chime = doc["c"] | false;   // absent (old daemon / chime off) → stay silent
    const char* acct = doc["acct"] | "pro";
    out->enterprise = (strcmp(acct, "ent") == 0);
    out->time_pct = doc["tp"] | 0;
    out->period_days = doc["pd"] | 30;
    strlcpy(out->reset_date, doc["rd"] | "", sizeof(out->reset_date));
    out->clock_epoch = doc["t"] | 0L;
    out->clock_fmt = doc["tf"] | 24;
    out->ok = doc["ok"] | false;
    out->valid = true;
    return true;
}

// ---- Serial command buffer ----
#define CMD_BUF_SIZE 64
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_pos = 0;

static void send_screenshot() {
#ifndef BOARD_HAS_PSRAM
    // A full RGB565 framebuffer doesn't fit in internal SRAM on PSRAM-free
    // boards (e.g. 480×480×2 = 460 KB). Capture is unsupported there.
    Serial.println("SCREENSHOT_UNSUPPORTED");
    return;
#else
    const uint32_t w = board_caps().width;
    const uint32_t h = board_caps().height;
    const uint32_t row_bytes = w * 2;
    const uint32_t buf_size = row_bytes * h;
    uint8_t* sbuf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!sbuf) {
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, w, h, LV_COLOR_FORMAT_RGB565, row_bytes, sbuf, buf_size);

    lv_result_t res = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &draw_buf);
    if (res != LV_RESULT_OK) {
        heap_caps_free(sbuf);
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    Serial.printf("SCREENSHOT_START %lu %lu %lu\n",
        (unsigned long)w, (unsigned long)h, (unsigned long)buf_size);
    Serial.flush();
    Serial.write(sbuf, buf_size);
    Serial.flush();
    Serial.println();
    Serial.println("SCREENSHOT_END");
    heap_caps_free(sbuf);
#endif
}

static void check_serial_cmd() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            cmd_buf[cmd_pos] = '\0';
            if (strcmp(cmd_buf, "screenshot") == 0) send_screenshot();
            else if (strcmp(cmd_buf, "buzz") == 0)  sound_hal_play_reset();
            // Navigate without touching the panel, so a screenshot of any app
            // can be scripted. `apps` lists the ids this board actually shows.
            else if (strcmp(cmd_buf, "home") == 0)  shell_go_home();
            else if (strncmp(cmd_buf, "open ", 5) == 0) shell_open_id(cmd_buf + 5);
            else if (strcmp(cmd_buf, "apps") == 0) {
                for (int i = 0; i < shell_app_count(); i++)
                    Serial.printf("%d %s\n", i, shell_app_at(i)->id);
            }
            else if (strncmp(cmd_buf, "page ", 5) == 0) {
                shell_go_home();
                launcher_goto_page(atoi(cmd_buf + 5));
            }
            else if (strcmp(cmd_buf, "touchdbg") == 0) {
                touch_debug = !touch_debug;
                Serial.printf("touchdbg %s\n", touch_debug ? "on" : "off");
            }
            else if (strcmp(cmd_buf, "imu") == 0) {
                float ax = 0.0f, ay = 0.0f, az = 0.0f;
                if (imu_hal_read_accel(&ax, &ay, &az))
                    Serial.printf("IMU q=%u ax=%.4f ay=%.4f az=%.4f\n",
                                  imu_hal_rotation_quadrant(), ax, ay, az);
                else
                    Serial.printf("IMU q=%u unavailable\n",
                                  imu_hal_rotation_quadrant());
            }
            cmd_pos = 0;
        } else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = c;
        }
    }
}

// Each board provides this. Must bring up the shared I2C bus (Wire.begin
// with the board's SDA/SCL pins) and any board-private hardware that has
// to settle before display/touch (e.g. an IO expander gating the LCD
// reset line). Called exactly once at the start of setup().
extern "C" void board_init(void);

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("{\"ready\":true}");

    board_init();

    display_hal_init();
    display_hal_begin();
    idle_init();        // takes over panel brightness and starts the idle timer
    brightness_init();  // load the user's saved brightness level and apply via idle

    power_hal_init();
    imu_hal_init();
    sound_hal_init();
    touch_hal_init();

    // ---- LVGL ----
    const int W = board_caps().width;
    const int H = board_caps().height;

    lv_init();
    lv_tick_set_cb(my_tick);

    buf1 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);
    buf2 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);

    lv_display_t* disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, W * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_cb);

    ble_init();
    input_hal_init();

    // The shell owns the screen from here: it builds the status bar, the
    // launcher and the persistent apps, then lands on the home screen.
    shell_init();
    shell_set_ble(ble_get_state());
    shell_set_battery(power_hal_battery_pct(), power_hal_is_charging());
    display_hal_tick();   // seed rotation baseline — after LVGL + IMU are up

    Serial.printf("Dashboard ready (%s, %dx%d), waiting for data on BLE...\n",
        board_caps().name, W, H);
}

static ble_state_t last_ble_state = BLE_STATE_INIT;

// Hold-to-pair gesture: hold the PWR button ~3s, then RELEASE → clear all BLE
// bonds and re-advertise. Clearing on *release* (not while held) is deliberate:
// holding to power the device OFF (AXP hardware shutdown at 8s) must not wipe
// the bond — a power-off hold never releases before shutdown. To stop a
// "chicken-out" release just before 8s from pairing, the gesture disarms at 6s.
//
//   ~1.5s long-press edge → PENDING
//   3.0s (+1500)          → ARMED   (release from here clears bonds)
//   6.0s (+4500)          → DISARMED (no clear; AXP powers off at 8s)
#define PAIR_ARM_AFTER_LONG_MS    1500   // 3.0s total
#define PAIR_DISARM_AFTER_LONG_MS 4500   // 6.0s total
enum pair_state_t { PAIR_IDLE, PAIR_PENDING, PAIR_ARMED };
static pair_state_t pair_state        = PAIR_IDLE;
static uint32_t     pair_long_seen_ms = 0;
// Release often queues a SHORT edge on the next poll, after pair_state is
// already IDLE — that was cycling brightness and felt like pairing failed.
static uint32_t     pair_ignore_short_until = 0;

static void pair_tick(void) {
    if (pair_state == PAIR_IDLE && power_hal_pwr_long_pressed()) {
        pair_state = PAIR_PENDING;
        pair_long_seen_ms = millis();
        (void)power_hal_pwr_released();  // drain any stale release edge
        Serial.println("PWR long-press: hold to ~3s then release to pair");
        return;
    }
    if (pair_state == PAIR_IDLE) return;

    // Advance before looking at release. On the frame where held crosses the
    // arm threshold, the AXP POSITIVE edge often lands in the same
    // power_hal_tick batch — checking release first still sees PENDING and
    // cancels as "too early", which is what a ~3s hold + prompt release hits.
    uint32_t held = millis() - pair_long_seen_ms;
    if (pair_state == PAIR_PENDING && held >= PAIR_ARM_AFTER_LONG_MS) {
        pair_state = PAIR_ARMED;
        Serial.println("Pair: armed — release to pair");
    } else if (pair_state == PAIR_ARMED && held >= PAIR_DISARM_AFTER_LONG_MS) {
        pair_state = PAIR_IDLE;  // power-off territory; don't pair
        Serial.println("Pair: disarmed (holding toward power-off)");
        return;
    }

    if (power_hal_pwr_released()) {
        if (pair_state == PAIR_ARMED) {
            Serial.println("Pair: released in window — clearing bonds, advertising");
            ble_clear_bonds();
            sound_hal_play_reset();   // audible ack — the screen may already show
                                      // the pairing hint, so there's little to see
            idle_set_awake_brightness(brightness_get()); // undo any rotation blank
            pair_ignore_short_until = millis() + 500;
            shell_set_ble(ble_get_state());
            ui_on_pairing_mode(ble_get_device_name(), ble_get_mac_address());
        } else {
            Serial.println("Pair: released too early — cancelled");
        }
        pair_state = PAIR_IDLE;
        // A long hold's release often queues a SHORT edge too. If we leave it
        // for the next loop iteration it fires after pair_state is back to
        // IDLE and the shell treats it as a normal tap — going home out of
        // Clawdmeter right after pairing succeeded, which reads as "pairing
        // didn't work". Upstream only brightness-cycled, so the bug was easy
        // to miss; with PWR-as-home it was obvious.
        (void)power_hal_pwr_pressed();
        return;
    }
}

void loop() {
    idle_tick();
    lv_timer_handler();
    shell_tick();       // status bar + the foreground app's own tick
    ble_tick();
    power_hal_tick();
    imu_hal_tick();
    sound_hal_tick();
    // Always run — skipping while idle-asleep could freeze a rotation blank at
    // brightness 0 (LVGL keeps rendering; the physical panel stays black).
    display_hal_tick();

    // ---- Physical buttons ----
    //   PRIMARY   → HID Space  (Claude Code voice-mode PTT)
    //   SECONDARY → HID Shift+Tab  (mode toggle; only if the board has one)
    //   PWR       → on splash: cycle animations; on usage: cycle brightness;
    //               hold ~3s + release: pairing mode
    // First press from sleep is consumed as a wake-only event by
    // idle_consume_wake_press(); the normal action fires from the second
    // press. Activity bookkeeping happens inside idle_consume_wake_press
    // so no separate idle_note_activity() call is needed here.
    {
        static bool primary_was = false;
        static bool primary_wake_swallowed = false;
        static bool primary_app_consumed = false;
        bool primary_now = input_hal_is_held(INPUT_BTN_PRIMARY);
        if (primary_now != primary_was) {
            if (primary_now) {
                // Offer the press to the foreground app first (Voice uses it as
                // push-to-talk); only fall through to HID if nobody wants it.
                if (idle_consume_wake_press())          primary_wake_swallowed = true;
                else if (shell_button(APP_BTN_PRIMARY)) primary_app_consumed = true;
                else ble_keyboard_press(0x2C, 0);  // HID Space, no mods
            } else {
                if (primary_wake_swallowed)   primary_wake_swallowed = false;
                else if (primary_app_consumed) primary_app_consumed = false;
                else                           ble_keyboard_release();
            }
            primary_was = primary_now;
        }

        if (board_caps().button_count >= 2) {
            static bool secondary_was = false;
            static bool secondary_wake_swallowed = false;
            static bool secondary_app_consumed = false;
            bool secondary_now = input_hal_is_held(INPUT_BTN_SECONDARY);
            if (secondary_now != secondary_was) {
                if (secondary_now) {
                    if (idle_consume_wake_press())            secondary_wake_swallowed = true;
                    else if (shell_button(APP_BTN_SECONDARY)) secondary_app_consumed = true;
                    else ble_keyboard_press(0x2B, 0x02);  // HID Tab + LEFT_SHIFT
                } else {
                    if (secondary_wake_swallowed)    secondary_wake_swallowed = false;
                    else if (secondary_app_consumed) secondary_app_consumed = false;
                    else                             ble_keyboard_release();
                }
                secondary_was = secondary_now;
            }
        }

        // Hold-to-pair runs first so release can clear bonds before a companion
        // SHORT edge from the same gesture is interpreted as a normal tap.
        pair_tick();

        if (power_hal_pwr_pressed()) {
            if (millis() < pair_ignore_short_until) {
                // Companion SHORT from the same release — already consumed above.
            } else if (!idle_consume_wake_press()) {
                // PWR is the shell's home key: inside an app it returns to the
                // launcher, and on the launcher it keeps cycling brightness.
                // Clawdmeter overrides that with brightness / splash cycling.
                // While a hold-to-pair gesture is in flight, ignore SHORT so a
                // mid-gesture edge can't navigate away.
                if (pair_state == PAIR_IDLE) shell_button(APP_BTN_PWR);
            }
        }
    }

    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state) {
        last_ble_state = bs;
        shell_set_ble(bs);
    }

    static int  last_pct      = -2;
    static bool last_charging = false;
    int  pct      = power_hal_battery_pct();
    bool charging = power_hal_is_charging();
    if (pct != last_pct || charging != last_charging) {
        if (pct != last_pct) ble_set_battery_level(pct);
        last_pct = pct;
        last_charging = charging;
        shell_set_battery(pct, charging);
    }

    check_serial_cmd();

    if (ble_has_data()) {
        if (parse_json(ble_get_data(), &usage)) {
            // Wall-clock time is device-wide (the status bar and Clock app use
            // it), so it's unpacked here; everything else about a usage payload
            // is Clawdmeter's business and handled in its on_usage hook.
            clock_src_set(usage.clock_epoch, usage.clock_fmt);
            shell_set_usage(&usage);
            ble_send_ack();
        } else {
            ble_send_nack();
        }
    }

    delay(5);
}
