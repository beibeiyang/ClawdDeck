#include "../../shell/app.h"
#include "../../theme.h"
#include "../../phosphor_cp.h"
#include "../../hal/board_caps.h"
#include "../../hal/audio_in_hal.h"
#include <math.h>
#ifndef BOARD_SIM
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif
#ifdef BOARD_HAS_PSRAM
#include <esp_heap_caps.h>
#endif

LV_FONT_DECLARE(font_inter_28);
LV_FONT_DECLARE(font_inter_14);

// Waveshare factory SpecAnalyzer (05_Spec_Analyzer) adapted to ClawdDeck.

#define N_SAMPLES     1024
#define STRIPE_COUNT  64
#define CANVAS_W      400
#define CANVAS_H      180

#ifdef BOARD_HAS_PSRAM
#define SPEC_ALLOC(sz) heap_caps_malloc((sz), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define SPEC_FREE(p)   heap_caps_free(p)
#else
#include <stdlib.h>
#define SPEC_ALLOC(sz) malloc(sz)
#define SPEC_FREE(p)   free(p)
#endif

#ifdef BOARD_AMOLED_216
#include "esp_dsp.h"
#endif

static lv_obj_t*   canvas      = nullptr;
static lv_timer_t* draw_timer  = nullptr;
static lv_draw_buf_t canvas_buf;
static uint8_t*    canvas_px   = nullptr;
static bool        capturing   = false;
static bool        fft_run     = false;

static float display_spectrum[STRIPE_COUNT];
static float peak_hold[STRIPE_COUNT];

#ifdef BOARD_AMOLED_216
static TaskHandle_t fft_task_h = nullptr;

static int16_t* raw_data     = nullptr;
static float*   audio_buffer = nullptr;
static float*   wind         = nullptr;
static float*   fft_buffer   = nullptr;
static float*   spectrum     = nullptr;

static bool alloc_fft_buffers(void) {
    raw_data     = (int16_t*)SPEC_ALLOC(N_SAMPLES * 2 * sizeof(int16_t));
    audio_buffer = (float*)SPEC_ALLOC(N_SAMPLES * sizeof(float));
    wind         = (float*)SPEC_ALLOC(N_SAMPLES * sizeof(float));
    fft_buffer   = (float*)SPEC_ALLOC(N_SAMPLES * 2 * sizeof(float));
    spectrum     = (float*)SPEC_ALLOC((N_SAMPLES / 2) * sizeof(float));
    return raw_data && audio_buffer && wind && fft_buffer && spectrum;
}

static void free_fft_buffers(void) {
    SPEC_FREE(raw_data);     raw_data     = nullptr;
    SPEC_FREE(audio_buffer); audio_buffer = nullptr;
    SPEC_FREE(wind);         wind         = nullptr;
    SPEC_FREE(fft_buffer);   fft_buffer   = nullptr;
    SPEC_FREE(spectrum);     spectrum     = nullptr;
}

static void fft_task(void* arg) {
    (void)arg;
    if (!raw_data) {
        vTaskDelete(nullptr);
        return;
    }
    if (dsps_fft2r_init_fc32(NULL, CONFIG_DSP_MAX_FFT_SIZE) != ESP_OK) {
        vTaskDelete(nullptr);
        return;
    }
    dsps_wind_hann_f32(wind, N_SAMPLES);

    while (fft_run) {
        const size_t got = audio_in_hal_read_stereo(raw_data, N_SAMPLES);
        if (got < N_SAMPLES / 4) {
            vTaskDelay(1);
            continue;
        }

        const size_t n = got < N_SAMPLES ? got : N_SAMPLES;
        for (size_t i = 0; i < n; i++) {
            const int16_t L = raw_data[i * 2];
            const int16_t R = raw_data[i * 2 + 1];
            audio_buffer[i] = (L + R) / (2.0f * 32768.0f);
        }
        for (size_t i = n; i < N_SAMPLES; i++) audio_buffer[i] = 0.0f;

        dsps_mul_f32(audio_buffer, wind, audio_buffer, N_SAMPLES, 1, 1, 1);
        for (int i = 0; i < N_SAMPLES; i++) {
            fft_buffer[2 * i]     = audio_buffer[i];
            fft_buffer[2 * i + 1] = 0.0f;
        }
        dsps_fft2r_fc32(fft_buffer, N_SAMPLES);
        dsps_bit_rev_fc32(fft_buffer, N_SAMPLES);

        for (int i = 0; i < N_SAMPLES / 2; i++) {
            const float re = fft_buffer[2 * i];
            const float im = fft_buffer[2 * i + 1];
            spectrum[i] = 20.0f * log10f(sqrtf(re * re + im * im) / (N_SAMPLES / 2) + 1e-9f);
        }

        for (int i = 0; i < STRIPE_COUNT; i++) {
            const int idx = i * (N_SAMPLES / 2) / STRIPE_COUNT;
            display_spectrum[i] = fmaxf(-90.0f, fminf(0.0f, spectrum[idx]));
        }
        vTaskDelay(1);
    }
    vTaskDelete(nullptr);
}
#else
static void sim_spectrum_tick(void) {
    static float phase = 0.0f;
    phase += 0.08f;
    for (int i = 0; i < STRIPE_COUNT; i++) {
        const float w = sinf(phase + (float)i * 0.22f) * 0.5f + 0.5f;
        display_spectrum[i] = -90.0f + w * 70.0f;
    }
}
#endif

static void draw_cb(lv_timer_t* timer) {
    lv_obj_t* c = (lv_obj_t*)lv_timer_get_user_data(timer);
    if (!c) return;

#ifndef BOARD_AMOLED_216
    sim_spectrum_tick();
#endif

    lv_layer_t layer;
    lv_canvas_init_layer(c, &layer);
    lv_canvas_fill_bg(c, lv_color_black(), LV_OPA_COVER);

    const int stripe_w = CANVAS_W / STRIPE_COUNT;
    const int center_y = CANVAS_H / 2;

    for (int i = 0; i < STRIPE_COUNT; i++) {
        float db = display_spectrum[i];
        float norm = (db + 90.0f) / 90.0f;
        norm = fmaxf(0.0f, fminf(1.0f, norm));
        norm = sqrtf(norm);
        int bar_h = (int)(norm * (CANVAS_H / 2));

        if (peak_hold[i] < (float)bar_h) peak_hold[i] = (float)bar_h;
        else {
            peak_hold[i] -= 2.0f;
            if (peak_hold[i] < 0.0f) peak_hold[i] = 0.0f;
        }

        const uint16_t hue = (uint16_t)(i * 270 / STRIPE_COUNT);
        lv_color_t color = lv_color_hsv_to_rgb(hue, 100, 100);

        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = color;
        dsc.bg_opa   = LV_OPA_COVER;

        const int x0 = i * stripe_w + 1;
        const int x1 = (i + 1) * stripe_w - 2;
        lv_area_t bar = { x0, center_y - bar_h, x1, center_y + bar_h };
        lv_draw_rect(&layer, &dsc, &bar);

        lv_area_t pk = { x0, center_y - (int)peak_hold[i] - 2, x1,
                         center_y - (int)peak_hold[i] };
        lv_draw_rect(&layer, &dsc, &pk);
        pk.y1 = center_y + (int)peak_hold[i];
        pk.y2 = center_y + (int)peak_hold[i] + 2;
        lv_draw_rect(&layer, &dsc, &pk);
    }

    lv_canvas_finish_layer(c, &layer);
}

static void style_factory_bg(lv_obj_t* root) {
    lv_obj_set_style_bg_color(root, lv_color_hex(0x050608), 0);
    lv_obj_set_style_bg_grad_color(root, lv_color_hex(0x151a28), 0);
    lv_obj_set_style_bg_grad_dir(root, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
}

static void micscope_create(lv_obj_t* root) {
    const int W = board_caps().width;
    const bool compact = (W < 400);

    style_factory_bg(root);

    lv_obj_t* title = lv_label_create(root);
    lv_label_set_text(title, "SpecAnalyzer");
    lv_obj_set_style_text_font(title, &font_inter_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xf0f4ff), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, compact ? 36 : 48);

    const uint32_t canvas_bytes = LV_DRAW_BUF_SIZE(CANVAS_W, CANVAS_H, LV_COLOR_FORMAT_RGB565);
    canvas_px = (uint8_t*)SPEC_ALLOC(canvas_bytes);
    if (!canvas_px) return;
    lv_draw_buf_init(&canvas_buf, CANVAS_W, CANVAS_H, LV_COLOR_FORMAT_RGB565,
                     CANVAS_W * 2, canvas_px, canvas_bytes);

    canvas = lv_canvas_create(root);
    lv_obj_set_size(canvas, CANVAS_W, CANVAS_H);
    lv_canvas_set_draw_buf(canvas, &canvas_buf);
    lv_obj_align(canvas, LV_ALIGN_CENTER, 0, compact ? 8 : 16);
    lv_obj_set_style_radius(canvas, 16, 0);
    lv_obj_set_style_clip_corner(canvas, true, 0);
    // Glass frame: cream rim + dark outline + puddle (the house recipe).
    lv_obj_set_style_border_color(canvas, RIM_CREAM, 0);
    lv_obj_set_style_border_width(canvas, 1, 0);
    lv_obj_set_style_border_opa(canvas, 110, 0);
    lv_obj_set_style_outline_color(canvas, RIM_DARK, 0);
    lv_obj_set_style_outline_width(canvas, 1, 0);
    lv_obj_set_style_outline_opa(canvas, 90, 0);
    lv_obj_set_style_shadow_color(canvas, GLASS_SHADOW, 0);
    lv_obj_set_style_shadow_width(canvas, 14, 0);
    lv_obj_set_style_shadow_ofs_y(canvas, 5, 0);
    lv_obj_set_style_shadow_opa(canvas, 25, 0);

    lv_obj_t* hint = lv_label_create(root);
    lv_obj_set_style_text_font(hint, &font_inter_14, 0);
    lv_obj_set_style_text_color(hint, THEME_DIM, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -(compact ? 28 : 36));

    for (int i = 0; i < STRIPE_COUNT; i++) {
        display_spectrum[i] = -90.0f;
        peak_hold[i]        = 0.0f;
    }

    capturing = audio_in_hal_start();
    lv_label_set_text(hint, capturing ? "Speak or tap near the mics"
                                      : "Microphone unavailable");

#ifdef BOARD_AMOLED_216
    if (capturing && alloc_fft_buffers()) {
        fft_run = true;
        xTaskCreate(fft_task, "spec_fft", 8192, nullptr, 4, &fft_task_h);
    } else if (capturing) {
        capturing = false;
        audio_in_hal_stop();
        lv_label_set_text(hint, "Out of memory for FFT");
    }
#endif

    draw_timer = lv_timer_create(draw_cb, 33, canvas);
}

static void micscope_destroy(void) {
#ifdef BOARD_AMOLED_216
    fft_run = false;
    if (fft_task_h) {
        vTaskDelay(pdMS_TO_TICKS(50));
        fft_task_h = nullptr;
    }
    free_fft_buffers();
#endif
    if (draw_timer) {
        lv_timer_delete(draw_timer);
        draw_timer = nullptr;
    }
    if (capturing) audio_in_hal_stop();
    capturing = false;
    SPEC_FREE(canvas_px);
    canvas_px = nullptr;
    canvas    = nullptr;
}

static void micscope_tick(void) {
#ifndef BOARD_AMOLED_216
    if (capturing) audio_in_hal_tick();
#endif
}

extern const AppDef app_micscope = {
    .id            = "micscope",
    .title         = "SpecAnalyzer",
    .glyph         = PH_MICROPHONE,
    .tile_rgb      = 0x8a4bc4,
    .blurb         = "Live audio spectrum — factory SpecAnalyzer demo.",
    .required_caps = APP_CAP_MIC,
    .persistent    = false,
    .immersive     = true,
    .create        = micscope_create,
    .destroy       = micscope_destroy,
    .tick          = micscope_tick,
    .on_button     = nullptr,
    .on_usage      = nullptr,
    .on_ble        = nullptr,
    .on_battery    = nullptr,
};
