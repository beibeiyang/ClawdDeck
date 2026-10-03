#pragma once
#include "app.h"

// The OS shell: owns the LVGL screen, the status bar, the launcher and the
// foreground app. main.cpp talks only to this — it never calls into an app or
// into ui.cpp directly.

void shell_init(void);
void shell_tick(void);

// ---- Navigation ----
void shell_go_home(void);
// Destroy every built-but-backgrounded app instance (persistent apps that
// were built at boot and keep running while hidden). Non-foreground only.
// Returns the number of app instances torn down.
int shell_kill_background(void);
// Built-but-backgrounded (persistent) app instances — the "background apps"
// count the Settings close-row shows.
int shell_background_count(void);
void shell_open(int visible_index);
void shell_open_id(const char* id);
bool shell_is_home(void);

// The app currently in the foreground, or being created right now (so an app's
// create() can read its own AppDef). NULL at the launcher.
const AppDef* shell_foreground(void);

// ---- Launcher's view of the app list (capability-filtered) ----
int           shell_app_count(void);
const AppDef* shell_app_at(int visible_index);

// ---- Input ----
// Returns true if the shell or the foreground app consumed the press, in which
// case main.cpp must not apply its default HID action.
bool shell_button(app_btn_t btn);

// ---- Host data fan-out ----
void shell_set_usage(const UsageData* data);
void shell_set_ble(ble_state_t state);
void shell_set_battery(int percent, bool charging);
