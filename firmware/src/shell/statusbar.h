#pragma once
#include <lvgl.h>
#include "../ble.h"

// Persistent top chrome: clock on the left, link + battery on the right.
// Created once at boot on top of everything else and shown/hidden by the
// shell, so apps never draw their own status row.

#define STATUSBAR_H 40

void statusbar_init(lv_obj_t* parent);
void statusbar_tick(void);
void statusbar_set_visible(bool visible);

void statusbar_set_battery(int percent, bool charging);
void statusbar_set_ble(ble_state_t state);
void statusbar_set_wifi(bool up, int rssi);

// Apps that own the whole panel (the splash/mascot view, Level) can ask for
// the bar to be suppressed while they're in the foreground.
void statusbar_set_immersive(bool immersive);
