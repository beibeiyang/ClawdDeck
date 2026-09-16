#include "../../shell/app.h"
#include "../../theme.h"
#include "../../ble.h"
#include "../../host_bridge.h"
#include "../../hal/board_caps.h"
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);

#define SESSIONS_FOOT_H 34
#define SESSION_ID_BUF    40

static lv_obj_t* link_lbl    = nullptr;
static lv_obj_t* usage_lbl   = nullptr;
static lv_obj_t* list_head   = nullptr;
static lv_obj_t* list_box    = nullptr;
static lv_obj_t* status_lbl  = nullptr;

static UsageData s_data = {};
static bool      s_have_data = false;
static ble_state_t s_ble = BLE_STATE_DISCONNECTED;
static host_bridge_state_t s_bridge = HOST_BRIDGE_IDLE;

static char session_ids[HOST_BRIDGE_MAX_SESSIONS][SESSION_ID_BUF];

static const char* link_text(void) {
    switch (s_ble) {
    case BLE_STATE_CONNECTED:    return "Mac connected";
    case BLE_STATE_ADVERTISING:  return "Waiting for Mac…";
    case BLE_STATE_DISCONNECTED:
    default:                     return "Not connected";
    }
}

static void refresh_usage_line(void) {
    if (!usage_lbl) return;
    if (!s_have_data || !s_data.valid) {
        lv_label_set_text(usage_lbl, "Usage: waiting for data…");
        return;
    }
    char buf[96];
    if (s_data.enterprise) {
        snprintf(buf, sizeof(buf), "Spending %d%% · period %d%%",
                 (int)(s_data.session_pct + 0.5f), s_data.time_pct);
    } else {
        snprintf(buf, sizeof(buf), "Session %d%% · weekly %d%%",
                 (int)(s_data.session_pct + 0.5f),
                 (int)(s_data.weekly_pct + 0.5f));
    }
    lv_label_set_text(usage_lbl, buf);
}

static void refresh_status(void) {
    if (!status_lbl) return;
    const char* msg = host_bridge_message();
    if (msg && msg[0]) {
        lv_label_set_text(status_lbl, msg);
        return;
    }
    switch (s_bridge) {
    case HOST_BRIDGE_LOADING:
        lv_label_set_text(status_lbl, "Loading recent sessions…");
        break;
    case HOST_BRIDGE_RESUME_SENT:
        lv_label_set_text(status_lbl, "Resuming on your Mac…");
        break;
    case HOST_BRIDGE_ERROR:
        lv_label_set_text(status_lbl, "Could not reach Claude on Mac");
        break;
    case HOST_BRIDGE_LIST_READY:
        if (host_bridge_session_count() == 0)
            lv_label_set_text(status_lbl, "No saved sessions found");
        else
            lv_label_set_text(status_lbl, "Tap a session to resume on your Mac");
        break;
    default:
        if (s_ble != BLE_STATE_CONNECTED)
            lv_label_set_text(status_lbl, "Connect your Mac to list sessions");
        else
            lv_label_set_text(status_lbl, "");
        break;
    }
}

static void row_click_cb(lv_event_t* e) {
    const char* id = (const char*)lv_event_get_user_data(e);
    if (!id || !id[0]) return;
    host_bridge_resume(id);
    s_bridge = HOST_BRIDGE_RESUME_SENT;
    refresh_status();
}

static void rebuild_list(void) {
    if (!list_box) return;
    lv_obj_clean(list_box);

    const int n = host_bridge_session_count();
    if (n <= 0) {
        lv_obj_t* empty = lv_label_create(list_box);
        lv_label_set_text(empty, "No recent Claude Code sessions");
        lv_obj_set_style_text_font(empty, &font_styrene_14, 0);
        lv_obj_set_style_text_color(empty, THEME_DIM, 0);
        lv_obj_set_width(empty, lv_pct(100));
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        return;
    }

    for (int i = 0; i < n; i++) {
        const HostSessionEntry* e = &host_bridge_sessions()[i];
        strncpy(session_ids[i], e->id, SESSION_ID_BUF - 1);
        session_ids[i][SESSION_ID_BUF - 1] = '\0';

        lv_obj_t* row = lv_button_create(list_box);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(row, THEME_PANEL, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 12, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, session_ids[i]);

        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(row, 2, 0);

        lv_obj_t* title = lv_label_create(row);
        lv_label_set_text(title, e->title[0] ? e->title : e->id);
        lv_obj_set_style_text_font(title, &font_styrene_16, 0);
        lv_obj_set_style_text_color(title, THEME_TEXT, 0);
        lv_obj_set_width(title, lv_pct(100));
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

        lv_obj_t* sub = lv_label_create(row);
        char subbuf[64];
        snprintf(subbuf, sizeof(subbuf), "%s", e->project[0] ? e->project : "Claude Code");
        lv_label_set_text(sub, subbuf);
        lv_obj_set_style_text_font(sub, &font_styrene_14, 0);
        lv_obj_set_style_text_color(sub, THEME_DIM, 0);
        lv_obj_set_width(sub, lv_pct(100));
    }
}

static void on_bridge_update(void) {
    host_bridge_state_t next = host_bridge_state();
    if (next == s_bridge && next != HOST_BRIDGE_LIST_READY) return;
    s_bridge = next;
    if (next == HOST_BRIDGE_LIST_READY) rebuild_list();
    refresh_status();
}

static void sessions_create(lv_obj_t* root) {
    const int W = board_caps().width;
    const bool compact = (W < 400);

    lv_obj_set_style_bg_color(root, THEME_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(root, compact ? 16 : 20, 0);
    lv_obj_set_style_pad_top(root, 8, 0);
    lv_obj_set_style_pad_bottom(root, SESSIONS_FOOT_H, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(root, compact ? 8 : 10, 0);

    lv_obj_t* title = lv_label_create(root);
    lv_label_set_text(title, "Sessions");
    lv_obj_set_style_text_font(title, &font_styrene_28, 0);
    lv_obj_set_style_text_color(title, THEME_TEXT, 0);

    link_lbl = lv_label_create(root);
    lv_obj_set_style_text_font(link_lbl, &font_styrene_14, 0);
    lv_obj_set_style_text_color(link_lbl, THEME_ACCENT, 0);
    lv_label_set_text(link_lbl, link_text());

    usage_lbl = lv_label_create(root);
    lv_obj_set_style_text_font(usage_lbl, &font_styrene_14, 0);
    lv_obj_set_style_text_color(usage_lbl, THEME_DIM, 0);
    refresh_usage_line();

    list_head = lv_label_create(root);
    lv_label_set_text(list_head, "Recent on Mac");
    lv_obj_set_style_text_font(list_head, &font_styrene_20, 0);
    lv_obj_set_style_text_color(list_head, THEME_TEXT, 0);

    list_box = lv_obj_create(root);
    lv_obj_set_width(list_box, lv_pct(100));
    lv_obj_set_flex_grow(list_box, 1);
    lv_obj_set_style_min_height(list_box, compact ? 180 : 220, 0);
    lv_obj_set_style_bg_opa(list_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_box, 0, 0);
    lv_obj_set_style_pad_all(list_box, 0, 0);
    lv_obj_set_style_pad_row(list_box, 8, 0);
    lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(list_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(list_box, LV_SCROLLBAR_MODE_AUTO);

    status_lbl = lv_label_create(root);
    lv_obj_set_style_text_font(status_lbl, &font_styrene_14, 0);
    lv_obj_set_style_text_color(status_lbl, THEME_DIM, 0);
    lv_obj_set_width(status_lbl, lv_pct(100));
    lv_label_set_long_mode(status_lbl, LV_LABEL_LONG_WRAP);

    host_bridge_reset();
    s_bridge = HOST_BRIDGE_IDLE;
    if (s_ble == BLE_STATE_CONNECTED) {
        host_bridge_request_list();
        s_bridge = HOST_BRIDGE_LOADING;
    }
    refresh_status();
}

static void sessions_destroy(void) {
    host_bridge_reset();
    link_lbl   = nullptr;
    usage_lbl  = nullptr;
    list_head  = nullptr;
    list_box   = nullptr;
    status_lbl = nullptr;
}

static void sessions_tick(void) {
    host_bridge_tick();
    on_bridge_update();
}

static void sessions_on_usage(const UsageData* data) {
    if (!data) return;
    s_data = *data;
    s_have_data = true;
    refresh_usage_line();
}

static void sessions_on_ble(ble_state_t state) {
    const ble_state_t prev = s_ble;
    s_ble = state;
    if (link_lbl) lv_label_set_text(link_lbl, link_text());
    if (state == BLE_STATE_CONNECTED && prev != BLE_STATE_CONNECTED) {
        host_bridge_request_list();
        s_bridge = HOST_BRIDGE_LOADING;
        refresh_status();
    }
}

extern const AppDef app_sessions = {
    .id            = "sessions",
    .title         = "Sessions",
    .glyph         = LV_SYMBOL_LIST,
    .tile_rgb      = 0x6b5b95,
    .blurb         = "Browse recent Claude Code sessions and resume them on your Mac.",
    .required_caps = APP_CAP_BLE,
    .persistent    = false,
    .immersive     = false,
    .create        = sessions_create,
    .destroy       = sessions_destroy,
    .tick          = sessions_tick,
    .on_button     = nullptr,
    .on_usage      = sessions_on_usage,
    .on_ble        = sessions_on_ble,
    .on_battery    = nullptr,
};
