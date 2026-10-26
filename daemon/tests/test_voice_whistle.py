"""Live dictation via Whistle: partial transcripts stream to the panel while
the user is still talking, the final pass replaces them on end-of-utterance.

The real WhistleEngine (single worker thread + executor) runs here; only the
model is faked, so these tests need neither cactus-needle nor its weights.
"""
import asyncio
import json

import pytest

import claude_usage_daemon as daemon
from tests.test_voice_code import encode_block


class FakeModel:
    def __init__(self):
        self.chunk_sizes = []
        self.final_samples = None
        self.kw = None

    def stream(self, chunks, language=None, keywords=None):
        self.kw = (language, keywords)
        for i, chunk in enumerate(chunks):
            self.chunk_sizes.append(len(chunk))
            yield {"text": f"word{i}", "pending": f"maybe{i}"}
        yield {"text": "", "pending": ""}

    def transcribe(self, audio, language=None, keywords=None):
        self.final_samples = len(audio)
        return {"text": "final \u201cquoted\u201d caf\u00e9 text"}


@pytest.fixture
def fake_model(monkeypatch):
    model = FakeModel()
    monkeypatch.setattr(daemon, "_whistle_engine", None)
    monkeypatch.setattr(daemon.WhistleEngine, "_load", lambda self: model)
    monkeypatch.setattr(daemon, "resolve_voice_engine", lambda _c: "whistle")
    monkeypatch.setattr(daemon, "read_voice_cfg", lambda: {
        "voice_engine": "whistle", "voice_keywords": ["Claude"],
        "voice_language": "en"})
    monkeypatch.setattr(daemon, "_ax_prompted", False)
    return model


def _session(replies):
    sess = daemon.Session(client=None)

    async def fake_write(payload, quiet=False):
        replies.append(payload)
        return True

    sess.write_bridge = fake_write
    return sess


def _send_blocks(sess, start, count, samples=350):
    pred = idx = 0
    for seq in range(start, start + count):
        block, pred, idx = encode_block([800, -800] * (samples // 2), pred, idx)
        sess._on_voice(None, bytearray([seq & 0xFF, 0, len(block)]) + block)


async def _settle(sess, rounds=20):
    """Let worker-thread partials land on the loop, then drain them."""
    for _ in range(rounds):
        await asyncio.sleep(0.01)
        sess._partial_at = 0.0          # bypass the 4 Hz throttle in tests
        await sess.drain_voice()


def test_partials_stream_before_final(fake_model):
    replies = []

    async def run():
        sess = _session(replies)
        sess._loop = asyncio.get_running_loop()
        _send_blocks(sess, 0, 46)       # ~1 s of 16 kHz audio, still talking
        await sess.drain_voice()
        await _settle(sess)
        partials_mid_speech = [r for r in replies if r["t"] == "vp"]
        _send_blocks(sess, 46, 23)
        sess._on_voice(None, bytearray([69, 0x01, 0]))   # release: EOU
        await sess.drain_voice()
        return partials_mid_speech

    mid = asyncio.run(run())

    assert mid, "no live partial reached the panel while audio was flowing"
    assert mid[-1]["c"].startswith("word0")
    assert mid[-1]["p"].startswith("maybe")
    # The final whole-clip pass replaces the partials; nothing follows it.
    final = replies[-1]
    assert final["t"] == "vt" and final["tx"] == 'final "quoted" cafe text'
    assert final["inj"] == "off" and final["m"] == ""   # display-only mode
    assert not any(r["t"] == "err" for r in replies)
    # Every decoded sample reached both the stream and the final pass.
    assert sum(fake_model.chunk_sizes) == 69 * 350
    assert fake_model.final_samples == 69 * 350
    assert all(n >= daemon.VOICE_CHUNK_SAMPLES for n in fake_model.chunk_sizes[:-1])
    assert fake_model.kw == ("en", ["Claude"])


def test_damaged_stream_aborts_whistle_and_reports(fake_model):
    replies = []

    async def run():
        sess = _session(replies)
        sess._loop = asyncio.get_running_loop()
        _send_blocks(sess, 0, 30)
        sess._on_voice(None, bytearray([31, 0x01, 0]))   # gap: 29 -> 31
        await sess.drain_voice()
        await asyncio.sleep(0.05)
        await sess.drain_voice()

    asyncio.run(run())
    assert replies[-1]["t"] == "err"
    assert "dropped" in replies[-1]["m"]
    assert fake_model.final_samples is None   # no final pass on bad audio


def test_whistle_failure_falls_back_to_whisper(fake_model, monkeypatch):
    replies = []

    def boom(self, utt, language, keywords):
        raise RuntimeError("engine missing")

    monkeypatch.setattr(daemon.WhistleEngine, "_run", boom)
    monkeypatch.setattr(daemon, "whisper_transcribe", lambda pcm: "whisper saved it")

    async def run():
        sess = _session(replies)
        sess._loop = asyncio.get_running_loop()
        _send_blocks(sess, 0, 30)
        sess._on_voice(None, bytearray([30, 0x01, 0]))
        await sess.drain_voice()

    asyncio.run(run())
    assert replies[-1]["t"] == "vt"
    assert replies[-1]["tx"] == "whisper saved it"


def test_session_close_unblocks_worker(fake_model):
    async def run():
        sess = _session([])
        sess._loop = asyncio.get_running_loop()
        _send_blocks(sess, 0, 10)       # utterance open, no EOU (link drops)
        await sess.drain_voice()
        utt = sess._utt
        sess.close_voice()
        await asyncio.wait_for(asyncio.wrap_future(utt.future), timeout=2)
        return utt.future.result()

    assert asyncio.run(run()) is None


# ---- Payload shaping ---------------------------------------------------------
def test_device_text_folds_to_panel_glyphs():
    assert daemon.device_text("It\u2019s \u201cna\u00efve\u201d \u2014 ok\u2026") == \
        'It\'s "naive" - ok...'
    assert daemon.device_text("tabs\tand\nnewlines  ") == "tabs and newlines"
    assert all(" " <= c <= "~" for c in daemon.device_text("Krak\u00f3w \u2603 \u4e2d"))


def test_partial_payload_fits_panel_and_bridge():
    long_committed = " ".join(f"word{i}" for i in range(200))
    msg = daemon.partial_payload(long_committed, "and the pending tail")
    assert len(msg["c"]) + 1 + len(msg["p"]) <= daemon.PARTIAL_MAX_CHARS
    assert msg["c"].startswith("...")
    assert msg["c"].endswith("word199")           # newest speech kept
    assert msg["p"] == "and the pending tail"
    assert len(json.dumps(msg, separators=(",", ":"))) < 500


def test_tail_text_short_passthrough():
    assert daemon.tail_text("short", 50) == "short"


def test_read_voice_cfg_parses_engine_keywords_language(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("voice_engine=Whisper\n"
                   "voice_keywords = Claude, ClawdDeck ,  pytest\n"
                   "voice_language=DE\nvoice_inject=on\n")
    monkeypatch.setattr(daemon, "CONFIG_FILE", cfg)
    out = daemon.read_voice_cfg()
    assert out["voice_engine"] == "whisper"
    assert out["voice_keywords"] == ["Claude", "ClawdDeck", "pytest"]
    assert out["voice_language"] == "de"
    assert out["voice_inject"] is True


def test_read_voice_cfg_defaults(tmp_path, monkeypatch):
    monkeypatch.setattr(daemon, "CONFIG_FILE", tmp_path / "missing")
    out = daemon.read_voice_cfg()
    assert out["voice_engine"] == "auto"
    assert out["voice_keywords"] == list(daemon.DEFAULT_VOICE_KEYWORDS)
    assert out["voice_language"] is None


# ---- Live typing into the focused app --------------------------------------
class FakeInjector:
    """Stands in for inject_macos; records keystrokes instead of posting."""

    def __init__(self, trusted=True, front=100):
        self.trusted = trusted
        self.front = front
        self.typed = []
        self.returns = 0
        self.prompted = 0
        self.on_type = None

    def preflight(self, prompt=False):
        self.prompted += bool(prompt)
        return self.trusted

    def responsible_app_hint(self):
        return "/x/Python.app"

    def frontmost_pid(self):
        return self.front

    def type_text(self, text, safety=False):
        if not self.trusted:
            return False
        self.typed.append(text)
        if self.on_type:
            self.on_type(self)
        return True

    def press_return(self):
        self.returns += 1
        return self.trusted

    def type_prompt(self, text, submit=False):
        if not self.trusted:
            return False
        self.typed.append(text)
        self.returns += bool(submit)
        return True


@pytest.fixture
def injector(fake_model, monkeypatch):
    inj = FakeInjector()
    cfg = {"voice_engine": "whistle", "voice_keywords": ["Claude"],
           "voice_language": "en", "voice_inject": True, "voice_autosubmit": False}
    monkeypatch.setattr(daemon, "read_voice_cfg", lambda: dict(cfg))
    monkeypatch.setattr(daemon, "load_injector", lambda: inj)
    inj.cfg = cfg
    return inj


def _utterance(sess, start_seq, blocks_before, blocks_after, typed_probe=None):
    """Talk for blocks_before, let partials land, talk more, release."""
    async def run():
        sess._loop = asyncio.get_running_loop()
        _send_blocks(sess, start_seq, blocks_before)
        await sess.drain_voice()
        await _settle(sess)
        if typed_probe is not None:
            typed_probe()
        _send_blocks(sess, start_seq + blocks_before, blocks_after)
        end = start_seq + blocks_before + blocks_after
        sess._on_voice(None, bytearray([end & 0xFF, 0x01, 0]))
        await sess.drain_voice()
    return run()


def test_words_are_typed_while_still_talking(injector, fake_model):
    replies = []
    sess = _session(replies)
    mid = []
    asyncio.run(_utterance(sess, 0, 46, 46,
                           typed_probe=lambda: mid.append(list(injector.typed))))

    assert mid[0], "nothing was typed before release"
    typed = "".join(injector.typed)
    assert typed.startswith("word0")
    assert typed == " ".join(f"word{i}" for i in range(len(fake_model.chunk_sizes)))
    final = replies[-1]
    assert final["t"] == "vt" and final["inj"] == "ok"
    assert final["tx"] == typed                      # panel and Mac agree
    assert final["m"].startswith("Typed ") and "on your Mac" in final["m"]
    assert fake_model.final_samples is None          # no re-worded final pass
    assert injector.returns == 0                     # review-first default


def test_autosubmit_presses_enter_once_after_the_tail(injector):
    injector.cfg["voice_autosubmit"] = True
    replies = []
    asyncio.run(_utterance(_session(replies), 0, 46, 23))
    assert injector.returns == 1
    assert replies[-1]["m"].startswith("Sent ")


def test_focus_change_holds_words_and_reports(injector):
    replies = []

    def switch_away(inj):
        inj.front = 999                 # user clicked another app after word 1
        inj.on_type = None

    injector.on_type = switch_away
    asyncio.run(_utterance(_session(replies), 0, 46, 46))
    assert injector.typed == ["word0"]  # nothing sprayed into the other app
    assert replies[-1]["inj"] == "focus"
    assert "not typed" in replies[-1]["m"]


def test_focus_returning_before_release_flushes_held_words(injector):
    replies = []

    def away(inj):
        inj.front = 999
        inj.on_type = None

    injector.on_type = away
    sess = _session(replies)
    asyncio.run(_utterance(sess, 0, 46, 46,
                           typed_probe=lambda: setattr(injector, "front", 100)))
    typed = "".join(injector.typed)
    assert typed.split() == [f"word{i}" for i in range(len(typed.split()))]
    assert replies[-1]["inj"] == "ok"


def test_missing_accessibility_types_nothing_and_says_so(injector, fake_model,
                                                          monkeypatch):
    monkeypatch.setattr(daemon.os, "getppid", lambda: 1)   # running under launchd
    injector.trusted = False
    replies = []
    asyncio.run(_utterance(_session(replies), 0, 46, 23))
    assert injector.typed == []
    assert replies[-1]["inj"] == "perm"
    assert "Accessibility" in replies[-1]["m"]
    assert fake_model.final_samples is not None      # whole-clip pass still runs
    assert injector.prompted == 1                    # macOS dialog asked once


def test_gap_mid_dictation_stops_typing_and_warns(injector):
    replies = []

    async def run():
        sess = _session(replies)
        sess._loop = asyncio.get_running_loop()
        _send_blocks(sess, 0, 46)
        await sess.drain_voice()
        await _settle(sess)
        typed_before = len(injector.typed)
        _send_blocks(sess, 50, 20)      # 45 -> 50: lost packets
        sess._on_voice(None, bytearray([70, 0x01, 0]))
        await sess.drain_voice()
        await asyncio.sleep(0.05)
        return typed_before

    before = asyncio.run(run())
    assert len(injector.typed) == before            # nothing typed past the gap
    assert replies[-1] == {"t": "err", "m": "Audio dropped - check the typed text"}


def test_follow_up_utterance_continues_with_a_space(injector):
    replies = []
    sess = _session(replies)
    asyncio.run(_utterance(sess, 0, 46, 0))
    first = len(injector.typed)
    asyncio.run(_utterance(sess, 0, 46, 0))
    assert injector.typed[first].startswith(" word0")


def test_inject_status_messages_fit_the_panel():
    for inj in ("ok", "none", "focus", "perm", "partial", "unavailable"):
        msg = daemon.inject_status_message(inj, 12, False)
        assert msg and len(msg) <= 42 and msg.isascii()
    assert daemon.inject_status_message("ok", 1, True) == "Sent 1 word on your Mac"
    assert daemon.inject_status_message("off", 3, False) == ""


# ---- Single instance -----------------------------------------------------------
def test_second_daemon_instance_is_refused(tmp_path):
    lock = tmp_path / "daemon.lock"
    first = daemon.acquire_single_instance(lock, wait=False)
    assert first
    assert daemon.acquire_single_instance(lock, wait=False) is None
    first.close()
    again = daemon.acquire_single_instance(lock, wait=False)
    assert again
    again.close()


def test_terminal_run_copy_never_pops_the_dialog(injector, monkeypatch):
    """A daemon started from a terminal is judged by the terminal's grant;
    prompting there would steer the user to the wrong app."""
    monkeypatch.setattr(daemon.os, "getppid", lambda: 4242)
    injector.trusted = False
    asyncio.run(_utterance(_session([]), 0, 46, 0))
    assert injector.prompted == 0
