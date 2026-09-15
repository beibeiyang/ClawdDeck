#include "../../shell/app.h"
#include "../../ui.h"
#include "../../splash.h"
#include "../../usage_rate.h"
#include "../../brightness.h"
#include "../../hal/sound_hal.h"
#include <Arduino.h>

// Clawdmeter — the original usage monitor, now one app among many.
//
// This is the only *persistent* app: ui.cpp keeps module-level widget pointers
// so its builder can't run twice, and it needs to keep taking host payloads
// while backgrounded so the numbers are right the moment you open it. The
// usage-rate / chime / mascot-rate logic used to live in main.cpp's loop and
// moved here, because all of it is this app's behaviour rather than the
// device's.
//
// It runs immersive: ui.cpp draws its own clock, battery and corner mascot,
// and the splash view wants the whole panel, so the shell's status bar stays
// out of the way.

static void clawdmeter_create(lv_obj_t* root) {
    ui_init(root);
    // Upstream booted straight to the splash because it was the idle view of a
    // single-purpose device. With a launcher in front, opening this tile is a
    // deliberate "show me my usage", so it lands on the numbers; the splash is
    // one tap away and still takes over as the screensaver.
    ui_show_screen(SCREEN_USAGE);
}

static void clawdmeter_tick(void) {
    ui_tick_anim();
    splash_tick();
    splash_mascot_tick();
}

static void clawdmeter_on_usage(const UsageData* data) {
    const int  group_before  = usage_rate_group();
    const bool session_reset = usage_rate_sample(data->session_pct);
    const int  group_after   = usage_rate_group();

    // The 5-hour session limit refilled — chime so the user knows they can use
    // Claude again without watching the screen. Gated on the daemon's opt-in
    // `chime` config; no-op on boards without a speaker.
    if (session_reset && data->chime) {
        Serial.println("session reset detected — chime");
        sound_hal_play_reset();
    }

    if (group_after != group_before) {
        Serial.printf("usage rate: group %d -> %d (s=%.2f%%)\n",
                      group_before, group_after, (double)data->session_pct);
        if (splash_is_active()) splash_pick_for_current_rate();
    }

    ui_update(data);
}

static void clawdmeter_on_ble(ble_state_t state) {
    ui_update_ble_status(state, ble_get_device_name(), ble_get_mac_address());
}

static bool clawdmeter_on_button(app_btn_t btn) {
    if (btn != APP_BTN_PWR) return false;
    // Upstream wired PWR short-press to in-app actions, not navigation. The
    // shell's default is "home", which broke both brightness cycling and the
    // hold-to-pair UX: release after a ~3s hold also queues a SHORT edge, and
    // going home on the next frame made it look like pairing never happened.
    if (ui_get_current_screen() == SCREEN_SPLASH) splash_next();
    else                                      brightness_cycle();
    return true;
}

static void clawdmeter_on_battery(int percent, bool charging) {
    ui_update_battery(percent, charging);
}

// `extern` is required: a file-scope `const` object has internal linkage in
// C++, so without it the registry in shell/apps.cpp can't see this symbol.
extern const AppDef app_clawdmeter = {
    .id         = "clawdmeter",
    .title      = "Clawdmeter",
    .glyph      = LV_SYMBOL_CHARGE,
    .tile_rgb   = 0xd97757,        // brand terra-cotta
    .required_caps = APP_CAP_BLE,
    .persistent = true,
    .immersive  = true,
    .create     = clawdmeter_create,
    .destroy    = nullptr,         // persistent: never torn down
    .tick       = clawdmeter_tick,
    .on_button  = clawdmeter_on_button,
    .on_usage   = clawdmeter_on_usage,
    .on_ble     = clawdmeter_on_ble,
    .on_battery = clawdmeter_on_battery,
};
