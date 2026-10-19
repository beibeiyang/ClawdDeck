#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Microphone capture for Mic Scope and Voice. Boards without a
// mic path no-op and audio_in_hal_start() returns false.

bool    audio_in_hal_start(void);
void    audio_in_hal_stop(void);
bool    audio_in_hal_active(void);
uint8_t audio_in_hal_peak(void);   // 0..100, smoothed RMS/peak
void    audio_in_hal_tick(void);   // poll I2S while the capture app is open

// Stereo interleaved samples @ 16 kHz for SpecAnalyzer FFT. Returns frames read.
size_t audio_in_hal_read_stereo(int16_t* out, size_t frames);

// ---- Voice dictation capture (dictate-to-panel) ---------------------------
// max_payload is the BLE bytes available for ONE ADPCM block (after the
// transport envelope). The mic HAL picks the largest even-sample block that
// fits. It keeps 16 kHz when that produces a safely sustainable notification
// rate, otherwise falls back to 8 kHz; callers advertise the chosen rate in
// the chunk flags. Boards without a mic return false.
bool     audio_in_hal_start_voice(size_t max_payload);
uint32_t audio_in_hal_voice_sample_rate(void);

struct audio_in_voice_stats_t {
    uint32_t captured_blocks;
    uint32_t dropped_blocks;
    uint16_t queue_high_water;
    uint16_t samples_per_block;
};

void audio_in_hal_voice_stats(audio_in_voice_stats_t* out);

// Mono IMA-ADPCM 4-bit blocks: [pred:i16le][step:u8][rsvd:u8] followed by
// packed nibbles. Non-blocking: returns the next captured block (bytes
// written), or 0 when none is ready. Capture continues in real time between
// calls. After audio_in_hal_stop(), queued tail blocks remain readable.
size_t audio_in_hal_read_adpcm(uint8_t* out, size_t cap);
