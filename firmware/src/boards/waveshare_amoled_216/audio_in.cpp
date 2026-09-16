#include "../../hal/audio_in_hal.h"
#include "board.h"
#include "../../chime.h"
#include "es7210/es7210.h"
#include <Arduino.h>
#include <Wire.h>
#include <ESP_I2S.h>

static I2SClass* mic_i2s = nullptr;
static bool      active  = false;
static uint8_t   peak    = 0;

static bool mic_hw_start(void) {
    chime_suspend();

    audio_hal_codec_config_t cfg = {
        .adc_input  = AUDIO_HAL_ADC_INPUT_ALL,
        .codec_mode = AUDIO_HAL_CODEC_MODE_ENCODE,
        .i2s_iface  = {
            .mode    = AUDIO_HAL_MODE_SLAVE,
            .fmt     = AUDIO_HAL_I2S_NORMAL,
            .samples = AUDIO_HAL_16K_SAMPLES,
            .bits    = AUDIO_HAL_BIT_LENGTH_16BITS,
        },
    };

    if (es7210_adc_init(&Wire, &cfg) != ESP_OK ||
        es7210_adc_config_i2s(cfg.codec_mode, &cfg.i2s_iface) != ESP_OK) {
        chime_resume();
        return false;
    }
    es7210_adc_set_gain(
        (es7210_input_mics_t)(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2),
        (es7210_gain_value_t)GAIN_0DB);
    es7210_adc_set_gain(
        (es7210_input_mics_t)(ES7210_INPUT_MIC3 | ES7210_INPUT_MIC4),
        (es7210_gain_value_t)GAIN_37_5DB);
    if (es7210_adc_ctrl_state(cfg.codec_mode, AUDIO_HAL_CTRL_START) != ESP_OK) {
        chime_resume();
        return false;
    }

    mic_i2s = new I2SClass();
    mic_i2s->setPins(SND_I2S_BCLK, SND_I2S_WS, -1, SND_I2S_DIN, SND_I2S_MCLK);
    if (!mic_i2s->begin(I2S_MODE_STD, MIC_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                        I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
        es7210_adc_ctrl_state(cfg.codec_mode, AUDIO_HAL_CTRL_STOP);
        delete mic_i2s;
        mic_i2s = nullptr;
        chime_resume();
        return false;
    }
    return true;
}

static void mic_hw_stop(void) {
    if (mic_i2s) {
        mic_i2s->end();
        delete mic_i2s;
        mic_i2s = nullptr;
    }
    es7210_adc_ctrl_state(AUDIO_HAL_CODEC_MODE_ENCODE, AUDIO_HAL_CTRL_STOP);
    chime_resume();
}

bool audio_in_hal_start(void) {
    if (active) return true;
    if (!mic_hw_start()) return false;
    active = true;
    peak   = 0;
    return true;
}

void audio_in_hal_stop(void) {
    if (!active) return;
    mic_hw_stop();
    active = false;
    peak   = 0;
}

bool audio_in_hal_active(void) { return active; }
uint8_t audio_in_hal_peak(void) { return peak; }

void audio_in_hal_tick(void) {
    if (!active || !mic_i2s) return;
    int16_t buf[256];
    const size_t got = mic_i2s->readBytes((char*)buf, sizeof(buf));
    if (got < sizeof(int16_t)) return;

    int32_t maxv = 0;
    const int n = (int)(got / sizeof(int16_t));
    for (int i = 0; i < n; i++) {
        const int32_t v = abs((int32_t)buf[i]);
        if (v > maxv) maxv = v;
    }
    uint8_t instant = (uint8_t)(maxv * 100 / 8000);
    if (instant > 100) instant = 100;
    peak = (uint8_t)((peak * 3 + instant) / 4);
}

size_t audio_in_hal_read_stereo(int16_t* out, size_t frames) {
    if (!active || !mic_i2s || !out || frames == 0) return 0;

    size_t total = 0;
    const size_t need_bytes = frames * 2 * sizeof(int16_t);
    uint8_t* dst = (uint8_t*)out;
    size_t remain = need_bytes;

    while (remain > 0) {
        const size_t got = mic_i2s->readBytes((char*)dst, remain);
        if (got == 0) break;
        dst    += got;
        remain -= got;
    }

    total = (need_bytes - remain) / (2 * sizeof(int16_t));
    return total;
}
