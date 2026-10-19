#include "../../shell/app.h"
#include "../../shell/statusbar.h"
#include "../../theme.h"
#include "../../phosphor_cp.h"
#include "../../ble.h"
#include "../../hal/input_hal.h"
#include "../../hal/board_caps.h"
#include "../../hal/audio_in_hal.h"
#include <Arduino.h>

LV_FONT_DECLARE(font_inter_20);
LV_FONT_DECLARE(font_inter_32);
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
    VOICE_RECORDING,       // PTT path: HID Space held (drive the Mac's voice mode)
    VOICE_PROCESSING,      // PTT path: waiting out the Mac's pipeline
    VOICE_DICTATING,       // dictate path: mic → ADPCM → BLE → daemon whisper
    VOICE_TRANSCRIBING,    // dictate path: EOU sent, waiting for 'vt' reply
};

static lv_obj_t* mic_btn    = nullptr;
static lv_obj_t* mic_glyph  = nullptr;
static lv_obj_t* state_lbl  = nullptr;
static lv_obj_t* level_bar  = nullptr;
static lv_obj_t* transcript = nullptr;

static voice_state_t state = VOICE_IDLE;
static bool          ptt_via_primary = false;
static uint32_t      processing_at = 0;

// Dictate stream state (mic tile hold): ADPCM blocks → ble_voice_send_chunk.
static uint8_t  dict_seq        = 0;
static bool     mic_capture_ok  = false;
static uint32_t transcribe_from = 0;
static uint8_t  dict_block[179];     // negotiated ADPCM block awaiting a notify slot
static size_t   dict_block_len  = 0;
static uint8_t  dict_audio_flags = 0; // bit1 = 8 kHz; bit2 reserved for overrun on EOU
static uint32_t dict_sent_blocks = 0;

// Send every captured block that's ready. A block whose notify fails (link
// busy) is kept and retried first next time, so nothing is dropped here.
static void dictate_pump(void) {
    for (int i = 0; i < 32; i++) {   // bound one tick's work
        if (!dict_block_len)
            dict_block_len = audio_in_hal_read_adpcm(dict_block, sizeof(dict_block));
        if (!dict_block_len) return;
        if (!ble_voice_send_chunk(dict_seq, dict_audio_flags,
                                  dict_block, (uint8_t)dict_block_len))
            return;
        dict_seq++;
        dict_sent_blocks++;
        dict_block_len = 0;
    }
}

// Stop the mic, flush every block captured before the stop, then close the
// stream with the EOU marker. Retries are bounded (~3 s) so a dead link can't
// hang the UI; a lost EOU would leave the daemon waiting forever.
static void dictate_finish(void) {
    audio_in_hal_stop();
    for (int i = 0; i < 600; i++) {
        dictate_pump();
        if (!dict_block_len) {
            dict_block_len = audio_in_hal_read_adpcm(dict_block, sizeof(dict_block));
            if (!dict_block_len) break;   // queue empty: tail flushed
        }
        delay(5);
    }
    audio_in_voice_stats_t stats = {};
    audio_in_hal_voice_stats(&stats);
    const uint8_t eou_flags = (uint8_t)(dict_audio_flags | 0x01 |
                              (stats.dropped_blocks ? 0x04 : 0x00));
    for (int i = 0; i < 100; i++) {
        if (ble_voice_send_chunk(dict_seq, eou_flags, nullptr, 0)) break;
        delay(5);
    }
    Serial.printf("voice: mtu=%u rate=%lu block_samples=%u captured=%lu sent=%lu "
                  "dropped=%lu queue_peak=%u\n",
                  ble_voice_mtu(),
                  (unsigned long)audio_in_hal_voice_sample_rate(),
                  stats.samples_per_block,
                  (unsigned long)stats.captured_blocks,
                  (unsigned long)dict_sent_blocks,
                  (unsigned long)stats.dropped_blocks,
                  stats.queue_high_water);
    dict_seq++;
    dict_block_len = 0;
}

static void set_state(voice_state_t next) {
    state = next;
    if (!state_lbl) return;

    switch (state) {
    case VOICE_IDLE:
        lv_label_set_text(state_lbl, "Hold mic = dictate · hold BOOT = Mac voice");
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
        lv_label_set_text(state_lbl, "Listening (Mac voice mode)...");
        if (level_bar) lv_obj_clear_flag(level_bar, LV_OBJ_FLAG_HIDDEN);
        break;
    case VOICE_PROCESSING:
        lv_label_set_text(state_lbl, "Processing...");
        if (level_bar) lv_obj_clear_flag(level_bar, LV_OBJ_FLAG_HIDDEN);
        processing_at = lv_tick_get();
        break;
    case VOICE_DICTATING:
        lv_label_set_text(state_lbl, "Recording (dictate)...");
        if (level_bar) lv_obj_clear_flag(level_bar, LV_OBJ_FLAG_HIDDEN);
        break;
    case VOICE_TRANSCRIBING:
        lv_label_set_text(state_lbl, "Transcribing on host...");
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

// ---- Dictate-to-panel (mic tile hold): capture → chunk → daemon whisper ----
static void dictate_start(void) {
    if (state != VOICE_IDLE) return;
    if (ble_get_state() != BLE_STATE_CONNECTED) {
        if (transcript)
            lv_label_set_text(transcript, "Connect to your Mac first.");
        return;
    }
    const size_t payload_capacity = ble_voice_payload_capacity();
    if (!payload_capacity) {
        if (transcript)
            lv_label_set_text(transcript, "Voice daemon not ready.");
        return;
    }
    mic_capture_ok = audio_in_hal_start_voice(payload_capacity);
    if (!mic_capture_ok) {
        if (transcript)
            lv_label_set_text(transcript, "Bluetooth audio link is too small.");
        return;
    }
    dict_seq       = 0;
    dict_block_len = 0;
    dict_sent_blocks = 0;
    dict_audio_flags = audio_in_hal_voice_sample_rate() == 8000 ? 0x02 : 0x00;
    set_state(VOICE_DICTATING);
    if (transcript) lv_label_set_text(transcript, "");
}

static void dictate_stop(void) {
    if (state != VOICE_DICTATING) return;
    dictate_finish();
    set_state(VOICE_TRANSCRIBING);
}

// Hold-to-talk touch semantics: press-and-hold = Space down, release = Space
// up (ptt_via_primary means the BOOT button owns the press — see
// voice_on_button/voice_tick; that path must not double-handle releases).
// The mic tile itself = DICTATE now (hold): PTT stays on BOOT per the design
// (voice_on_button). The pressed LOOK lives in LV_STATE_PRESSED styles.
// The touch controller reports brief releases while a finger is held, so a
// release only ends dictation once it has lasted RELEASE_DEBOUNCE_MS; a
// re-press inside that window continues the same utterance.
#define RELEASE_DEBOUNCE_MS 250
static bool     release_pending = false;
static uint32_t release_at      = 0;

static void mic_event(lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        if (release_pending) release_pending = false;   // flicker: keep recording
        else dictate_start();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (state == VOICE_DICTATING && !release_pending) {
            release_pending = true;
            release_at      = lv_tick_get();
        }
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
    lv_obj_set_style_text_font(title, &font_inter_32, 0);
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
        lv_color_mix(lv_color_mix(wall, RIM_CREAM, 85), hue, 170), 0);
    lv_obj_set_style_bg_opa(mic_btn, 170, 0);
    // r2b nit: the base pool = a flat tail — deepen it into a real pooled
    // glow (more saturation at the same stop, luma near-equal).
    lv_obj_set_style_bg_grad_color(mic_btn,
        lv_color_mix(lv_color_mix(wall, lv_color_hex(0xffffff), 14), hue, 110), 0);
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
    lv_obj_set_style_text_font(state_lbl, &font_inter_20, 0);
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
                      "Hold the mic tile — words land here and (daemon\n"
                      "voice_inject=on) type into your focused terminal.\n"
                      "BOOT = Mac voice-mode PTT.");
    lv_obj_set_style_text_font(transcript, &font_inter_16, 0);
    lv_obj_set_style_text_color(transcript, THEME_DIM, 0);
    lv_obj_set_style_text_opa(transcript, 200, 0);
    lv_obj_set_width(transcript, W - (compact ? 32 : 48));
    lv_label_set_long_mode(transcript, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(transcript, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(transcript, LV_ALIGN_BOTTOM_MID, 0, -(VOICE_FOOT_H + 12));

    set_state(VOICE_IDLE);
}

static void voice_destroy(void) {
    // A dictation cut short by leaving the app: stop the mic + close the
    // chunk stream so the daemon doesn't wait on an EOU that never comes.
    if (state == VOICE_DICTATING) dictate_finish();
    release_pending = false;
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
    // PTT (BOOT-held) release detection — unchanged.
    if (ptt_via_primary && state == VOICE_RECORDING &&
        !input_hal_is_held(INPUT_BTN_PRIMARY)) {
        ptt_via_primary = false;
        ptt_stop();
    }

    // Level meter: PTT shows a placeholder (the mic is the Mac's); dictation
    // shows the real peak, fed by the board's capture path.
    if (state == VOICE_RECORDING && level_bar) {
        const int v = 25 + (int)(lv_tick_get() / 40 % 60);
        (void)v;   // PTT path keeps the placeholder — the mic there is the Mac's
        lv_bar_set_value(level_bar, 25 + (int)(lv_tick_get() / 40 % 60), LV_ANIM_OFF);
    }
    if (state == VOICE_DICTATING && level_bar) {
        lv_bar_set_value(level_bar, audio_in_hal_peak(), LV_ANIM_OFF);
    }

    // Dictate chunk pump: capture runs in real time on its own; drain it.
    if (state == VOICE_DICTATING && mic_capture_ok) dictate_pump();
    if (release_pending && lv_tick_elaps(release_at) >= RELEASE_DEBOUNCE_MS) {
        release_pending = false;
        dictate_stop();
    }

    // Dictate reply: the 'vt' payload from the daemon (JSON bridge rx).
    if (state == VOICE_TRANSCRIBING) {
        const char* rx = ble_bridge_take_rx();
        if (rx && strstr(rx, "\"t\":\"vt\"")) {
            const char* tx = strstr(rx, "\"tx\":\"");
            if (tx) {
                tx += 5;
                static char msg[480];
                size_t o = 0;
                while (*tx && *tx != '"' && o < sizeof(msg) - 1) {
                    msg[o++] = *tx++;          // no escapes emitted by the daemon
                }
                msg[o] = '\0';
                set_state(VOICE_IDLE);
                if (transcript) lv_label_set_text(transcript, msg);
            }
        } else if (rx && strstr(rx, "\"t\":\"err\"")) {
            set_state(VOICE_IDLE);
            // Show the daemon's message ("m"); generic text if it has none.
            static char msg[96];
            const char* m = strstr(rx, "\"m\":\"");
            size_t o = 0;
            if (m) {
                m += 5;
                while (*m && *m != '"' && o < sizeof(msg) - 1) msg[o++] = *m++;
            }
            msg[o] = '\0';
            if (transcript)
                lv_label_set_text(transcript, o ? msg : "Host error — see daemon log.");
        }
        if (lv_tick_elaps(processing_at) > 15000) {  // 15s no reply = give up
            set_state(VOICE_IDLE);
            if (transcript) lv_label_set_text(transcript, "Transcription timed out.");
        }
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
