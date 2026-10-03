#include "../../shell/app.h"
#include "../../shell/statusbar.h"
#include "../../theme.h"
#include "../../phosphor_cp.h"
#include "../../ble.h"
#include "../../hal/input_hal.h"
#include "../../hal/board_caps.h"
#include <Arduino.h>

LV_FONT_DECLARE(font_inter_28);
LV_FONT_DECLARE(font_inter_16);
LV_FONT_DECLARE(font_inter_14);
LV_FONT_DECLARE(font_phos_28);
LV_FONT_DECLARE(font_phos_40);
LV_FONT_DECLARE(font_phos_64);

// Push-to-talk into Claude Code via BLE HID Space. Whisper transcription and
// WiFi offload to the host bridge are not wired yet — the button drives the
// whole screen: a glass mic tile (the launcher's glass_tile() scaled up, with
// the soon-card's light dome giving it light to bend) that visibly takes the
// press while Space is held on the Mac.

#define VOICE_FOOT_H 34
#define HID_SPACE 0x2C

enum voice_state_t {
    VOICE_IDLE,
    VOICE_RECORDING,
    VOICE_PROCESSING,
};

static lv_obj_t* mic_btn    = nullptr;
static lv_obj_t* mic_glyph  = nullptr;
static lv_obj_t* state_lbl  = nullptr;
static lv_obj_t* level_bar  = nullptr;
static lv_obj_t* transcript = nullptr;

static voice_state_t state = VOICE_IDLE;
static bool          ptt_via_primary = false;
static uint32_t      processing_at = 0;

static void set_state(voice_state_t next) {
    state = next;
    if (!state_lbl) return;

    switch (state) {
    case VOICE_IDLE:
        lv_label_set_text(state_lbl, "Hold to talk");
        // The meter only reads while listening — idle hides it so the empty
        // track doesn't smudge behind the footnote text (their rows overlap;
        // while recording the footnote is emptied by ptt_start, so the bar
        // owns its zone).
        if (level_bar) {
            lv_bar_set_value(level_bar, 0, LV_ANIM_OFF);
            lv_obj_add_flag(level_bar, LV_OBJ_FLAG_HIDDEN);
        }
        break;
    case VOICE_RECORDING:
        lv_label_set_text(state_lbl, "Listening...");
        if (level_bar) lv_obj_clear_flag(level_bar, LV_OBJ_FLAG_HIDDEN);
        break;
    case VOICE_PROCESSING:
        lv_label_set_text(state_lbl, "Processing...");
        if (level_bar) lv_obj_clear_flag(level_bar, LV_OBJ_FLAG_HIDDEN);
        processing_at = lv_tick_get();
        break;
    }
}

static void ptt_start(void) {
    if (state == VOICE_RECORDING) return;
    if (ble_get_state() != BLE_STATE_CONNECTED) {
        if (transcript)
            lv_label_set_text(transcript, "Connect to your Mac first.");
        return;
    }
    ble_keyboard_press(HID_SPACE, 0);
    set_state(VOICE_RECORDING);
    if (transcript) lv_label_set_text(transcript, "");
}

static void ptt_stop(void) {
    if (state != VOICE_RECORDING) return;
    ble_keyboard_release();
    set_state(VOICE_PROCESSING);
}

// Hold-to-talk touch semantics: press-and-hold = Space down, release = Space
// up (ptt_via_primary means the BOOT button owns the press — see
// voice_on_button/voice_tick; that path must not double-handle releases).
// The pressed LOOK lives purely in LV_STATE_PRESSED styles on the button.
static void mic_event(lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        ptt_via_primary = false;
        ptt_start();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (!ptt_via_primary) ptt_stop();
    }
}

static void voice_create(lv_obj_t* root) {
    const int W = board_caps().width;
    const bool compact = (W < 400);
    const int btn = compact ? 150 : 240;

    // ---- Quiet light field (the glass needs light to bend) -----------------
    // App backdrop: the launcher duotone's own slice (warm crest → navy
    // floor, falloff ending mid-panel so the 5-bit gradient can't band).
    lv_obj_set_style_bg_color(root, THEME_BG_TOP, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_color(root, THEME_BG_BOT, 0);
    lv_obj_set_style_bg_grad_dir(root, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(root, 0, 0);
    lv_obj_set_style_bg_grad_stop(root, 230, 0);

    // The launcher's luminous light dome (soon.cpp recipe verbatim): a giant
    // cream ellipse, top edge off-panel, lower rim arcing across the field —
    // rendered OVER the duotone and THROUGH the translucent glass.
    lv_obj_t* dome = lv_obj_create(root);
    lv_obj_set_size(dome, W + W * 2 / 3, W);
    lv_obj_set_pos(dome, -W / 4, -W * 5 / 8);
    lv_obj_set_style_radius(dome, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dome, SHEEN_TINT, 0);
    lv_obj_set_style_bg_opa(dome, 34, 0);
    lv_obj_set_style_bg_grad_color(dome, SHEEN_TINT, 0);
    lv_obj_set_style_bg_grad_opa(dome, 0, 0);
    lv_obj_set_style_bg_grad_dir(dome, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(dome, 0, 0);
    lv_obj_set_style_bg_grad_stop(dome, 255, 0);
    lv_obj_set_style_border_color(dome, RIM_CREAM, 0);
    lv_obj_set_style_border_width(dome, 3, 0);
    lv_obj_set_style_border_opa(dome, 55, 0);
    lv_obj_set_style_border_side(dome, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_outline_width(dome, 0, 0);
    lv_obj_clear_flag(dome, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(dome, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(dome, LV_OBJ_FLAG_IGNORE_LAYOUT);

    lv_obj_t* title = lv_label_create(root);
    lv_label_set_text(title, "Voice");
    lv_obj_set_style_text_font(title, &font_inter_28, 0);
    lv_obj_set_style_text_color(title, THEME_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, STATUSBAR_H + (compact ? 8 : 14));

    // ---- The glass mic button ----------------------------------------------
    mic_btn = lv_btn_create(root);
    lv_obj_set_size(mic_btn, btn, btn);
    lv_obj_align(mic_btn, LV_ALIGN_CENTER, 0, -6);

    const lv_color_t hue  = lv_color_hex(0x5f8f46);   // voice tile hue
    // Backdrop light behind the button — the duotone at mid-screen (the pool
    // stop must pull that light through, exactly like the launcher's
    // wall_base_at()).
    const lv_color_t wall = lv_color_mix(THEME_BG_FLOOR, THEME_BG_TOP, 60);

    // LIFT fill, one VER gradient carrying the whole material (launcher
    // glass_tile numbers): top stop = the sheen (wall lifted hard toward
    // cream, hue as a cast), bottom stop the pool (wall lifted toward hue,
    // near-white kissed in). Brightness beats the backdrop by a wide margin
    // or the glass reads as a hole.
    // Critic r1: card read flat/murky. Lift hard: cream sheen dominates the
    // top (green demotes to a cast), pool lifts toward white and keeps the
    // green story — glass beats the backdrop.
    lv_obj_set_style_bg_color(mic_btn,
        lv_color_mix(lv_color_mix(wall, RIM_CREAM, 85), hue, 45), 0);
    lv_obj_set_style_bg_opa(mic_btn, 170, 0);
    lv_obj_set_style_bg_grad_color(mic_btn,
        lv_color_mix(lv_color_mix(wall, lv_color_hex(0xffffff), 18), hue, 55), 0);
    lv_obj_set_style_bg_grad_opa(mic_btn, 200, 0);
    lv_obj_set_style_bg_grad_dir(mic_btn, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(mic_btn, 0, 0);
    lv_obj_set_style_bg_grad_stop(mic_btn, 240, 0);

    // Soft puddle shadow + cream rim + dark outer contour (launcher-tuned).
    lv_obj_set_style_shadow_color(mic_btn, GLASS_SHADOW, 0);
    lv_obj_set_style_shadow_width(mic_btn, 14, 0);
    lv_obj_set_style_shadow_spread(mic_btn, 0, 0);
    lv_obj_set_style_shadow_ofs_y(mic_btn, 5, 0);
    lv_obj_set_style_shadow_opa(mic_btn, 25, 0);
    lv_obj_set_style_radius(mic_btn, btn / 2, 0);
    lv_obj_set_style_border_color(mic_btn, RIM_CREAM, 0);
    lv_obj_set_style_border_width(mic_btn, 3, 0);
    lv_obj_set_style_border_opa(mic_btn, 150, 0);
    lv_obj_set_style_outline_color(mic_btn, GLASS_SHADOW, 0);
    lv_obj_set_style_outline_width(mic_btn, 1, 0);
    lv_obj_set_style_outline_pad(mic_btn, 0, 0);
    lv_obj_set_style_outline_opa(mic_btn, 90, 0);
    lv_obj_clear_flag(mic_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(mic_btn, LV_SCROLLBAR_MODE_OFF);

    // Directional rim: a top-arc glint INSIDE the rim — a concentric circle
    // child drawing only its top border edge, so light-from-above follows
    // the curvature. (The launcher tile's rounded-rect glint child does not
    // survive a circular button: its straight chords leak past the disc —
    // build-1 defect, replaced by the arc.)
    lv_obj_t* glint = lv_obj_create(mic_btn);
    lv_obj_set_size(glint, btn - 12, btn - 12);
    lv_obj_align(glint, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_radius(glint, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(glint, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(glint, RIM_CREAM, 0);
    lv_obj_set_style_border_width(glint, 3, 0);
    lv_obj_set_style_border_side(glint, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_opa(glint, 120, 0);
    lv_obj_set_style_outline_width(glint, 0, 0);
    lv_obj_set_style_pad_all(glint, 0, 0);
    lv_obj_clear_flag(glint, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(glint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(glint, LV_OBJ_FLAG_IGNORE_LAYOUT);

    // Press feedback: the glass thickens — fill deepens, rim brightens, the
    // puddle pools darker. Style states only; no new widgets to clean up.
    lv_obj_set_style_bg_opa(mic_btn, 190, LV_STATE_PRESSED);
    lv_obj_set_style_bg_grad_opa(mic_btn, 210, LV_STATE_PRESSED);
    lv_obj_set_style_border_opa(mic_btn, 190, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_opa(mic_btn, 60, LV_STATE_PRESSED);

    lv_obj_add_event_cb(mic_btn, mic_event, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(mic_btn, mic_event, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(mic_btn, mic_event, LV_EVENT_PRESS_LOST, nullptr);

    // Glyph: dark ink on the light glass, optically centered (optical center
    // sits ~2px above geometric on the 40px Phosphor face).
    mic_glyph = lv_label_create(mic_btn);
    lv_label_set_text(mic_glyph, PH_MICROPHONE);
    lv_obj_set_style_text_font(mic_glyph,
                               compact ? &font_phos_28 : &font_phos_64, 0);
    lv_obj_set_style_text_color(mic_glyph, THEME_INK, 0);
    lv_obj_set_style_text_opa(mic_glyph, 235, 0);
    lv_obj_set_style_text_letter_space(mic_glyph, 0, 0);
    lv_obj_align(mic_glyph, LV_ALIGN_CENTER, 0, -2);

    state_lbl = lv_label_create(root);
    lv_label_set_text(state_lbl, "Hold to talk");
    lv_obj_set_style_text_font(state_lbl, &font_inter_16, 0);
    lv_obj_set_style_text_color(state_lbl, THEME_DIM, 0);
    lv_obj_set_width(state_lbl, btn);
    lv_obj_set_style_text_align(state_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(state_lbl, mic_btn, LV_ALIGN_OUT_BOTTOM_MID, 0, 18);

    level_bar = lv_bar_create(root);
    lv_obj_set_width(level_bar, btn);
    lv_obj_set_height(level_bar, 6);
    lv_bar_set_range(level_bar, 0, 100);
    lv_obj_set_style_bg_color(level_bar, THEME_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_radius(level_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(level_bar, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(level_bar, THEME_GREEN, LV_PART_INDICATOR);
    lv_obj_align_to(level_bar, state_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    transcript = lv_label_create(root);
    lv_label_set_text(transcript,
                      "BOOT also works as push-to-talk here.\n"
                      "Whisper + reply display need the host bridge.");
    lv_obj_set_style_text_font(transcript, &font_inter_14, 0);
    lv_obj_set_style_text_color(transcript, THEME_DIM, 0);
    lv_obj_set_style_text_opa(transcript, 200, 0);
    lv_obj_set_width(transcript, W - (compact ? 32 : 48));
    lv_label_set_long_mode(transcript, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(transcript, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(transcript, LV_ALIGN_BOTTOM_MID, 0, -(VOICE_FOOT_H + 12));

    set_state(VOICE_IDLE);
}

static void voice_destroy(void) {
    mic_btn = nullptr;
    mic_glyph = nullptr;
    state_lbl = nullptr;
    level_bar = nullptr;
    transcript = nullptr;
    ptt_via_primary = false;
    state = VOICE_IDLE;
}

static bool voice_on_button(app_btn_t btn) {
    if (btn != APP_BTN_PRIMARY) return false;
    ptt_via_primary = true;
    ptt_start();
    return true;
}

static void voice_tick(void) {
    if (ptt_via_primary && state == VOICE_RECORDING &&
        !input_hal_is_held(INPUT_BTN_PRIMARY)) {
        ptt_via_primary = false;
        ptt_stop();
    }

    if (state == VOICE_RECORDING && level_bar) {
        // Placeholder meter until audio_in HAL feeds real peaks.
        const int v = 25 + (int)(lv_tick_get() / 40 % 60);
        lv_bar_set_value(level_bar, v, LV_ANIM_OFF);
    }

    if (state == VOICE_PROCESSING && processing_at != 0 &&
        lv_tick_elaps(processing_at) > 1200) {
        processing_at = 0;
        set_state(VOICE_IDLE);
        if (transcript) {
            lv_label_set_text(transcript,
                              "Voice mode toggled on your Mac.\n"
                              "Transcription display needs the host bridge.");
        }
    }
}

extern const AppDef app_voice = {
    .id            = "voice",
    .title         = "Voice",
    .glyph         = PH_WAVEFORM,
    .tile_rgb      = 0x5f8f46,
    .blurb         = "Hold to talk. Whisper runs on your Mac and the text goes straight into Claude Code.",
    .required_caps = APP_CAP_MIC | APP_CAP_WIFI | APP_CAP_PSRAM,
    .persistent    = false,
    .immersive     = false,
    .create        = voice_create,
    .destroy       = voice_destroy,
    .tick          = voice_tick,
    .on_button     = voice_on_button,
    .on_usage      = nullptr,
    .on_ble        = nullptr,
    .on_battery    = nullptr,
};