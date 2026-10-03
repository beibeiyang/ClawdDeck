#include "../../shell/app.h"
#include "../../shell/statusbar.h"
#include "../../theme.h"
#include "../../phosphor_cp.h"
#include "../../ble.h"
#include "../../host_bridge.h"
#include "../../hal/board_caps.h"
#include <stdio.h>
#include <string.h>

// Apple-class type stack (Inter, mirroring the shell): titles SemiBold 28,
// section headers / row titles 20, meta 14. The "·" and "…" glyphs have no
// codepoints in the converted Inter range (0x20-0x7E) — ASCII separators only.
LV_FONT_DECLARE(font_inter_16);
LV_FONT_DECLARE(font_inter_24);
LV_FONT_DECLARE(font_inter_32);
LV_FONT_DECLARE(font_inter_28);
LV_FONT_DECLARE(font_inter_20);
LV_FONT_DECLARE(font_inter_14);

#define SESSIONS_FOOT_H 34
#define SESSION_ID_BUF    40
#define SESSION_ROW_H    64   // uniform iOS list row (compact panel: 56)
#define SESSION_CHIP_H   30   // "Mac connected" glass pill

static lv_obj_t* chip_pill   = nullptr;
static lv_obj_t* chip_dot    = nullptr;
static lv_obj_t* link_lbl    = nullptr;
static lv_obj_t* usage_lbl   = nullptr;
static lv_obj_t* list_box    = nullptr;
static lv_obj_t* status_lbl  = nullptr;

static UsageData s_data = {};
static bool      s_have_data = false;
static ble_state_t s_ble = BLE_STATE_DISCONNECTED;
static host_bridge_state_t s_bridge = HOST_BRIDGE_IDLE;

static char session_ids[HOST_BRIDGE_MAX_SESSIONS][SESSION_ID_BUF];
// Chevron strokes: lv_line needs point arrays that outlive the widget, one
// per row slot. (0,0)->(6,7)->(0,14) = a thin iOS disclosure chevron.
static lv_point_precise_t chev_pts[HOST_BRIDGE_MAX_SESSIONS][3];

static const char* link_text(void) {
    switch (s_ble) {
    case BLE_STATE_CONNECTED:    return "Mac connected";
    case BLE_STATE_ADVERTISING:  return "Waiting for Mac...";
    case BLE_STATE_DISCONNECTED:
    default:                     return "Not connected";
    }
}

static lv_color_t dot_color(void) {
    switch (s_ble) {
    case BLE_STATE_CONNECTED:    return THEME_GREEN;
    case BLE_STATE_ADVERTISING:  return THEME_AMBER;
    default:                     return THEME_RED;
    }
}

static void refresh_usage_line(void) {
    if (!usage_lbl) return;
    if (!s_have_data || !s_data.valid) {
        lv_label_set_text(usage_lbl, "waiting for data...");
        return;
    }
    char buf[96];
    if (s_data.enterprise) {
        snprintf(buf, sizeof(buf), "Spending %d%% / period %d%%",
                 (int)(s_data.session_pct + 0.5f), s_data.time_pct);
    } else {
        snprintf(buf, sizeof(buf), "Session %d%% / weekly %d%%",
                 (int)(s_data.session_pct + 0.5f),
                 (int)(s_data.weekly_pct + 0.5f));
    }
    lv_label_set_text(usage_lbl, buf);
}

static void refresh_status(void) {
    if (!status_lbl) return;
    const char* msg = host_bridge_message();
    const bool error = (s_bridge == HOST_BRIDGE_ERROR);
    if (!msg || !msg[0]) {
        switch (s_bridge) {
        case HOST_BRIDGE_LOADING:
            msg = "Loading recent sessions...";
            break;
        case HOST_BRIDGE_RESUME_SENT:
            msg = "Resuming on your Mac...";
            break;
        case HOST_BRIDGE_LIST_READY:
            msg = (host_bridge_session_count() == 0)
                  ? "No saved sessions found"
                  : "Tap a session to resume on your Mac";
            break;
        default:
            msg = (s_ble != BLE_STATE_CONNECTED)
                  ? "Connect your Mac to list sessions" : "";
            break;
        }
    }
    lv_label_set_text(status_lbl, msg ? msg : "");
    lv_obj_set_style_text_color(status_lbl, error ? THEME_RED : THEME_DIM, 0);
}

static void row_click_cb(lv_event_t* e) {
    const char* id = (const char*)lv_event_get_user_data(e);
    if (!id || !id[0]) return;
    host_bridge_resume(id);
    s_bridge = HOST_BRIDGE_RESUME_SENT;
    refresh_status();
}

// One function owns the row material so every row reads as the same glass as
// the launcher tiles (shell/launcher.cpp glass_tile): the fill is the
// wallpaper's own light LIFTED — a vertical gradient whose top stop is the
// sheen (wall whitened, low hue cast) and whose bottom stop is the pool (hue
// gathering where light settles), rim on the object edge, thin dark outline
// outside, soft shadow puddle below. Chroma-dominant ramp so the 16bpp luma
// quantization never shows plateaus.
static void row_glass(lv_obj_t* row) {
    const lv_color_t hue  = lv_color_hex(0x7256b8);   // Sessions tile cast
    const lv_color_t wall = THEME_BG;                 // backdrop: navy floor

    // Critic r1: pools read painted (saturated purple). Neutralize the cast —
    // chroma only as a whisper in the pool, so the sheen reads as transmitted
    // light, not pigment.
    const lv_color_t top_c = lv_color_mix(lv_color_mix(wall, RIM_CREAM, 85),
                                          hue, 55);
    lv_obj_set_style_bg_color(row, top_c, 0);
    lv_obj_set_style_bg_opa(row, 145, 0);
    lv_obj_set_style_bg_grad_color(row,
        lv_color_mix(lv_color_mix(wall, lv_color_hex(0xffffff), 12), hue, 28), 0);
    lv_obj_set_style_bg_grad_opa(row, 185, 0);
    lv_obj_set_style_bg_grad_dir(row, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(row, 0, 0);
    lv_obj_set_style_bg_grad_stop(row, 240, 0);

    // Soft puddle straight down — grounds the glass, no halo.
    lv_obj_set_style_shadow_color(row, GLASS_SHADOW, 0);
    lv_obj_set_style_shadow_width(row, 14, 0);
    lv_obj_set_style_shadow_spread(row, 0, 0);
    lv_obj_set_style_shadow_ofs_y(row, 5, 0);
    lv_obj_set_style_shadow_opa(row, 25, 0);

    // Rim ON the object (cannot detach) + dark contour just outside.
    lv_obj_set_style_border_color(row, RIM_CREAM, 0);
    lv_obj_set_style_border_width(row, 2, 0);
    lv_obj_set_style_border_opa(row, 95, 0);
    lv_obj_set_style_outline_color(row, GLASS_SHADOW, 0);
    lv_obj_set_style_outline_width(row, 1, 0);
    lv_obj_set_style_outline_pad(row, 0, 0);
    lv_obj_set_style_outline_opa(row, 90, 0);

    // Pressed glass brightens and the puddle deepens.
    lv_obj_set_style_bg_opa(row, 190, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_opa(row, 60, LV_STATE_PRESSED);
}

// Top-half sheen on the chip — the pill's specular segment (statusbar recipe):
// a clipped RIM_HI wash that keeps to the upper half of the lozenge.
static void pill_sheen(lv_obj_t* pill, int w, int h) {
    lv_obj_t* sheen = lv_obj_create(pill);
    lv_obj_set_size(sheen, w, h / 2);
    lv_obj_set_pos(sheen, 0, 0);
    lv_obj_set_style_bg_color(sheen, RIM_HI, 0);
    lv_obj_set_style_bg_grad_color(sheen, RIM_HI, 0);
    lv_obj_set_style_bg_grad_opa(sheen, 0, 0);
    lv_obj_set_style_bg_grad_dir(sheen, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(sheen, 52, 0);
    lv_obj_set_style_radius(sheen, h / 2, 0);
    lv_obj_set_style_border_width(sheen, 0, 0);
    lv_obj_set_style_pad_all(sheen, 0, 0);
    lv_obj_clear_flag(sheen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(sheen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_clip_corner(sheen, true, 0);
    lv_obj_add_flag(sheen, LV_OBJ_FLAG_IGNORE_LAYOUT);
}

static void rebuild_list(void) {
    if (!list_box) return;
    lv_obj_clean(list_box);

    const int n = host_bridge_session_count();
    if (n <= 0) {
        lv_obj_t* empty = lv_label_create(list_box);
        lv_label_set_text(empty, "No recent Claude Code sessions");
        lv_obj_set_style_text_font(empty, &font_inter_16, 0);
        lv_obj_set_style_text_color(empty, THEME_DIM, 0);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_add_flag(empty, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 18);
        return;
    }

    const int W = board_caps().width;
    const bool compact = (W < 400);
    const int row_h = compact ? 56 : SESSION_ROW_H;
    const int row_pad_ver = (row_h - (24 + 18 + 4)) / 2;
    const int row_radius  = compact ? 12 : 14;

    for (int i = 0; i < n; i++) {
        const HostSessionEntry* e = &host_bridge_sessions()[i];
        strncpy(session_ids[i], e->id, SESSION_ID_BUF - 1);
        session_ids[i][SESSION_ID_BUF - 1] = '\0';

        lv_obj_t* row = lv_obj_create(list_box);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, row_h);
        lv_obj_set_style_radius(row, row_radius, 0);
        lv_obj_set_style_pad_hor(row, 14, 0);
        lv_obj_set_style_pad_left(row, 16, 0);
        // Right pad reserves the chevron lane; title never runs under it.
        lv_obj_set_style_pad_right(row, 34, 0);
        lv_obj_set_style_pad_ver(row, row_pad_ver, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, session_ids[i]);
        row_glass(row);

        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(row, 4, 0);

        lv_obj_t* title = lv_label_create(row);
        lv_label_set_text(title, e->title[0] ? e->title : e->id);
        lv_obj_set_style_text_font(title, &font_inter_24, 0);
        lv_obj_set_style_text_color(title, THEME_TEXT, 0);
        lv_obj_set_width(title, lv_pct(100));
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

        lv_obj_t* sub = lv_label_create(row);
        lv_label_set_text(sub, e->project[0] ? e->project : "Claude Code");
        lv_obj_set_style_text_font(sub, &font_inter_16, 0);
        lv_obj_set_style_text_color(sub, THEME_DIM, 0);
        lv_obj_set_width(sub, lv_pct(100));
        lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);

        // iOS disclosure chevron, drawn (no glyph in the Phosphor subset).
        chev_pts[i][0].x = 0; chev_pts[i][0].y = 0;
        chev_pts[i][1].x = 6; chev_pts[i][1].y = 7;
        chev_pts[i][2].x = 0; chev_pts[i][2].y = 14;
        lv_obj_t* chev = lv_line_create(row);
        lv_line_set_points(chev, chev_pts[i], 3);
        lv_obj_set_style_line_width(chev, 2, 0);
        lv_obj_set_style_line_color(chev, THEME_DIM, 0);
        lv_obj_set_style_line_opa(chev, 200, 0);
        lv_obj_set_style_line_rounded(chev, true, 0);
        lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -14, 0);
        lv_obj_add_flag(chev, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_clear_flag(chev, LV_OBJ_FLAG_CLICKABLE);
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

    // Fixed layout root: the page does NOT scroll (only the list does), so the
    // title can never slide under the floating status pill — content starts a
    // clear gap BELOW STATUSBAR_H.
    lv_obj_set_style_bg_color(root, THEME_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(root, compact ? 16 : 20, 0);
    lv_obj_set_style_pad_top(root, STATUSBAR_H + (compact ? 10 : 14), 0);
    lv_obj_set_style_pad_bottom(root, SESSIONS_FOOT_H, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(root, compact ? 10 : 12, 0);

    // Nav title.
    lv_obj_t* title = lv_label_create(root);
    lv_label_set_text(title, "Sessions");
    lv_obj_set_style_text_font(title, &font_inter_32, 0);
    lv_obj_set_style_text_color(title, THEME_TEXT, 0);

    // Status row: a small glass pill with a live status dot, usage meta
    // right-aligned opposite it (truncates on narrow panels).
    lv_obj_t* chip_row = lv_obj_create(root);
    lv_obj_set_width(chip_row, lv_pct(100));
    lv_obj_set_height(chip_row, SESSION_CHIP_H);
    lv_obj_set_style_bg_opa(chip_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chip_row, 0, 0);
    lv_obj_set_style_pad_all(chip_row, 0, 0);
    lv_obj_set_flex_flow(chip_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chip_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(chip_row, 10, 0);
    lv_obj_clear_flag(chip_row, LV_OBJ_FLAG_SCROLLABLE);

    chip_pill = lv_obj_create(chip_row);
    lv_obj_set_size(chip_pill, LV_SIZE_CONTENT, SESSION_CHIP_H);
    lv_obj_set_style_radius(chip_pill, SESSION_CHIP_H / 2, 0);
    lv_obj_set_style_pad_hor(chip_pill, 14, 0);
    lv_obj_clear_flag(chip_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(chip_pill, LV_OBJ_FLAG_CLICKABLE);
    // Same translucent pill recipe as the status bar's glass (scaled down):
    // cream-tinted wall light on top pooling dark at the base, warm rim,
    // dark outer contour. One fill carries the material.
    lv_obj_set_style_bg_color(chip_pill,
        lv_color_mix(THEME_BG_TOP, SHEEN_TINT, 40), 0);
    lv_obj_set_style_bg_opa(chip_pill, 130, 0);
    lv_obj_set_style_bg_grad_color(chip_pill,
        lv_color_mix(THEME_BG_BOT, SHEEN_TINT, 60), 0);
    lv_obj_set_style_bg_grad_opa(chip_pill, 190, 0);
    lv_obj_set_style_bg_grad_dir(chip_pill, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(chip_pill, 0, 0);
    lv_obj_set_style_bg_grad_stop(chip_pill, 255, 0);
    lv_obj_set_style_border_color(chip_pill, RIM_CREAM, 0);
    lv_obj_set_style_border_width(chip_pill, 2, 0);
    lv_obj_set_style_border_opa(chip_pill, 150, 0);
    lv_obj_set_style_outline_color(chip_pill, GLASS_SHADOW, 0);
    lv_obj_set_style_outline_width(chip_pill, 1, 0);
    lv_obj_set_style_outline_pad(chip_pill, 0, 0);
    lv_obj_set_style_outline_opa(chip_pill, 80, 0);

    lv_obj_t* chip_inner = lv_obj_create(chip_pill);
    lv_obj_set_size(chip_inner, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(chip_inner, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chip_inner, 0, 0);
    lv_obj_set_style_pad_all(chip_inner, 0, 0);
    lv_obj_set_flex_flow(chip_inner, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chip_inner, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(chip_inner, 6, 0);
    lv_obj_clear_flag(chip_inner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(chip_inner, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(chip_inner, LV_ALIGN_CENTER, 0, 0);

    chip_dot = lv_obj_create(chip_inner);
    lv_obj_set_size(chip_dot, 8, 8);
    lv_obj_set_style_radius(chip_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(chip_dot, dot_color(), 0);
    lv_obj_set_style_bg_opa(chip_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(chip_dot, 0, 0);
    lv_obj_clear_flag(chip_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(chip_dot, LV_OBJ_FLAG_CLICKABLE);

    link_lbl = lv_label_create(chip_inner);
    lv_obj_set_style_text_font(link_lbl, &font_inter_16, 0);
    lv_obj_set_style_text_color(link_lbl, THEME_TEXT, 0);
    lv_label_set_text(link_lbl, link_text());

    usage_lbl = lv_label_create(chip_row);
    lv_obj_set_style_text_font(usage_lbl, &font_inter_16, 0);
    lv_obj_set_style_text_color(usage_lbl, THEME_DIM, 0);
    lv_label_set_long_mode(usage_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(usage_lbl, 1);
    lv_obj_set_style_text_align(usage_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    refresh_usage_line();

    // Section header.
    lv_obj_t* head = lv_label_create(root);
    lv_label_set_text(head, "Recent on Mac");
    lv_obj_set_style_text_font(head, &font_inter_24, 0);
    lv_obj_set_style_text_color(head, THEME_TEXT, 0);

    // The list — the ONLY scrolling region.
    list_box = lv_obj_create(root);
    // Critic r1: don't grow the list to pin the status line at the floor —
    // the footer rides below the content (iOS rhythm) and the dead zone dies.
    lv_obj_set_width(list_box, lv_pct(100));
    lv_obj_set_style_min_height(list_box, compact ? 150 : 200, 0);
    lv_obj_set_style_bg_opa(list_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_box, 0, 0);
    lv_obj_set_style_pad_all(list_box, 0, 0);
    lv_obj_set_style_pad_top(list_box, 2, 0);
    lv_obj_set_style_pad_row(list_box, compact ? 8 : 10, 0);
    lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(list_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(list_box, LV_SCROLLBAR_MODE_AUTO);

    // Status line sits under the list, above the home-pill strip.
    status_lbl = lv_label_create(root);
    lv_obj_set_style_text_font(status_lbl, &font_inter_16, 0);
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
    chip_pill   = nullptr;
    chip_dot    = nullptr;
    link_lbl    = nullptr;
    usage_lbl   = nullptr;
    list_box    = nullptr;
    status_lbl  = nullptr;
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
    if (chip_dot) lv_obj_set_style_bg_color(chip_dot, dot_color(), 0);
    if (state == BLE_STATE_CONNECTED && prev != BLE_STATE_CONNECTED) {
        host_bridge_request_list();
        s_bridge = HOST_BRIDGE_LOADING;
        refresh_status();
    }
}

// `extern` is required: a file-scope `const` object has internal linkage in
// C++, so without it the registry in shell/apps.cpp can't see this symbol.
extern const AppDef app_sessions = {
    .id            = "sessions",
    .title         = "Sessions",
    .glyph         = PH_LIST,
    .tile_rgb      = 0x7256b8,
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