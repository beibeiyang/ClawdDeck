#pragma once
#include <lvgl.h>
#include "data.h"
#include "ble.h"

enum screen_t {
    SCREEN_SPLASH,
    SCREEN_USAGE,
    SCREEN_COUNT,
};

// Builds the Clawdmeter view into `parent` (the app root handed over by the
// shell) rather than straight onto the active screen, so the launcher and the
// other apps can coexist with it. Not re-entrant — module-level widget
// pointers mean this runs exactly once, which is why the app is persistent.
void ui_init(lv_obj_t* parent);
void ui_update(const UsageData* data);
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);
void ui_toggle_splash(void);
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_on_pairing_mode(const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);
