#include "shell.h"
#include "apps.h"
#include "launcher.h"
#include "statusbar.h"
#include "../theme.h"
#include "../brightness.h"
#include "../hal/board_caps.h"
#include <string.h>

#define MAX_VISIBLE_APPS 24

static lv_obj_t* app_host  = nullptr;   // parent for the active app's root
static lv_obj_t* home_pill = nullptr;

// Capability-filtered app list, plus each app's root once it has been built.
static const AppDef* visible[MAX_VISIBLE_APPS];
static lv_obj_t*     roots[MAX_VISIBLE_APPS];
static int           visible_n = 0;

static const AppDef* fg       = nullptr;   // foreground app, NULL at the launcher
static int           fg_index = -1;
static const AppDef* creating = nullptr;   // set only during create()

// What this board can actually offer an app. BOARD_HAS_PSRAM is a build flag
// (not a board.h symbol) so shared code may legitimately test it; everything
// else comes from BoardCaps, per the no-#ifdef-BOARD_* rule.
static uint32_t board_cap_mask(void) {
    const BoardCaps& c = board_caps();
    uint32_t m = APP_CAP_BLE;   // every supported board has a BLE peripheral
    if (c.has_battery) m |= APP_CAP_BATTERY;
    if (c.has_imu)     m |= APP_CAP_IMU;
    if (c.has_sound)   m |= APP_CAP_SOUND;
    if (c.has_mic)     m |= APP_CAP_MIC;
    if (c.has_wifi)    m |= APP_CAP_WIFI;
#ifdef BOARD_HAS_PSRAM
    m |= APP_CAP_PSRAM;
#endif
    return m;
}

static lv_obj_t* make_app_root(void) {
    lv_obj_t* r = lv_obj_create(app_host);
    lv_obj_set_size(r, board_caps().width, board_caps().height);
    lv_obj_set_pos(r, 0, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

static void build_app(int i) {
    if (roots[i]) return;
    roots[i] = make_app_root();
    creating = visible[i];
    if (visible[i]->create) visible[i]->create(roots[i]);
    creating = nullptr;
}

static void home_pill_cb(lv_event_t* e) {
    (void)e;
    shell_go_home();
}

// Swipe up from anywhere in an app returns to the launcher — the iOS gesture.
// Apps bubble their events (LV_OBJ_FLAG_EVENT_BUBBLE) so this sees drags that
// start over app content, and LVGL suppresses the click when the drag exceeds
// the scroll threshold, so it can't be mistaken for a tap.
static void gesture_cb(lv_event_t* e) {
    (void)e;
    if (!fg) return;
    if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_TOP) shell_go_home();
}

static void build_home_pill(lv_obj_t* parent) {
    const int W = board_caps().width;
    const int H = board_caps().height;

    // A generous invisible hit area around a thin visible bar: the bar itself
    // is only a few pixels tall, which is far too small a touch target. It
    // lives in the bottom strip that Clawdmeter's status line leaves clear.
    home_pill = lv_obj_create(parent);
    lv_obj_set_size(home_pill, W / 2, 30);
    lv_obj_set_pos(home_pill, W / 4, H - 30);
    lv_obj_set_style_bg_opa(home_pill, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(home_pill, 0, 0);
    lv_obj_set_style_pad_all(home_pill, 0, 0);
    lv_obj_clear_flag(home_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(home_pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(home_pill, home_pill_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* bar = lv_obj_create(home_pill);
    lv_obj_set_size(bar, (W >= 400) ? 132 : 96, 5);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(bar, THEME_TEXT, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_50, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, 6);

    lv_obj_add_flag(home_pill, LV_OBJ_FLAG_HIDDEN);   // only shown inside apps
}

void shell_init(void) {
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, THEME_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, gesture_cb, LV_EVENT_GESTURE, NULL);

    const uint32_t have = board_cap_mask();
    visible_n = 0;
    for (int i = 0; i < apps_count() && visible_n < MAX_VISIBLE_APPS; i++) {
        const AppDef* d = apps_at(i);
        if (!d) continue;
        if ((d->required_caps & have) != d->required_caps) continue;  // board can't run it
        roots[visible_n] = nullptr;
        visible[visible_n++] = d;
    }

    // Child order sets the z-order: launcher and apps at the bottom, then the
    // home pill, then the status bar on top.
    launcher_init(scr);

    app_host = lv_obj_create(scr);
    lv_obj_set_size(app_host, board_caps().width, board_caps().height);
    lv_obj_set_pos(app_host, 0, 0);
    lv_obj_set_style_bg_opa(app_host, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(app_host, 0, 0);
    lv_obj_set_style_pad_all(app_host, 0, 0);
    lv_obj_clear_flag(app_host, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(app_host, LV_OBJ_FLAG_HIDDEN);

    build_home_pill(scr);
    statusbar_init(scr);

    // Persistent apps are built now, hidden, so they accumulate host data from
    // boot and are correct the instant they're opened.
    for (int i = 0; i < visible_n; i++) {
        if (!visible[i]->persistent) continue;
        build_app(i);
        lv_obj_add_flag(roots[i], LV_OBJ_FLAG_HIDDEN);
    }

    shell_go_home();
}

void shell_tick(void) {
    statusbar_tick();
    if (fg && fg->tick) fg->tick();
}

void shell_open(int visible_index) {
    if (visible_index < 0 || visible_index >= visible_n) return;
    if (fg_index == visible_index) return;

    // Leave the current app before entering the new one.
    if (fg) shell_go_home();

    build_app(visible_index);
    for (int i = 0; i < visible_n; i++) {
        if (roots[i]) lv_obj_add_flag(roots[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(roots[visible_index], LV_OBJ_FLAG_HIDDEN);

    fg       = visible[visible_index];
    fg_index = visible_index;

    launcher_set_visible(false);
    lv_obj_clear_flag(app_host, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(home_pill, LV_OBJ_FLAG_HIDDEN);
    statusbar_set_immersive(fg->immersive);
}

void shell_open_id(const char* id) {
    if (!id) return;
    for (int i = 0; i < visible_n; i++) {
        if (strcmp(visible[i]->id, id) == 0) { shell_open(i); return; }
    }
}

void shell_go_home(void) {
    if (fg && fg_index >= 0) {
        if (fg->persistent) {
            lv_obj_add_flag(roots[fg_index], LV_OBJ_FLAG_HIDDEN);
        } else {
            if (fg->destroy) fg->destroy();
            lv_obj_delete(roots[fg_index]);
            roots[fg_index] = nullptr;
        }
    }
    fg       = nullptr;
    fg_index = -1;

    lv_obj_add_flag(app_host, LV_OBJ_FLAG_HIDDEN);
    if (home_pill) lv_obj_add_flag(home_pill, LV_OBJ_FLAG_HIDDEN);
    statusbar_set_immersive(false);
    launcher_set_visible(true);
}

bool shell_is_home(void)            { return fg == nullptr; }
const AppDef* shell_foreground(void) { return creating ? creating : fg; }
int shell_app_count(void)            { return visible_n; }

const AppDef* shell_app_at(int visible_index) {
    if (visible_index < 0 || visible_index >= visible_n) return nullptr;
    return visible[visible_index];
}

bool shell_button(app_btn_t btn) {
    if (fg && fg->on_button && fg->on_button(btn)) return true;

    if (btn == APP_BTN_PWR) {
        // In an app the PWR key is "home"; on the launcher there's nothing to
        // leave, so it keeps its upstream job of cycling screen brightness.
        if (fg) shell_go_home();
        else    brightness_cycle();
        return true;
    }
    return false;   // PRIMARY/SECONDARY fall through to their HID actions
}

// ---- Host data fan-out: every built app hears about it, foreground or not ----

void shell_set_usage(const UsageData* data) {
    for (int i = 0; i < visible_n; i++) {
        if (roots[i] && visible[i]->on_usage) visible[i]->on_usage(data);
    }
}

void shell_set_ble(ble_state_t state) {
    statusbar_set_ble(state);
    for (int i = 0; i < visible_n; i++) {
        if (roots[i] && visible[i]->on_ble) visible[i]->on_ble(state);
    }
}

void shell_set_battery(int percent, bool charging) {
    statusbar_set_battery(percent, charging);
    for (int i = 0; i < visible_n; i++) {
        if (roots[i] && visible[i]->on_battery) visible[i]->on_battery(percent, charging);
    }
}
