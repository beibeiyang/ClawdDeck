#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Microphone capture for Mic Scope and (eventually) Voice. Boards without a
// mic path no-op and audio_in_hal_start() returns false.

bool    audio_in_hal_start(void);
void    audio_in_hal_stop(void);
bool    audio_in_hal_active(void);
uint8_t audio_in_hal_peak(void);   // 0..100, smoothed RMS/peak
void    audio_in_hal_tick(void);   // poll I2S while the capture app is open

// Stereo interleaved samples @ 16 kHz for SpecAnalyzer FFT. Returns frames read.
size_t audio_in_hal_read_stereo(int16_t* out, size_t frames);

// ---- Voice dictation capture (dictate-to-panel) ---------------------------
// 16 kHz mono, IMA-ADPCM 4-bit blocks: [pred:i16le][step:u8][rsvd:u8] + 88
// packed nibbles (176 samples, 11 ms). Non-blocking: returns the next
// captured block (bytes written), or 0 when none is ready yet. Capture keeps
// running in real time between calls, so callers drain until 0 each tick.
// After audio_in_hal_stop() it still returns blocks captured before the stop
// (then 0), so the caller can flush the tail. Boards without a mic return 0.
size_t audio_in_hal_read_adpcm(uint8_t* out, size_t cap);
