"""Voice dictation codec tests: the daemon's IMA-ADPCM decoder against the
board encoder convention (mirrored in Python here) + chunk-envelope framing.

Blocks are [pred:i16le][step_idx:u8][rsvd:u8][packed nibbles; low nibble
first]. Firmware sizes them to the negotiated BLE MTU; the legacy size was
92 bytes / 176 samples. The daemon preserves chunk boundaries so both forms
decode correctly.
"""
import numpy as np
import pytest

from claude_usage_daemon import ima_adpcm_decode


@pytest.fixture(autouse=True)
def _pin_whisper_engine(monkeypatch):
    """The legacy transport tests exercise the faster-whisper path; Whistle
    tests below opt back in explicitly."""
    import claude_usage_daemon as daemon
    monkeypatch.setattr(daemon, "resolve_voice_engine", lambda _choice: "whisper")

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


def test_variable_mtu_sized_blocks_decode_exact_sample_count():
    """The normal MTU-185 path carries 350 samples, while a smaller link can
    choose a different block size without changing the daemon decoder."""
    sizes = [350, 350, 128]
    blocks = []
    pred, idx = 0, 0
    phase = 0
    for size in sizes:
        t = np.arange(phase, phase + size) / 16000
        pcm = (np.sin(2 * np.pi * 330 * t) * 7000).astype(np.int16)
        block, pred, idx = encode_block(pcm, pred, idx)
        blocks.append(block)
        phase += size
    rec = ima_adpcm_decode(blocks, 16000)
    assert rec is not None and len(rec) == sum(sizes)

def test_header_only_eou_chunk_triggers_transcription():
    """The board's EOU marker is a bare 3-byte header (empty payload) —
    drain_voice must act on it, not discard it as too short."""
    import asyncio
    from claude_usage_daemon import Session

    sess = Session(client=None)
    calls = []

    async def fake_transcribe():
        calls.append(bytes(sess._voice_buf))

    sess._transcribe_voice = fake_transcribe
    block, _, _ = encode_block([0] * SAMPLES_PER_BLOCK)
    sess._on_voice(None, bytearray([0, 0, len(block)]) + block)
    sess._on_voice(None, bytearray([1, 0x01, 0]))   # EOU, as dictate_stop() sends it
    asyncio.run(sess.drain_voice())
    assert calls == [bytes(block)]


def test_zero_len_byte_still_carries_payload():
    """Firmware before the buf[2] fix left the envelope len byte at 0 on
    every chunk; the notify length is authoritative, so audio must survive."""
    import asyncio
    from claude_usage_daemon import Session

    sess = Session(client=None)
    calls = []

    async def fake_transcribe():
        calls.append(bytes(sess._voice_buf))

    sess._transcribe_voice = fake_transcribe
    block, _, _ = encode_block([100] * SAMPLES_PER_BLOCK)
    sess._on_voice(None, bytearray([0, 0, 0]) + block)   # len byte = 0, as observed
    sess._on_voice(None, bytearray([1, 0x01, 0]))
    asyncio.run(sess.drain_voice())
    assert calls == [bytes(block)]


def test_8khz_transport_is_upsampled_for_whisper(monkeypatch):
    """8 kHz is a compatibility fallback; duration stays correct and Whisper
    still receives the 16 kHz numpy waveform it expects."""
    import asyncio
    import claude_usage_daemon as daemon

    sess = daemon.Session(client=None)
    typed = []
    replies = []

    monkeypatch.setattr(daemon, "whisper_transcribe",
                        lambda pcm: typed.append(len(pcm)) or "fallback works")

    async def fake_write(payload):
        replies.append(payload)
        return True

    sess.write_bridge = fake_write
    pred, idx = 0, 0
    for seq in range(16):  # 2816 samples = 0.352 s at 8 kHz
        block, pred, idx = encode_block([1000] * 176, pred, idx)
        sess._on_voice(None, bytearray([seq, 0x02, len(block)]) + block)
    sess._on_voice(None, bytearray([16, 0x03, 0]))
    asyncio.run(sess.drain_voice())

    assert typed == [16 * 176 * 2]
    assert any(r.get("t") == "vt" and r.get("tx") == "fallback works" for r in replies)


@pytest.mark.parametrize("bad_flags", [0x05, 0x01])
def test_damaged_transport_is_rejected_not_transcribed(monkeypatch, bad_flags):
    """Queue overruns and sequence gaps must surface as retryable errors,
    never plausible-but-corrupted transcripts."""
    import asyncio
    import claude_usage_daemon as daemon

    sess = daemon.Session(client=None)
    called = []
    replies = []
    monkeypatch.setattr(daemon, "whisper_transcribe",
                        lambda _pcm: called.append(True) or "wrong")

    async def fake_write(payload):
        replies.append(payload)
        return True

    sess.write_bridge = fake_write
    block, _, _ = encode_block([1000] * 350)
    sess._on_voice(None, bytearray([0, 0, len(block)]) + block)
    if bad_flags == 0x01:  # create a sequence gap rather than an overrun flag
        sess._on_voice(None, bytearray([2, bad_flags, 0]))
    else:
        sess._on_voice(None, bytearray([1, bad_flags, 0]))
    asyncio.run(sess.drain_voice())

    assert not called
    assert replies[-1]["t"] == "err"
    assert "dropped" in replies[-1]["m"]


def test_restarted_stream_discards_stale_partial(monkeypatch):
    """A lost EOU leaves a partial utterance behind; the device's next stream
    restarts at seq 0. That must not be reported as packet loss, and the stale
    blocks must not be spliced into the new utterance."""
    import asyncio
    import claude_usage_daemon as daemon

    sess = daemon.Session(client=None)
    typed = []
    replies = []
    monkeypatch.setattr(daemon, "whisper_transcribe",
                        lambda pcm: typed.append(len(pcm)) or "ok")
    monkeypatch.setattr(daemon, "read_voice_cfg", lambda: {})

    async def fake_write(payload):
        replies.append(payload)
        return True

    sess.write_bridge = fake_write
    block, _, _ = encode_block([1000] * 350)
    for seq in range(7):          # stale stream 0..6, EOU lost
        sess._on_voice(None, bytearray([seq, 0, len(block)]) + block)
    for seq in range(20):         # fresh stream restarts at 0 (0.44 s)
        sess._on_voice(None, bytearray([seq, 0, len(block)]) + block)
    sess._on_voice(None, bytearray([20, 0x01, 0]))
    asyncio.run(sess.drain_voice())

    assert typed == [20 * 350]    # only the fresh utterance
    assert any(r.get("t") == "vt" and r.get("tx") == "ok" for r in replies)
    assert not any(r.get("t") == "err" for r in replies)


def test_seq_wrap_is_not_a_restart(monkeypatch):
    import asyncio
    import claude_usage_daemon as daemon

    sess = daemon.Session(client=None)
    typed = []
    monkeypatch.setattr(daemon, "whisper_transcribe",
                        lambda pcm: typed.append(len(pcm)) or "ok")

    async def fake_write(payload):
        return True

    sess.write_bridge = fake_write
    block, _, _ = encode_block([1000] * 350)
    for i in range(300):          # crosses 255 -> 0
        sess._on_voice(None, bytearray([i & 0xFF, 0, len(block)]) + block)
    sess._on_voice(None, bytearray([300 & 0xFF, 0x01, 0]))
    asyncio.run(sess.drain_voice())

    assert typed == [300 * 350]


def test_voice_chunk_wakes_main_loop():
    import asyncio
    import claude_usage_daemon as daemon

    async def run():
        sess = daemon.Session(client=None)
        loop = asyncio.get_running_loop()
        loop.call_later(0.05, sess._on_voice, None, bytearray([0, 0, 0]))
        t0 = loop.time()
        await sess.wait_for_wake(5)
        return loop.time() - t0

    assert asyncio.run(run()) < 1
