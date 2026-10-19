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

// ---- Voice dictation: 16 kHz stereo → mono → IMA-ADPCM 4-bit ---------------
// One call = one ADPCM block: 176 mono samples (11 ms) → 4-bit nibbles + an
// IMA header (pred int16 LE + step u8 + reserved u8) = 4 bytes + 88 packed.
// IMA table/logic = the ADPCM spec (Intel/DVI), decode lives on the daemon
// side. The encoder pulls from the SAME I2S handle as tick() (reads are
// exclusive per loop iteration — the voice app drives it, not micscope).
#define ADPCM_SAMPLES_PER_BLOCK 176   // 11 ms... kept ≤ 179 so one block + 3-byte envelope fits a min-MTU (185) notify
// IMA step-size table, 89 entries: indexes 64..88 saturate at 32767 —
// the spec's table, mirrored by the daemon decoder.
static const int32_t ima_step_table[89] = {
      7,   8,   9,  10,  11,  12,  13,  14,
     16,  17,  19,  21,  23,  25,  28,  31,
     34,  37,  41,  45,  50,  55,  60,  66,
     73,  80,  88,  97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963,1060,1166,1282,1410,
   1552,1707,1878,2066,2272,2499,2749,3024,
   32767,32767,32767,32767,32767,32767,32767,32767,
   32767,32767,32767,32767,32767,32767,32767,32767,
   32767,32767,32767,32767,32767,32767,32767
};
static int32_t adpcm_pred  = 0;    // encoder predictor
static int     adpcm_step  = 0;    // encoder step INDEX (0..88)
static bool    adpcm_reset_pending = true;

size_t audio_in_hal_read_adpcm(uint8_t* out, size_t cap) {
    if (!active || !mic_i2s || !out || cap < 4 + (ADPCM_SAMPLES_PER_BLOCK + 1) / 2)
        return 0;

    if (adpcm_reset_pending) {       // fresh utterance: predictors from zero
        adpcm_pred = 0;
        adpcm_step = 0;
        adpcm_reset_pending = false;
    }

    // Read 480 stereo frames from I2S (may come short — loop), fold to mono.
    int16_t mono[ADPCM_SAMPLES_PER_BLOCK];
    size_t have = 0;
    while (have < ADPCM_SAMPLES_PER_BLOCK) {
        int16_t st[64];
        const size_t got = mic_i2s->readBytes((char*)st, sizeof(st));
        if (got == 0) break;
        const size_t frames = got / (2 * sizeof(int16_t));
        for (size_t i = 0; i < frames && have < ADPCM_SAMPLES_PER_BLOCK; i++)
            mono[have++] = (int16_t)(((int32_t)st[i * 2] + st[i * 2 + 1]) / 2);
    }
    if (have < ADPCM_SAMPLES_PER_BLOCK && have == 0) return 0;

    // Zero-pad a short final block only on stop — here a short read means the
    // mic is not keeping up; emit what we have (block size = have samples).
    const size_t samples = have;
    const size_t nibbles = samples;
    const size_t bytes   = 3 + (nibbles + 1) / 2;   // 3 hdr + packed nibbles
    if (cap < bytes) return 0;

    // IMA header: predictor (int16 LE) + step index (u8) + reserved (u8)
    out[0] = (uint8_t)(adpcm_pred & 0xff);
    out[1] = (uint8_t)((adpcm_pred >> 8) & 0xff);
    out[2] = (uint8_t)adpcm_step;
    out[3] = 0;

    int32_t pred  = adpcm_pred;
    int     s_idx = adpcm_step;   // step INDEX 0..88; value = table[idx]
    for (size_t i = 0; i < samples; i++) {
        int32_t diff = (int32_t)mono[i] - pred;
        const int32_t s = (int32_t)ima_step_table[s_idx];
        int nib = 0;
        if (diff < 0) { nib = 8; diff = -diff; }
        if (diff >= s)      { nib |= 4; diff -= s; }
        if (diff >= s >> 1) { nib |= 2; diff -= s >> 1; }
        if (diff >= s >> 2) { nib |= 1; }
        // predictor update (clamp to int16)
        int32_t d = s >> 3;
        if (nib & 4) d += s;
        if (nib & 2) d += s >> 1;
        if (nib & 1) d += s >> 2;
        if (nib & 8) d = -d;
        pred += d;
        if (pred >  32767) pred =  32767;
        if (pred < -32768) pred = -32768;
        // step-index adjustment (0..88 legal against the 89-entry table)
        static const int8_t adj[16] = {-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};
        s_idx += adj[nib];
        if (s_idx < 0)  s_idx = 0;
        if (s_idx > 88) s_idx = 88;
        // pack nibble (low nibble first — IMA order)
        if (i & 1) out[3 + i / 2] |= (uint8_t)(nib << 4);
        else       out[3 + i / 2]  = (uint8_t)nib;
    }
    adpcm_pred = pred;
    adpcm_step = s_idx;

    // Bytes = header (4, incl. reserved nibble-half for odd safety) + packed.
    const size_t total = 4 + (samples + 1) / 2;
    return (cap < total) ? 0 : total;
}
