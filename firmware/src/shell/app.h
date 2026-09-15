#pragma once
#include <lvgl.h>
#include <stdint.h>
#include "../data.h"
#include "../ble.h"

// ---- App contract ----
//
// ClawdDeck is a launcher plus a set of apps. An app never touches
// lv_screen_active(): the shell hands it a root container and the app builds
// its UI inside that. Apps stay board-agnostic the same way the rest of the
// shared code does — they declare the hardware they need via `requires` and
// the launcher hides the ones this board can't satisfy. See docs/apps.md.

// Hardware/feature an app depends on. Resolved against board_caps() at boot,
// so a mic-less board simply never shows the Voice tile.
enum app_cap_t {
    APP_CAP_NONE    = 0,
    APP_CAP_MIC     = 1u << 0,   // audio_in HAL (ES7210 dual mic on the 2.16)
    APP_CAP_WIFI    = 1u << 1,   // station mode; needed to reach the host bridge
    APP_CAP_IMU     = 1u << 2,
    APP_CAP_SOUND   = 1u << 3,   // speaker out
    APP_CAP_BATTERY = 1u << 4,
    APP_CAP_BLE     = 1u << 5,   // custom data service and/or HID keyboard
    APP_CAP_PSRAM   = 1u << 6,   // large buffers (splash animations, audio)
};

// Physical buttons, offered to the foreground app before the shell acts.
// PWR is the shell's "home" key, so an app only sees it if it asks to.
enum app_btn_t {
    APP_BTN_PRIMARY,     // BOOT
    APP_BTN_SECONDARY,   // second user button, if the board has one
    APP_BTN_PWR,
};

struct AppDef {
    const char* id;       // stable identifier, used by settings + serial cmds
    const char* title;    // shown under the launcher tile
    const char* glyph;    // LV_SYMBOL_* drawn on the tile
    uint32_t    tile_rgb; // tile background
    const char* blurb;    // one line on what the app does, for Settings/About

    uint32_t    requires;   // bitmask of app_cap_t

    // Persistent apps are built once during shell_init() and thereafter only
    // hidden and shown; they are never destroyed. Use this for apps whose
    // builder is not re-entrant (Clawdmeter's ui.cpp keeps module-level widget
    // pointers) or that must keep accumulating host data while backgrounded so
    // they're correct the instant they're opened. Everything else is built on
    // open and torn down on close, which keeps LVGL's heap flat.
    bool        persistent;

    // Full-bleed apps draw their own chrome and get the status bar suppressed
    // while in the foreground. Clawdmeter is immersive: it renders its own
    // clock, battery and corner mascot, and its splash view needs the whole
    // panel. The home pill stays available either way.
    bool        immersive;

    // Build the app's UI into `root` (already sized, positioned, transparent).
    void (*create)(lv_obj_t* root);

    // Release non-LVGL resources (timers, I2S, sockets). The shell deletes
    // `root` and its children itself. Never called for persistent apps.
    void (*destroy)(void);

    // Called every main-loop iteration while the app is in the foreground.
    void (*tick)(void);

    // Return true to consume the press and stop the shell acting on it.
    // Consuming APP_BTN_PWR means the user can't leave via the button — the
    // on-screen home pill still works, but prefer not to.
    bool (*on_button)(app_btn_t btn);

    // ---- Host data, delivered to every *created* app ----
    // These fire whether or not the app is in the foreground, so a persistent
    // app stays current while backgrounded. They are never called before the
    // app's create() has run, so implementations don't need null guards.
    void (*on_usage)(const UsageData* data);
    void (*on_ble)(ble_state_t state);
    void (*on_battery)(int percent, bool charging);
};
