#pragma once
#include <lvgl.h>
#include <stdint.h>

// ---- App contract ----
//
// ClawdDeck is a launcher plus a set of apps. An app never touches
// lv_screen_active(): the shell hands it a root container sized to the content
// area (below the status bar) and the app builds its UI inside that. Apps are
// board-agnostic in exactly the same way the rest of the shared code is — they
// declare the hardware they need via `requires` and the launcher hides the ones
// this board can't satisfy. See docs/apps.md.

// Hardware/feature an app depends on. The launcher resolves these against
// board_caps() at boot and only lists apps whose requirements are all met, so
// a mic-less board simply never shows the Voice tile.
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

// Physical buttons, forwarded to the foreground app before the shell acts on
// them. PWR is the shell's "home" key, so an app only sees it if it asks to.
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

    uint32_t    requires;   // bitmask of app_cap_t

    // Apps whose UI is cheap to rebuild get torn down on close, which keeps
    // LVGL's heap flat. Set `persistent` for apps whose builder is not
    // re-entrant (Clawdmeter's ui.cpp keeps module-level widget pointers) or
    // whose UI is expensive to reconstruct — those are built once and then
    // just hidden.
    bool        persistent;

    // Build the app's UI into `root`. Called once per open (or once ever, for
    // persistent apps). `root` is already sized, positioned and transparent.
    void (*create)(lv_obj_t* root);

    // Release non-LVGL resources (timers, I2S, sockets). The shell deletes
    // `root` and its children itself. NULL if there's nothing to release.
    void (*destroy)(void);

    // Called every main-loop iteration while the app is in the foreground.
    void (*tick)(void);

    // Return true to consume the press and stop the shell acting on it.
    // Consuming APP_BTN_PWR means the user can't leave via the button, so only
    // do it while genuinely modal — the on-screen home pill still works.
    bool (*on_button)(app_btn_t btn);
};
