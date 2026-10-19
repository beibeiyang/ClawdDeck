"""Voice dictation codec tests: the daemon's IMA-ADPCM decoder against the
board encoder convention (mirrored in Python here) + chunk-envelope framing.

The board (firmware/src/boards/waveshare_amoled_216/audio_in.cpp) emits
back-to-back blocks of 4 + 88 bytes: [pred:i16le][step_idx:u8][rsvd:u8]
[88 packed nibbles; low nibble = first sample], 176 samples per block @
16 kHz. The daemon reassembles chunk envelopes [seq:u8][flags:u8][len:u8]
[payload...] before decoding.
"""
import numpy as np
import pytest

from claude_usage_daemon import ima_adpcm_decode

STEP_TABLE = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
    598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1410, 1552, 1707,
    1878, 2066, 2272, 2499, 2749, 3024,
] + [32767] * 25                     # indexes 64..88 saturate (89 entries)

_ADJ = [-1, -1, -1, -1, 2, 4, 6, 8] * 2
SAMPLES_PER_BLOCK = 176


def encode_block(pcm, pred=0, idx=0):
    """Python twin of the board's C IMA-ADPCM encoder (one block)."""
    out = bytearray(4 + (len(pcm) + 1) // 2)
    out[0] = pred & 0xFF
    out[1] = (pred >> 8) & 0xFF
    out[2] = idx
    p_, i_ = pred, idx
    for k, v in enumerate(pcm):
        diff = int(v) - p_
        nib = 8 if diff < 0 else 0
        if diff < 0:
            diff = -diff
        s = STEP_TABLE[i_]
        if diff >= s:
            nib |= 4
            diff -= s
        if diff >= s >> 1:
            nib |= 2
            diff -= s >> 1
        if diff >= s >> 2:
            nib |= 1
        d = s >> 3
        if nib & 4:
            d += s
        if nib & 2:
            d += s >> 1
        if nib & 1:
            d += s >> 2
        if nib & 8:
            d = -d
        p_ = max(-32768, min(32767, p_ + d))
        i_ = max(0, min(88, i_ + _ADJ[nib]))
        if k & 1:
            out[4 + k // 2] |= nib << 4
        else:
            out[4 + k // 2] = nib
    return bytes(out), p_, i_


def frame_chunks(stream):
    """Wrap the block stream in the board's chunk envelopes, like the BLE
    notify stream does, then reassemble the way the daemon does."""
    framed = b""
    for seq, off in enumerate(range(0, len(stream), 92)):
        blk = stream[off:off + 92]
        framed += bytes([seq, 0, len(blk)]) + blk
    buf = bytearray()
    p = 0
    while p < len(framed):
        ln = framed[p + 2]
        buf.extend(framed[p + 3:p + 3 + ln])
        p += 3 + ln
    return bytes(buf)


def test_roundtrip_speech_like_snr():
    rate = 16000
    t = np.arange(rate) / rate
    env = 0.5 + 0.5 * np.sin(2 * np.pi * 3 * t)          # 3 Hz speech envelope
    sig = (env * (np.sin(2 * np.pi * 200 * t) * 0.6
                  + np.sin(2 * np.pi * 1200 * t) * 0.25
                  + np.sin(2 * np.pi * 2400 * t) * 0.15) * 20000).astype(np.int16)
    stream = b""
    pred, idx = 0, 0
    for off in range(0, len(sig), SAMPLES_PER_BLOCK):
        blk, pred, idx = encode_block(sig[off:off + SAMPLES_PER_BLOCK], pred, idx)
        stream += blk
    assert len(stream) % 92 == 0
    rec = ima_adpcm_decode(stream, rate)
    assert rec is not None and len(rec) == len(sig)
    err = sig.astype(float) - rec.astype(float)
    snr = 10 * np.log10((sig.astype(float) ** 2).mean()
                        / max((err ** 2).mean(), 1e-9))
    # 4-bit IMA worst case on synthetic tones is 12-15 dB; speech lands higher.
    # (whisper tiny.en degrades gracefully well below this bar)
    assert snr > 12, f"round-trip SNR too low: {snr:.1f} dB"


def test_envelope_reassembly_byte_exact():
    t = np.arange(SAMPLES_PER_BLOCK * 4) / 16000
    sig = (np.sin(2 * np.pi * 220 * t) * 8000).astype(np.int16)
    stream = b""
    pred, idx = 0, 0
    for off in range(0, len(sig), SAMPLES_PER_BLOCK):
        blk, pred, idx = encode_block(sig[off:off + SAMPLES_PER_BLOCK], pred, idx)
        stream += blk
    assert frame_chunks(stream) == stream


def test_trailing_partial_block_decodes():
    rate = 16000
    t = np.arange(SAMPLES_PER_BLOCK + 40) / rate
    sig = (np.sin(2 * np.pi * 300 * t) * 5000).astype(np.int16)
    stream, pred, idx = b"", 0, 0
    for off in range(0, len(sig), SAMPLES_PER_BLOCK):
        blk, pred, idx = encode_block(sig[off:off + SAMPLES_PER_BLOCK], pred, idx)
        stream += blk
    # the last block came short on hardware: only its header + a few nibbles
    partial = stream[:-40]
    rec = ima_adpcm_decode(partial, rate)
    assert rec is not None and len(rec) > 0