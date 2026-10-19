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
static uint32_t  capture_sample_rate = 16000;

static bool mic_hw_start(uint32_t sample_rate) {
    chime_suspend();

    audio_hal_codec_config_t cfg = {
        .adc_input  = AUDIO_HAL_ADC_INPUT_ALL,
        .codec_mode = AUDIO_HAL_CODEC_MODE_ENCODE,
        .i2s_iface  = {
            .mode    = AUDIO_HAL_MODE_SLAVE,
            .fmt     = AUDIO_HAL_I2S_NORMAL,
            .samples = sample_rate == 8000 ? AUDIO_HAL_08K_SAMPLES
                                           : AUDIO_HAL_16K_SAMPLES,
            .bits    = AUDIO_HAL_BIT_LENGTH_16BITS,
        },
    };

    if (es7210_adc_init(&Wire, &cfg) != ESP_OK ||
        es7210_adc_config_i2s(cfg.codec_mode, &cfg.i2s_iface) != ESP_OK) {
        chime_resume();
        return false;
    }
    // MIC1/MIC2 are the pair on SDOUT1 — the stereo stream we read. At 0 dB
    // speech landed at the noise floor (RMS ~5 of 32767).
    es7210_adc_set_gain(
        (es7210_input_mics_t)(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2),
        (es7210_gain_value_t)GAIN_37_5DB);
    es7210_adc_set_gain(
        (es7210_input_mics_t)(ES7210_INPUT_MIC3 | ES7210_INPUT_MIC4),
        (es7210_gain_value_t)GAIN_37_5DB);
    if (es7210_adc_ctrl_state(cfg.codec_mode, AUDIO_HAL_CTRL_START) != ESP_OK) {
        chime_resume();
        return false;
    }

    mic_i2s = new I2SClass();
    mic_i2s->setPins(SND_I2S_BCLK, SND_I2S_WS, -1, SND_I2S_DIN, SND_I2S_MCLK);
    if (!mic_i2s->begin(I2S_MODE_STD, sample_rate, I2S_DATA_BIT_WIDTH_16BIT,
                        I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
        es7210_adc_ctrl_state(cfg.codec_mode, AUDIO_HAL_CTRL_STOP);
        delete mic_i2s;
        mic_i2s = nullptr;
        chime_resume();
        return false;
    }
    mic_i2s->setTimeout(50);   // bound readBytes so the capture task stops promptly
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

static void capture_task_stop(void);   // dictation capture task (below)

bool audio_in_hal_start(void) {
    if (active) return true;
    capture_sample_rate = MIC_SAMPLE_RATE;
    if (!mic_hw_start(capture_sample_rate)) return false;
    active = true;
    peak   = 0;
    return true;
}

void audio_in_hal_stop(void) {
    if (!active) return;
    capture_task_stop();
    mic_hw_stop();
    active = false;
    peak   = 0;
}

bool audio_in_hal_active(void) { return active; }
uint8_t audio_in_hal_peak(void) { return peak; }

static TaskHandle_t capture_task = nullptr;

void audio_in_hal_tick(void) {
    // While dictating, the capture task owns the I2S stream (and the peak).
    if (!active || !mic_i2s || capture_task) return;
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

// ---- Voice dictation: stereo → mono → IMA-ADPCM 4-bit ----------------------
// Block size follows the subscribed BLE link's negotiated ATT MTU. At the
// usual macOS MTU 185, a 179-byte block carries 350 mono samples (21.9 ms at
// 16 kHz) and needs only 46 notifies/s instead of the former 91 notifies/s.
// Smaller links fall back to 8 kHz only when a 16 kHz stream would outrun the
// measured notification budget.
// IMA table/logic = the ADPCM spec (Intel/DVI), decode lives on the daemon
// side. The encoder pulls from the SAME I2S handle as tick() (reads are
// exclusive per loop iteration — the voice app drives it, not micscope).
#define ADPCM_HEADER_BYTES          4
#define ADPCM_MAX_BLOCK_BYTES     179   // + 3-byte envelope = 182 = MTU 185 value ceiling
#define ADPCM_MAX_SAMPLES          ((ADPCM_MAX_BLOCK_BYTES - ADPCM_HEADER_BYTES) * 2)
#define VOICE_MIN_PAYLOAD_BYTES     68   // 128 samples: 62.5 notifies/s at 8 kHz
#define VOICE_16K_MIN_SAMPLES      300   // ≤53.4 notifies/s leaves useful link headroom

static uint16_t adpcm_samples_per_block = 176;
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

// Encode one block from I2S into out (blocking on the mic). Task-side only.
static size_t encode_block(uint8_t* out, size_t cap) {
    const size_t target_samples = adpcm_samples_per_block;
    if (!mic_i2s || cap < ADPCM_HEADER_BYTES + (target_samples + 1) / 2)
        return 0;

    if (adpcm_reset_pending) {       // fresh utterance: predictors from zero
        adpcm_pred = 0;
        adpcm_step = 0;
        adpcm_reset_pending = false;
    }

    // Read one negotiated-size stereo block from I2S, then fold it to mono.
    int16_t mono[ADPCM_MAX_SAMPLES];
    size_t have = 0;
    while (have < target_samples) {
        int16_t st[64];
        const size_t got = mic_i2s->readBytes((char*)st, sizeof(st));
        if (got == 0) break;
        const size_t frames = got / (2 * sizeof(int16_t));
        for (size_t i = 0; i < frames && have < target_samples; i++)
            mono[have++] = (int16_t)(((int32_t)st[i * 2] + st[i * 2 + 1]) / 2);
    }
    if (have == 0) return 0;

    int32_t maxv = 0;   // level meter: the task owns the stream, so it owns peak
    for (size_t i = 0; i < have; i++) {
        const int32_t v = abs((int32_t)mono[i]);
        if (v > maxv) maxv = v;
    }
    const uint8_t instant = (uint8_t)(maxv > 8000 ? 100 : maxv * 100 / 8000);
    peak = (uint8_t)((peak * 3 + instant) / 4);

    // Zero-pad a short final block only on stop — here a short read means the
    // mic is not keeping up; emit what we have (block size = have samples).
    const size_t samples = have;
    const size_t total   = ADPCM_HEADER_BYTES + (samples + 1) / 2;
    if (cap < total) return 0;

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
        if (i & 1) out[4 + i / 2] |= (uint8_t)(nib << 4);
        else       out[4 + i / 2]  = (uint8_t)nib;
    }
    adpcm_pred = pred;
    adpcm_step = s_idx;

    return total;
}

// ---- Dictation capture task ------------------------------------------------
// The UI loop runs far slower than real time (~13 Hz with display flushes),
// and the I2S DMA ring only holds ~90 ms — pulling one block per loop lost
// most of the audio. A dedicated task keeps up with the mic and queues encoded
// blocks; audio_in_hal_read_adpcm() pops them without blocking.
#define BLOCK_QUEUE_DEPTH 96   // >2 s slack at the normal 350-sample block

struct adpcm_block_t {
    uint8_t len;
    uint8_t data[ADPCM_MAX_BLOCK_BYTES];
};

static QueueHandle_t block_q     = nullptr;
static volatile bool capture_run = false;
static volatile uint32_t captured_blocks = 0;
static volatile uint32_t dropped_blocks = 0;
static volatile uint16_t queue_high_water = 0;

static void capture_task_fn(void*) {
    adpcm_block_t blk;
    while (capture_run) {
        blk.len = (uint8_t)encode_block(blk.data, sizeof(blk.data));
        if (!blk.len) continue;
        captured_blocks = captured_blocks + 1;
        if (xQueueSend(block_q, &blk, 0) != pdTRUE) {
            dropped_blocks = dropped_blocks + 1;
        } else {
            const UBaseType_t waiting = uxQueueMessagesWaiting(block_q);
            if (waiting > queue_high_water) queue_high_water = (uint16_t)waiting;
        }
    }
    if (dropped_blocks)   // BLE couldn't keep up for long enough to fill the queue
        Serial.printf("audio_in: block queue full, dropped %lu blocks\n",
                      (unsigned long)dropped_blocks);
    capture_task = nullptr;
    vTaskDelete(nullptr);
}

static bool capture_task_start(void) {
    if (!block_q) block_q = xQueueCreate(BLOCK_QUEUE_DEPTH, sizeof(adpcm_block_t));
    if (!block_q) return false;
    xQueueReset(block_q);
    adpcm_reset_pending = true;
    captured_blocks = 0;
    dropped_blocks = 0;
    queue_high_water = 0;
    capture_run = true;
    if (xTaskCreate(capture_task_fn, "mic_capture", 4096, nullptr, 3,
                    &capture_task) != pdPASS) {
        capture_run  = false;
        capture_task = nullptr;
        return false;
    }
    return true;
}

bool audio_in_hal_start_voice(size_t max_payload) {
    if (active || max_payload < VOICE_MIN_PAYLOAD_BYTES) return false;
    if (max_payload > ADPCM_MAX_BLOCK_BYTES) max_payload = ADPCM_MAX_BLOCK_BYTES;

    adpcm_samples_per_block = (uint16_t)((max_payload - ADPCM_HEADER_BYTES) * 2);
    capture_sample_rate = adpcm_samples_per_block >= VOICE_16K_MIN_SAMPLES
                            ? 16000 : 8000;
    if (!mic_hw_start(capture_sample_rate)) return false;
    active = true;
    peak = 0;
    Serial.printf("audio_in: voice %lu Hz, %u samples/block, %u-byte payload\n",
                  (unsigned long)capture_sample_rate,
                  (unsigned)adpcm_samples_per_block,
                  (unsigned)(ADPCM_HEADER_BYTES + adpcm_samples_per_block / 2));
    return true;
}

uint32_t audio_in_hal_voice_sample_rate(void) {
    return capture_sample_rate;
}

void audio_in_hal_voice_stats(audio_in_voice_stats_t* out) {
    if (!out) return;
    out->captured_blocks = captured_blocks;
    out->dropped_blocks = dropped_blocks;
    out->queue_high_water = queue_high_water;
    out->samples_per_block = adpcm_samples_per_block;
}

static void capture_task_stop(void) {
    capture_run = false;
    // readBytes is bounded (setTimeout), so the task exits within one block.
    for (int i = 0; i < 40 && capture_task; i++) delay(5);
}

size_t audio_in_hal_read_adpcm(uint8_t* out, size_t cap) {
    if (!out) return 0;
    // Start capture on first read; after audio_in_hal_stop() keep handing
    // out what's still queued so the caller can flush the utterance's tail.
    if (active && mic_i2s && !capture_task && !capture_task_start()) return 0;
    if (!block_q) return 0;
    adpcm_block_t blk;
    if (xQueueReceive(block_q, &blk, 0) != pdTRUE) return 0;
    if (cap < blk.len) return 0;
    memcpy(out, blk.data, blk.len);
    return blk.len;
}
