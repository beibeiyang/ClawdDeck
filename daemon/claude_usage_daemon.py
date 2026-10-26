#!/usr/bin/env python3
"""Claude Usage Tracker Daemon (BLE) — macOS port of claude-usage-daemon.sh.

Polls Claude API rate-limit headers and writes a JSON payload to the
ESP32 "Clawdmeter" peripheral over a custom GATT service. Uses
bleak (CoreBluetooth backend on macOS).
"""

import asyncio
import calendar
import concurrent.futures
import datetime
import importlib.util
import getpass
import json
import os
import queue
import re
import shutil
import signal
import subprocess
import sys
import time
import unicodedata
from pathlib import Path

import httpx
from bleak import BleakClient
from bleak.exc import BleakError

# Voice dictation: ADPCM decode + speech-to-text (all optional at import —
# the daemon still runs rate-limit duty without them). Whistle
# (cactus-needle) is preferred because it streams partial transcripts while
# you talk; faster-whisper is the whole-clip fallback.
try:
    import numpy as np
except ImportError:
    np = None
try:
    from faster_whisper import WhisperModel
except ImportError:
    WhisperModel = None
# cactus-needle phones home with anonymous usage counts unless told not to.
os.environ.setdefault("NEEDLE_TELEMETRY", "0")

from claude_sessions import (
    find_session_cwd,
    list_recent_sessions,
    read_resume_terminal_setting,
    resume_session_mac,
    resume_terminal_label,
    sessions_payload,
)

DEVICE_NAME = "ClawdDeck"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"
HOST_RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000005"
DEV_CMD_CHAR_UUID = "4c41555a-4465-7669-6365-000000000006"
VOICE_CHAR_UUID = "4c41555a-4465-7669-6365-000000000007"

POLL_INTERVAL = 60
TICK = 5
CONNECT_TIMEOUT = 20.0

# macOS: token lives in Keychain (service "Claude Code-credentials").
# Linux: token lives in ~/.claude/.credentials.json.
KEYCHAIN_SERVICE = "Claude Code-credentials"
DEFAULT_CONFIG_DIR = Path.home() / ".claude"
SAVED_ADDR_FILE = Path.home() / ".config" / "claude-usage-monitor" / "ble-address"
CONFIG_FILE = Path.home() / ".config" / "claude-usage-monitor" / "config"

API_URL = "https://api.anthropic.com/v1/messages"
API_HEADERS_TEMPLATE = {
    "anthropic-version": "2023-06-01",
    "anthropic-beta": "oauth-2025-04-20",
    "Content-Type": "application/json",
    "User-Agent": "claude-code/2.1.5",
}
API_BODY = {
    "model": "claude-haiku-4-5-20251001",
    "max_tokens": 1,
    "messages": [{"role": "user", "content": "hi"}],
}


class TokenExpired(Exception):
    """Raised by poll_api on a 401/403 — the access token is dead. The daemon never
    refreshes (pure free-ride: Claude Code owns refreshing), so the caller just
    signals "No data" to the device until the CLI re-seeds the token."""


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def _extract_access_token(blob: str) -> str | None:
    """Pull the accessToken out of a credentials blob.

    Claude Code stores credentials as a JSON object; the blob may also be
    nested ({"claudeAiOauth": {"accessToken": "..."}}). Fall back to a
    regex match so unexpected shapes still work, and finally treat the
    blob as a raw token if nothing else matches.
    """
    blob = blob.strip()
    if not blob:
        return None
    try:
        data = json.loads(blob)
    except json.JSONDecodeError:
        data = None
    if isinstance(data, dict):
        # direct: {"accessToken": "..."}
        tok = data.get("accessToken")
        if isinstance(tok, str) and tok.strip():
            return tok
        # nested: {"claudeAiOauth": {"accessToken": "..."}}
        for v in data.values():
            if isinstance(v, dict):
                tok = v.get("accessToken")
                if isinstance(tok, str) and tok.strip():
                    return tok
    m = re.search(r'"accessToken"\s*:\s*"([^"]+)"', blob)
    if m:
        return m.group(1)
    # Raw token (no JSON wrapper) — must look plausible (sk-ant-... etc.)
    if re.fullmatch(r"[A-Za-z0-9_\-.~+/=]{20,}", blob):
        return blob
    return None


def _decode_keychain_blob(raw: str) -> str:
    """Transparently decode a hex-dumped Keychain secret back to text.

    ``security … -w`` prints the password as a continuous hex string whenever
    the stored bytes aren't cleanly printable (e.g. an embedded newline). A
    normal credentials blob is JSON, which is never valid hex (it contains
    '{', '"', …), so all-hex detection is unambiguous and safe.
    """
    s = raw.strip()
    if s and len(s) % 2 == 0 and re.fullmatch(r"[0-9a-fA-F]+", s):
        try:
            return bytes.fromhex(s).decode("utf-8")
        except (ValueError, UnicodeDecodeError):
            return raw
    return raw


def _read_token_keychain() -> str | None:
    """Read the OAuth access token from the macOS Keychain, or None.

    ``security … -w`` may hex-dump the stored secret (see _decode_keychain_blob),
    so decode before extracting the access token.
    """
    try:
        out = subprocess.run(
            [
                "security",
                "find-generic-password",
                "-s",
                KEYCHAIN_SERVICE,
                "-a",
                getpass.getuser(),
                "-w",
            ],
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except subprocess.CalledProcessError as e:
        log(f"Keychain read failed (rc={e.returncode}): {e.stderr.strip()}")
        return None
    except (FileNotFoundError, subprocess.TimeoutExpired) as e:
        log(f"Keychain access error: {e}")
        return None
    return _extract_access_token(_decode_keychain_blob(out.stdout))


def read_voice_cfg() -> dict:
    """Voice-dictation settings from the daemon config file.

    Keys (booleans, default off/off):
      voice_inject=on      type the transcript into the focused window
      voice_autosubmit=on  press Enter after typing (per-agent Enter keys
      are a follow-up; Enter is correct for shells + Claude Code and the
      default stays review-before-send)

    Strings:
      voice_engine=auto|whistle|whisper   auto = Whistle when installed
      voice_keywords=Claude, Clawdmeter   comma list biased during decoding
                                          (Whistle only)
      voice_language=en                   force one of en de fr es it nl pl;
                                          empty = detect (Whistle only)
    """
    cfg: dict = {"voice_inject": False, "voice_autosubmit": False,
                 "voice_engine": "auto",
                 "voice_keywords": list(DEFAULT_VOICE_KEYWORDS),
                 "voice_language": None}
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                k, v = key.strip().lower(), val.strip()
                if k in ("voice_inject", "voice_autosubmit"):
                    cfg[k] = v.lower() in ("1", "on", "true", "yes")
                elif k == "voice_engine" and v.lower() in ("auto", "whistle", "whisper"):
                    cfg[k] = v.lower()
                elif k == "voice_keywords":
                    cfg[k] = [w.strip() for w in v.split(",") if w.strip()]
                elif k == "voice_language":
                    cfg[k] = v.lower() or None
    except OSError:
        pass
    return cfg


def read_config_dirs() -> list[Path]:
    """Claude config dirs to poll, from the `config_dirs` option (comma list).

    Defaults to [~/.claude] so existing single-plan setups are unchanged. ~ is
    expanded. Mirrors the Linux bash daemon's read_config_dirs.
    """
    raw = ""
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip().lower() == "config_dirs":
                    raw = val.strip()
    except OSError:
        pass
    if not raw:
        return [DEFAULT_CONFIG_DIR]
    dirs = [Path(p.strip()).expanduser() for p in raw.split(",") if p.strip()]
    return dirs or [DEFAULT_CONFIG_DIR]


def read_token_for(config_dir: Path) -> str | None:
    """Read the OAuth token for one config dir.

    Linux: each dir keeps its own ``<dir>/.credentials.json``. macOS: the default
    install stores the token in Keychain with no file, so for the default dir we
    fall back to Keychain when no file is present — preserving existing
    single-plan macOS behavior. Additional macOS dirs are read from their files;
    a work plan whose token lives only in the single Keychain entry can't be told
    apart there (documented follow-up).
    """
    cred = config_dir / ".credentials.json"
    try:
        if cred.exists():
            return _extract_access_token(cred.read_text())
    except OSError as e:
        log(f"Error reading credentials in {config_dir}: {e}")
    if sys.platform == "darwin" and config_dir == DEFAULT_CONFIG_DIR:
        return _read_token_keychain()
    return None


def load_cached_address() -> str | None:
    if not SAVED_ADDR_FILE.exists():
        return None
    addr = SAVED_ADDR_FILE.read_text().strip()
    # Accept both Linux MAC (AA:BB:CC:DD:EE:FF) and macOS CoreBluetooth UUID
    # (E621E1F8-C36C-495A-93FC-0C247A3E6E5F).
    if re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", addr) or re.fullmatch(
        r"[0-9A-Fa-f]{8}-(?:[0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}", addr
    ):
        return addr
    log("Cached address malformed, discarding")
    SAVED_ADDR_FILE.unlink(missing_ok=True)
    return None


# --- macOS: recover a device the OS already holds as an HID keyboard --------
#
# The firmware advertises as a BLE HID keyboard so its buttons type into the
# Mac. macOS auto-connects to that HID, and CoreBluetooth then EXCLUDES the
# peripheral from BleakScanner.discover() results (already-connected devices
# never appear in scans). bleak's connect-by-address path also scans
# internally, so a cached address can't help either. The documented escape
# hatch is retrieveConnectedPeripheralsWithServices_, which returns
# peripherals the system is already connected to. We wrap the result in a
# BLEDevice carrying the live (peripheral, manager) details so BleakClient
# connects to it directly without scanning. CoreBluetooth shares the single
# physical link, so this rides the existing HID connection — the keyboard
# keeps working.
_cb_manager = None  # reused CentralManagerDelegate (CoreBluetooth)


async def _get_cb_manager():
    """Lazily create and ready a shared CoreBluetooth central manager."""
    global _cb_manager
    if _cb_manager is None:
        from bleak.backends.corebluetooth.CentralManagerDelegate import (
            CentralManagerDelegate,
        )

        mgr = CentralManagerDelegate()
        await mgr.wait_until_ready()  # raises if Bluetooth is unauthorized/off
        _cb_manager = mgr
    return _cb_manager


async def retrieve_connected_macos(skip_addr: str | None = None):
    """Return a BLEDevice for a system-connected 'Clawdmeter', or None.

    Two-step lookup, strongest signal first:

    1. Peripherals connected under our CUSTOM service UUID. Membership in
       that service is unambiguous (no other device exposes it), so we accept
       by service alone — the peripheral's name can be None on macOS.
    2. Fall back to the generic HID service 0x1812, but ONLY trust a
       peripheral whose name matches DEVICE_NAME. 0x1812 also matches
       unrelated keyboards/mice, so picking blindly here could grab the
       wrong device.

    ``skip_addr`` skips a peripheral whose UUID just failed to connect, so a
    stale CoreBluetooth handle can't trap us into never trying a fresh scan.
    """
    from CoreBluetooth import CBUUID
    from bleak.backends.device import BLEDevice

    try:
        manager = await _get_cb_manager()
    except Exception as e:  # BleakBluetoothNotAvailableError etc.
        log(f"CoreBluetooth unavailable: {e}")
        return None

    cm = manager.central_manager

    def _wrap(p):
        addr = p.identifier().UUIDString()
        log(f"Found system-connected peripheral: {p.name()!r} [{addr}]")
        return BLEDevice(addr, p.name(), (p, manager))

    def _ok(p) -> bool:
        return not (skip_addr and p.identifier().UUIDString() == skip_addr)

    # 1. Custom service — accept by service membership alone.
    custom = cm.retrieveConnectedPeripheralsWithServices_(
        [CBUUID.UUIDWithString_(SERVICE_UUID)]
    )
    for p in custom or []:
        if _ok(p):
            return _wrap(p)

    # 2. Generic HID service — require an exact name match.
    hid = cm.retrieveConnectedPeripheralsWithServices_(
        [CBUUID.UUIDWithString_("1812")]
    )
    for p in hid or []:
        if _ok(p) and p.name() == DEVICE_NAME:
            return _wrap(p)

    return None


async def discover_target(skip_addr: str | None = None):
    """Return a connectable target, or None.

    The daemon only ever targets the device this system already holds — it
    never scans for a nearby device by name, so it can't grab a stranger's or
    the wrong nearby unit. On macOS that's the system-connected peripheral (the
    firmware advertises as an HID keyboard, so once paired the OS auto-connects
    and holds it — HID-grabbed devices are invisible to scans anyway). On other
    platforms it's a previously-pinned address in the cache file. If the device
    isn't held/pinned, we log and wait rather than scanning. ``skip_addr`` skips
    a peripheral whose handle just failed to connect.
    """
    if sys.platform == "darwin":
        dev = await retrieve_connected_macos(skip_addr=skip_addr)
        if dev is None:
            log("Device not held by OS; waiting (not scanning by name)")
        return dev

    address = load_cached_address()
    if not address:
        log("No pinned address cached; waiting (not scanning by name)")
    return address


def read_chime_setting() -> str:
    """Read the `chime` option from the config file. One of: off|on.

    Defaults to "off" (the device stays silent) so existing setups are
    unaffected until the user opts in.
    """
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip().lower() == "chime":
                    val = val.strip().lower()
                    if val in ("off", "on"):
                        return val
    except OSError:
        pass
    return "off"


def read_clock_setting() -> str:
    """Read the `clock` option from the config file. One of: off|auto|12|24.

    Defaults to "off" (no clock; the device keeps showing "Usage") so existing
    setups are unaffected until the user opts in.
    """
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip().lower() == "clock":
                    val = val.strip().lower()
                    if val in ("off", "auto", "12", "24"):
                        return val
    except OSError:
        pass
    return "off"


def add_chime_field(payload: dict) -> None:
    """Add "c":1 to the payload when the config opts in, so the firmware may
    sound the session-reset chime. Omitted entirely when chime is off."""
    if read_chime_setting() == "on":
        payload["c"] = 1


def detect_hour_format() -> int:
    """Best-effort 12h/24h detection for the host. Returns 12 or 24 (default 24)."""
    # macOS: the explicit System Settings toggle lives in NSGlobalDomain.
    for key, result in (("AppleICUForce24HourTime", 24), ("AppleICUForce12HourTime", 12)):
        try:
            out = subprocess.run(["defaults", "read", "-g", key],
                                 capture_output=True, text=True, timeout=3)
            if out.stdout.strip() == "1":
                return result
        except (OSError, subprocess.SubprocessError):
            pass
    # Fallback to the C locale's time format (may be C/24h under launchd).
    try:
        import locale
        locale.setlocale(locale.LC_TIME, "")
        fmt = locale.nl_langinfo(locale.T_FMT)
        if "%p" in fmt or "%r" in fmt or "%I" in fmt:
            return 12
    except (ImportError, locale.Error, AttributeError):
        pass
    return 24


def add_clock_fields(payload: dict) -> None:
    """Add wall-clock fields on every payload so the status bar and Clock app
    stay synced. The legacy `clock` config key only selects 12h vs 24h now;
    "off" no longer suppresses time (that left the launcher stuck at --:--)."""
    clock = read_clock_setting()
    if clock == "24":
        tf = 24
    elif clock == "12":
        tf = 12
    else:
        tf = detect_hour_format()
    payload["t"] = int(time.time()) + time.localtime().tm_gmtoff
    payload["tf"] = tf


async def poll_api(token: str) -> dict | None:
    headers = dict(API_HEADERS_TEMPLATE)
    headers["Authorization"] = f"Bearer {token}"
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.post(API_URL, headers=headers, json=API_BODY)
    except httpx.HTTPError as e:
        log(f"API call failed: {e}")
        return None
    if resp.status_code in (401, 403):
        log(f"API HTTP {resp.status_code} (token expired/invalid)")
        raise TokenExpired()
    if resp.status_code >= 400:
        log(f"API HTTP {resp.status_code}: {resp.text[:200]}")
        return None

    def hdr(name: str, default: str = "0") -> str:
        return resp.headers.get(name, default)

    now = time.time()

    def reset_minutes(reset_ts: str) -> int:
        try:
            r = float(reset_ts)
        except ValueError:
            return 0
        mins = (r - now) / 60.0
        return int(round(mins)) if mins > 0 else 0

    def pct(util: str) -> int:
        try:
            return int(round(float(util) * 100))
        except ValueError:
            return 0

    # Pro/Max accounts expose 5h/7d windows; Enterprise/overage use a single
    # spending-limit model reported via overage-utilization.
    if resp.headers.get("anthropic-ratelimit-unified-5h-utilization"):
        payload = {
            "s": pct(hdr("anthropic-ratelimit-unified-5h-utilization")),
            "sr": reset_minutes(hdr("anthropic-ratelimit-unified-5h-reset")),
            "w": pct(hdr("anthropic-ratelimit-unified-7d-utilization")),
            "wr": reset_minutes(hdr("anthropic-ratelimit-unified-7d-reset")),
            "st": hdr("anthropic-ratelimit-unified-5h-status", "unknown"),
            "acct": "pro",
            "ok": True,
        }
    else:
        reset_ts = hdr("anthropic-ratelimit-unified-overage-reset")
        payload = {
            "s": pct(hdr("anthropic-ratelimit-unified-overage-utilization")),
            "sr": reset_minutes(reset_ts),
            "w": 0,
            "wr": 0,
            "st": hdr("anthropic-ratelimit-unified-status", "unknown"),
            "acct": "ent",
            **_billing_period_info(now, reset_ts),
            "ok": True,
        }
    add_chime_field(payload)   # adds "c":1 iff the config opts in
    add_clock_fields(payload)   # adds "t" + "tf" on every payload
    return payload


def _billing_period_info(now: float, reset_ts: str) -> dict:
    """Fraction of billing period elapsed (tp, 0-100) and period length in days (pd).

    Billing periods are assumed calendar-monthly: period_end is the reset
    timestamp, period_start is the same day/time one calendar month earlier.

    The rate-limit headers expose only the reset timestamp, not the period
    length, so the monthly window is an assumption — but a documented one:
    Enterprise spend-limit `period` "the only value today is monthly"
    (Claude Enterprise Admin API reference). The doc notes period is an open
    string that may gain other values later; revisit this if so.
    """
    try:
        period_end = float(reset_ts)
    except ValueError:
        return {"tp": 0, "pd": 30}
    if period_end <= 0:
        # reset_ts defaults to "0" when the overage-reset header is absent.
        # fromtimestamp(0) is 1970; stepping a month back lands in 1969, and
        # datetime.timestamp() raises OSError for pre-1970 dates on Windows.
        # Benign on macOS/Linux, but guard here too to keep the daemons parallel.
        return {"tp": 0, "pd": 30}
    dt_end = datetime.datetime.fromtimestamp(period_end)
    prev_month = dt_end.month - 1 or 12
    prev_year = dt_end.year if dt_end.month > 1 else dt_end.year - 1
    prev_day = min(dt_end.day, calendar.monthrange(prev_year, prev_month)[1])
    dt_start = dt_end.replace(year=prev_year, month=prev_month, day=prev_day)
    period_start = dt_start.timestamp()
    period_len = period_end - period_start
    if period_len <= 0:
        return {"tp": 0, "pd": 30}
    pct_val = (now - period_start) / period_len * 100
    total_days = int(round(period_len / 86400))
    rd = f"{dt_end.strftime('%b')} {dt_end.day}"
    return {
        "tp": max(0, min(100, int(round(pct_val)))),
        "pd": total_days,
        "rd": rd,
    }


class PlanSelector:
    """Decide which config dir's plan is "active" across polls.

    "Active" = the plan whose session % rose most recently (recent API activity).
    A rise stamps a monotonic poll counter, so the choice is sticky and a window
    reset (a drop to 0) isn't mistaken for use. Before any rise is seen (startup)
    the highest current session % wins. Mirrors the Linux bash daemon.
    """

    def __init__(self) -> None:
        self.prev_s: dict[Path, int] = {}
        self.last_active: dict[Path, int] = {}
        self.seq = 0

    def choose(self, sessions: dict[Path, int]) -> Path:
        """Update state from this cycle's {dir: session_pct} and return the active dir."""
        self.seq += 1
        for d, s in sessions.items():
            if d in self.prev_s and s > self.prev_s[d]:
                self.last_active[d] = self.seq
            self.prev_s[d] = s
        # Most recent activity wins; ties (and the startup case) break by highest %.
        return max(sessions, key=lambda d: (self.last_active.get(d, 0), sessions[d]))


# Module-level so the active-plan state survives reconnects.
_SELECTOR = PlanSelector()


async def poll_active(selector: PlanSelector = _SELECTOR) -> tuple[dict | None, bool]:
    """Poll every configured config dir; return ``(active_payload, all_dead)``.

    ``active_payload`` — the active plan's payload dict, or None when no dir
    yields a usable payload this cycle. A single configured dir (the default)
    collapses to exactly the old single-poll path.

    ``all_dead`` — True when *every* configured dir lacked a usable token this
    cycle (file/Keychain empty, or a 401/expired token), so the caller can
    signal "No data". False when at least one token authenticated — including a
    transient non-auth poll failure worth retrying silently rather than idling.

    Pure free-ride: a 401 (TokenExpired) means that dir's token has expired and
    only Claude Code (its owner) can re-seed it — we never refresh it ourselves.
    """
    dirs = read_config_dirs()
    payloads: dict[Path, dict] = {}
    sessions: dict[Path, int] = {}
    any_live = False
    for d in dirs:
        token = read_token_for(d)
        if not token:
            log(f"No token in {d}; skipping")
            continue
        try:
            payload = await poll_api(token)
        except TokenExpired:
            log(f"Token in {d} expired/invalid; skipping")
            continue
        # Authenticated: a transient None here isn't an auth failure, so the
        # dir counts as live and we stay silent rather than idling the device.
        any_live = True
        if payload is not None:
            payloads[d] = payload
            sessions[d] = int(payload.get("s", 0) or 0)
    if not payloads:
        return None, not any_live
    active = selector.choose(sessions)
    if len(dirs) > 1:
        log(f"Active plan: {active} (s={sessions[active]})")
    return payloads[active], False


async def poll_active_payload(selector: PlanSelector = _SELECTOR) -> dict | None:
    """The active plan's payload, or None when no dir yields one this cycle.

    Thin wrapper over :func:`poll_active` for callers that don't need the
    all-dead flag.
    """
    payload, _dead = await poll_active(selector)
    return payload


# ---- Voice dictation helpers -----------------------------------------------
SAMPLE_RATE_VOICE = 16000

_IMA_STEP_TABLE = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
    598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1410, 1552, 1707,
    1878, 2066, 2272, 2499, 2749, 3024,
] + [32767] * 25   # indexes 64..88 saturate (spec table = 89 entries)
_IMA_ADJ = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]


def ima_adpcm_decode(blocks: "bytes | list[bytes]", rate: int) -> "np.ndarray | None":
    """Decode self-contained IMA-ADPCM blocks to int16 mono PCM.

    New callers pass one item per BLE notification, preserving each block's
    real length so firmware can size packets to the negotiated MTU. A flat
    byte string remains supported for old recordings/tests and is split using
    the original 92-byte block stride.
    """
    if np is None:
        return None
    del rate  # The codec is rate-independent; rate matters at transcription.
    if isinstance(blocks, (bytes, bytearray)):
        legacy = bytes(blocks)
        block_list = [legacy[p:p + 92] for p in range(0, len(legacy), 92)]
    else:
        block_list = blocks
    sample_capacity = sum(max(0, len(block) - 4) * 2 for block in block_list)
    if sample_capacity == 0:
        return None
    out = np.empty(sample_capacity, dtype=np.int16)
    n_out = 0
    for block in block_list:
        if len(block) < 4:
            continue
        raw = block[0] | (block[1] << 8)                # uint16 LE
        pred = raw - 65536 if raw >= 0x8000 else raw    # int16 wrap (numpy 2 strictness)
        step_idx = min(block[2], 88)
        for b in block[4:]:
            for nib in (b & 0x0F, (b >> 4) & 0x0F):
                step = _IMA_STEP_TABLE[step_idx]
                d = step >> 3
                if nib & 4:
                    d += step
                if nib & 2:
                    d += step >> 1
                if nib & 1:
                    d += step >> 2
                if nib & 8:
                    d = -d
                pred += d
                if pred > 32767:
                    pred = 32767
                elif pred < -32768:
                    pred = -32768
                out[n_out] = pred
                n_out += 1
                step_idx = max(0, min(88, step_idx + _IMA_ADJ[nib]))
    return out[:n_out] if n_out else None


def whisper_transcribe(pcm: "np.ndarray") -> "str | None":
    """faster-whisper tiny.en on int16 mono 16k; returns text or None."""
    if WhisperModel is None:
        log("Voice: faster-whisper not installed (pip install faster-whisper)")
        return None
    global _whisper_model
    try:
        if _whisper_model is None:
            log("Voice: loading faster-whisper tiny.en (first use)")
            _whisper_model = WhisperModel("tiny.en", device="cpu", compute_type="int8")
        segments, _info = _whisper_model.transcribe(
            pcm.astype("float32") / 32768.0, language="en", beam_size=1)
        text = " ".join(seg.text for seg in segments).strip()
        return text or None
    except Exception as e:
        log(f"Voice: whisper failed: {e}")
        return None


_whisper_model = None


# ---- Whistle live transcription ---------------------------------------------
# The board already streams ADPCM in real time; Whistle re-transcribes the
# growing buffer every VOICE_CHUNK_SAMPLES and commits the words two passes
# agree on, so committed text never changes under the reader. Each pass costs
# tens of ms on Apple silicon, so text trails speech by ~one chunk.
DEFAULT_VOICE_KEYWORDS = ("Claude", "Claude Code", "Clawdmeter")
VOICE_CHUNK_SAMPLES = SAMPLE_RATE_VOICE // 2   # 0.5 s per streaming pass
VOICE_STALL_TIMEOUT = 30.0   # worker gives up on an utterance that stops feeding
VOICE_FINAL_TIMEOUT = 15.0
PARTIAL_MIN_INTERVAL = 0.25  # cap 'vp' bridge writes at 4 Hz
PARTIAL_MAX_CHARS = 160      # committed tail + pending; panel shows ~3 lines
FINAL_MAX_CHARS = 400        # device label buffer is 480 bytes

_ASCII_MAP = str.maketrans({
    "\u2018": "'", "\u2019": "'", "\u201a": "'", "\u201b": "'",
    "\u201c": '"', "\u201d": '"', "\u201e": '"',
    "\u2013": "-", "\u2014": "-", "\u2026": "...", "\u00a0": " ",
})


def device_text(text: str) -> str:
    """Fold a transcript to the panel's glyph set (fonts are ASCII 0x20-0x7E).

    Accents are stripped (café -> cafe) rather than dropped as missing glyphs.
    Only the copy sent to the panel is folded; injection keeps the original.
    """
    text = unicodedata.normalize("NFKD", text.translate(_ASCII_MAP))
    text = re.sub(r"\s+", " ", text)   # before the filter, or \t/\n glue words
    return "".join(c for c in text if " " <= c <= "~").strip()


def tail_text(text: str, limit: int) -> str:
    """The last <= limit chars of text, cut at a word boundary, '...'-prefixed."""
    if len(text) <= limit:
        return text
    cut = text[len(text) - (limit - 3):]
    space = cut.find(" ")
    if 0 <= space < 24:
        cut = cut[space + 1:]
    return "..." + cut


def partial_payload(committed: str, pending: str) -> dict:
    """Bridge message for a live transcript: committed text + unconfirmed tail.

    Older committed words are trimmed from the left so the newest speech always
    fits the panel's transcript box.
    """
    pending = tail_text(device_text(pending), PARTIAL_MAX_CHARS // 2)
    committed = device_text(committed)
    room = PARTIAL_MAX_CHARS - len(pending) - (1 if pending else 0)
    return {"t": "vp", "c": tail_text(committed, max(room, 0)) if room > 0 else "",
            "p": pending}


def resolve_voice_engine(choice: str) -> str:
    """'whistle' or 'whisper' for the configured choice and installed packages."""
    has_whistle = importlib.util.find_spec("needle") is not None
    if choice == "whistle" or (choice == "auto" and has_whistle):
        return "whistle" if has_whistle else "whisper"
    return "whisper"


class WhistleUtterance:
    """One live-transcribed utterance. feed() from the event loop; the stream
    itself runs on the engine's single worker thread."""

    def __init__(self, on_partial) -> None:
        self._q: "queue.Queue[np.ndarray | None]" = queue.Queue()
        self._block: list = []
        self._held = 0
        self.audio: list = []      # every sample, for the final whole-clip pass
        self.samples = 0
        self.aborted = False
        self.final_pass = True
        self.on_partial = on_partial
        self.future: "concurrent.futures.Future | None" = None

    def feed(self, pcm16: "np.ndarray") -> None:
        samples = pcm16.astype(np.float32) / 32768.0
        self.audio.append(samples)
        self.samples += len(samples)
        self._block.append(samples)
        self._held += len(samples)
        if self._held >= VOICE_CHUNK_SAMPLES:
            self._q.put(np.concatenate(self._block))
            self._block, self._held = [], 0

    def close(self) -> None:
        if self._block:
            self._q.put(np.concatenate(self._block))
            self._block, self._held = [], 0
        self._q.put(None)

    def abort(self) -> None:
        self.aborted = True
        self._q.put(None)

    def chunks(self):
        while True:
            try:
                chunk = self._q.get(timeout=VOICE_STALL_TIMEOUT)
            except queue.Empty:   # link died without an EOU or abort
                self.aborted = True
                return
            if chunk is None:
                return
            yield chunk


class WhistleEngine:
    """Owns the Whistle model on ONE thread: the engine is not thread-safe and
    holds one model per process, so every call (load, stream, transcribe) goes
    through the same single-worker executor."""

    def __init__(self) -> None:
        self._pool = concurrent.futures.ThreadPoolExecutor(
            max_workers=1, thread_name_prefix="whistle")
        self._model = None

    def _load(self):
        if self._model is None:
            import needle   # lazy: keeps daemon start-up free of the engine
            log("Voice: loading Whistle (first use downloads ~17 MB)")
            started = time.time()
            self._model = needle.Whistle()
            log(f"Voice: Whistle ready in {time.time() - started:.1f}s")
        return self._model

    def warm_up(self) -> None:
        self._pool.submit(self._load).add_done_callback(_log_future_error)

    def start(self, on_partial, language, keywords,
              final_pass: bool = True) -> WhistleUtterance:
        utt = WhistleUtterance(on_partial)
        utt.final_pass = final_pass
        utt.future = self._pool.submit(self._run, utt, language, keywords)
        return utt

    def _run(self, utt: WhistleUtterance, language, keywords) -> "tuple[str, str] | None":
        """(final, committed) for the utterance, or None when aborted/empty."""
        model = self._load()
        committed = ""
        for step in model.stream(utt.chunks(), language=language, keywords=keywords):
            if utt.aborted:
                continue          # let the generator finish so the engine resets
            if step.get("text"):
                committed = f"{committed} {step['text']}".strip()
            utt.on_partial(committed, step.get("pending", ""))
        if utt.aborted or not utt.samples:
            return None
        if not utt.final_pass:
            # Live typing already put the committed words on screen; a
            # whole-clip pass could word things differently and leave the
            # panel and the typed prompt disagreeing.
            return committed, committed
        # A whole-clip pass sees the entire utterance at once (the encoder is
        # bidirectional) and costs ~10 ms per second of audio, so the final
        # text gets full context instead of the streaming commits.
        final = model.transcribe(np.concatenate(utt.audio), language=language,
                                 keywords=keywords).get("text", "").strip()
        return (final or committed), committed


def _log_future_error(fut: "concurrent.futures.Future") -> None:
    if not fut.cancelled() and fut.exception() is not None:
        log(f"Voice: Whistle failed: {fut.exception()}")


_whistle_engine: "WhistleEngine | None" = None


def whistle_engine() -> WhistleEngine:
    global _whistle_engine
    if _whistle_engine is None:
        _whistle_engine = WhistleEngine()
    return _whistle_engine


# ---- Live typing (dictation straight into the focused app) -----------------
_ax_prompted = False   # ask macOS for the Accessibility dialog once per process


def load_injector():
    """inject_macos, or None off macOS / without pyobjc."""
    if sys.platform != "darwin":
        return None
    try:
        import inject_macos
        return inject_macos
    except ImportError:
        return None


def accessibility_ok(injector, prompt: bool = False) -> bool:
    """Check (and, at most once per process, request) the Accessibility grant.

    Logs which app needs the grant: under launchd it is Python.app, but a
    daemon started from a terminal inherits the terminal's grant instead.
    """
    global _ax_prompted
    # Only the launchd copy asks: a terminal-run copy would pop the dialog
    # for the terminal app, which is the wrong grant for the service.
    ask = prompt and not _ax_prompted and os.getppid() == 1
    _ax_prompted = _ax_prompted or ask
    if injector.preflight(prompt=ask):
        return True
    log("Voice: Accessibility permission missing — typing disabled. Grant it to "
        f"{injector.responsible_app_hint()} in System Settings > Privacy & "
        "Security > Accessibility, then restart the daemon.")
    return False


class LiveTyper:
    """Types committed words into the focused app while the user talks.

    Committed words never change, so they can go out as soon as Whistle
    commits them. Keystrokes run on one dedicated thread (order kept, BLE loop
    never blocked). Edge cases:
      - focus moves to another app mid-utterance: typing pauses and words are
        held; they are typed if focus returns before release, else dropped
        (status 'focus') — never sprayed into the wrong window
      - Accessibility revoked mid-utterance: stop, status 'perm'
      - damaged audio: stop typing at the damage, status 'partial'
      - a second utterance into the same app (no Enter between) gets a
        leading space so sentences don't glue together
    """

    def __init__(self, injector, prev_target: "int | None") -> None:
        self._inj = injector
        self._pool = concurrent.futures.ThreadPoolExecutor(
            max_workers=1, thread_name_prefix="typer")
        self._prev_target = prev_target
        self._consumed = 0          # chars of the committed string handed out
        self._held = ""             # words waiting for focus to come back
        self.target: "int | None" = None
        self.typed_any = False
        self.pushed_any = False
        self.perm_lost = False
        self.stopped = False

    def push(self, committed: str) -> None:
        """Queue whatever was committed since the last push (event loop)."""
        if self.stopped or len(committed) <= self._consumed:
            return
        delta = committed[self._consumed:]
        self._consumed = len(committed)
        if delta.strip():
            self.pushed_any = True
            self._pool.submit(self._type, delta).add_done_callback(_log_typer_error)

    def stop(self) -> None:
        self.stopped = True

    def cancel(self) -> None:
        """Abandon the utterance: type nothing further, free the thread."""
        self.stopped = True
        self._pool.shutdown(wait=False, cancel_futures=True)

    def _type(self, delta: str) -> None:
        if self.stopped or self.perm_lost:
            return
        pid = self._inj.frontmost_pid()
        if self.target is None:
            self.target = pid
        text = self._held + delta
        if pid is not None and pid != self.target:
            self._held = text          # wrong window: hold, don't type
            return
        self._held = ""
        safety = False
        if not self.typed_any:
            text = text.lstrip()
            if self._prev_target is not None and self._prev_target == self.target:
                text = " " + text      # continuing an unsent prompt
            safety = True              # misfired Enter submits " x", not words
        if not self._inj.type_text(text, safety=safety):
            self.perm_lost = True
            return
        self.typed_any = True

    async def finish(self, committed: str, submit: bool) -> str:
        """Type the tail, optionally press Enter; returns the outcome:
        ok | none | focus | perm | partial."""
        self.push(committed)
        fut = self._pool.submit(self._finish, submit)
        try:
            return await asyncio.wrap_future(fut)
        finally:
            self._pool.shutdown(wait=False)

    def _finish(self, submit: bool) -> str:
        if self.perm_lost:
            return "perm"
        if self.stopped:
            return "partial" if self.typed_any else "none"
        if self._held:
            if self._inj.frontmost_pid() == self.target:
                self._type("")         # focus is back: flush the held words
            if self._held:
                return "focus"
        if not self.typed_any:
            return "none"
        if submit and not self._inj.press_return():
            return "perm"
        return "ok"


CONTINUE_PROMPT_S = 120.0   # a follow-up utterance within this continues a prompt


def inject_status_message(inj: str, words: int, submitted: bool) -> str:
    """One short panel line (ASCII, ~40 chars) for a typing outcome."""
    noun = "word" if words == 1 else "words"
    return {
        "ok": f"{'Sent' if submitted else 'Typed'} {words} {noun} on your Mac",
        "off": "",
        "none": "Nothing typed - window not ready",
        "focus": "Window changed - some words not typed",
        "perm": "Mac needs Accessibility - see daemon log",
        "partial": "Audio dropped - check the typed text",
        "unavailable": "Install pyobjc on the Mac to type text",
    }.get(inj, "")


def _log_typer_error(fut: "concurrent.futures.Future") -> None:
    if not fut.cancelled() and fut.exception() is not None:
        log(f"Voice: live typing failed: {fut.exception()}")


class Session:
    def __init__(self, client: BleakClient) -> None:
        self.client = client
        self.refresh_requested = asyncio.Event()
        self._bridge_queue: asyncio.Queue[dict] = asyncio.Queue()
        self._voice_queue: "asyncio.Queue[bytes]" = asyncio.Queue()
        self._voice_buf = bytearray()
        self._voice_blocks: list[bytes] = []
        self._voice_last_seq: int | None = None
        self._voice_rate = SAMPLE_RATE_VOICE
        self._voice_seq_gaps = 0
        self._voice_overrun = False
        self.voice_ready = asyncio.Event()
        # Live transcription (Whistle): the active utterance and the newest
        # partial waiting to go to the panel. Partials arrive from the worker
        # thread via call_soon_threadsafe; only the event loop writes BLE.
        self._loop: "asyncio.AbstractEventLoop | None" = None
        self._voice_cfg: dict = {}
        self._voice_engine = "whisper"
        self._utt: "WhistleUtterance | None" = None
        self._partial: "tuple[str, str] | None" = None
        self._partial_sent: "tuple[str, str] | None" = None
        self._partial_at = 0.0
        self._typer: "LiveTyper | None" = None
        self._last_target: "tuple[int | None, float] | None" = None

    async def wait_for_wake(self, timeout: float) -> None:
        # Sleep until a refresh request, incoming voice audio, or the timeout.
        # Waiting on refresh alone left voice chunks undrained for up to TICK.
        self.voice_ready.clear()
        if not self._voice_queue.empty():
            return
        waiters = [asyncio.ensure_future(self.refresh_requested.wait()),
                   asyncio.ensure_future(self.voice_ready.wait())]
        try:
            await asyncio.wait(waiters, timeout=timeout,
                               return_when=asyncio.FIRST_COMPLETED)
        finally:
            for w in waiters:
                w.cancel()

    def _on_refresh(self, _char, _data: bytearray) -> None:
        log("Refresh requested by device")
        self.refresh_requested.set()

    def _on_bridge_cmd(self, _char, data: bytearray) -> None:
        try:
            msg = json.loads(bytes(data).decode())
        except (UnicodeDecodeError, json.JSONDecodeError) as e:
            log(f"Bridge cmd parse failed: {e}")
            return
        try:
            self._bridge_queue.put_nowait(msg)
        except asyncio.QueueFull:
            log("Bridge cmd queue full; dropping")

    async def setup_refresh_subscription(self) -> None:
        # start_notify awaits CoreBluetooth's CCCD-write confirmation, which
        # never arrives if the peripheral doesn't ACK the subscribe (a
        # half-open link after the OS auto-connects the HID). Unbounded, that
        # await wedges the whole daemon between "Connected" and the first poll
        # — the device then shows nothing until a manual restart. Bound it: the
        # subscription is only an optional device-initiated refresh nudge (we
        # poll every POLL_INTERVAL regardless), so on timeout we proceed.
        try:
            await asyncio.wait_for(
                self.client.start_notify(REQ_CHAR_UUID, self._on_refresh),
                timeout=10,
            )
        except (BleakError, ValueError) as e:
            log(f"Refresh subscription unavailable: {e}")
        except asyncio.TimeoutError:
            log("Refresh subscription timed out; polling without it")

    async def setup_bridge_subscription(self) -> None:
        if sys.platform != "darwin":
            return
        try:
            await asyncio.wait_for(
                self.client.start_notify(DEV_CMD_CHAR_UUID, self._on_bridge_cmd),
                timeout=10,
            )
        except (BleakError, ValueError) as e:
            log(f"Bridge subscription unavailable: {e}")
        except asyncio.TimeoutError:
            log("Bridge subscription timed out")

    async def write_bridge(self, payload: dict, quiet: bool = False) -> bool:
        data = json.dumps(payload, separators=(",", ":")).encode()
        if len(data) > 500:
            log(f"Bridge payload too large ({len(data)} bytes); truncating list")
            if payload.get("t") == "ss" and isinstance(payload.get("n"), list):
                payload = dict(payload)
                payload["n"] = payload["n"][:6]
                data = json.dumps(payload, separators=(",", ":")).encode()
        if not quiet:   # live partials go out at up to 4 Hz; keep the log readable
            log(f"Bridge -> device: {data.decode()}")
        try:
            await self.client.write_gatt_char(HOST_RX_CHAR_UUID, data, response=False)
            return True
        except BleakError as e:
            log(f"Bridge write failed: {e}")
            return False

    async def drain_bridge(self) -> None:
        while not self._bridge_queue.empty():
            msg = await self._bridge_queue.get()
            await self._handle_bridge(msg)

    # ---- Voice dictation (VOICE_CHAR binary chunk stream) -----------------
    def _on_voice(self, _char, data: bytearray) -> None:
        self._voice_queue.put_nowait(bytes(data))
        self.voice_ready.set()   # wake the main loop; don't wait out TICK

    async def setup_voice_subscription(self) -> None:
        self._loop = asyncio.get_running_loop()
        self._voice_cfg = read_voice_cfg()
        self._voice_engine = resolve_voice_engine(self._voice_cfg.get("voice_engine", "auto"))
        log(f"Voice engine: {self._voice_engine}")
        if self._voice_engine == "whistle":
            whistle_engine().warm_up()   # first dictation shouldn't pay the load
        if self._voice_cfg.get("voice_inject"):
            injector = load_injector()
            if injector is None:
                log("Voice: voice_inject=on but pyobjc is missing — install "
                    "pyobjc-framework-Quartz pyobjc-framework-ApplicationServices")
            elif accessibility_ok(injector, prompt=True):
                log("Voice: Accessibility granted — live typing enabled")
        try:
            await asyncio.wait_for(
                self.client.start_notify(VOICE_CHAR_UUID, self._on_voice),
                timeout=10,
            )
        except (BleakError, ValueError) as e:
            log(f"Voice subscription unavailable: {e}")
        except asyncio.TimeoutError:
            log("Voice subscription timed out")

    async def drain_voice(self) -> None:
        # Chunk envelope: [seq:u8][flags:u8][len:u8][payload...]; flags bit0 =
        # EOU. Preserve notification boundaries: block size follows negotiated
        # MTU and each ADPCM block carries its own predictor header, so every
        # block decodes on arrival and feeds the live transcriber immediately.
        while not self._voice_queue.empty():
            chunk = await self._voice_queue.get()
            if len(chunk) < 3:   # EOU is a bare 3-byte header (empty payload)
                continue
            seq, flags = chunk[0], chunk[1]
            # The notify is already length-delimited; the envelope's len byte
            # is redundant (and was left 0 by older firmware), so ignore it.
            payload = bytes(chunk[3:])
            last = self._voice_last_seq
            if last is not None and seq == 0 and last != 0xFF:
                # The device restarts every utterance at seq 0 (not a wrap), so
                # the previous stream's EOU never arrived. Drop its stale
                # partial audio instead of splicing it onto this utterance and
                # reporting the restart as packet loss.
                log(f"Voice: stream restarted ({last} -> 0); "
                    f"discarding {len(self._voice_blocks)} stale blocks")
                self._reset_voice_stream()
            elif last is not None and seq != (last + 1) & 0xFF:
                self._voice_seq_gaps += 1
                log(f"Voice: seq gap {last} -> {seq}")
                if self._typer is not None:
                    self._typer.stop()   # don't type words heard through a gap
            self._voice_last_seq = seq
            if payload:
                packet_rate = 8000 if flags & 0x02 else SAMPLE_RATE_VOICE
                if self._voice_blocks and packet_rate != self._voice_rate:
                    self._voice_overrun = True
                    log("Voice: sample rate changed inside utterance")
                self._voice_rate = packet_rate
                self._voice_blocks.append(payload)
                self._voice_buf.extend(payload)  # compatibility/debug byte count
                self._feed_live(payload, packet_rate)
            if flags & 0x04:
                self._voice_overrun = True
            if flags & 0x01:   # end-of-utterance
                await self._transcribe_voice()
        await self._send_partial()

    # ---- Live partials -----------------------------------------------------
    def _feed_live(self, block: bytes, rate: int) -> None:
        """Decode one ADPCM block and hand it to the live transcriber."""
        if self._utt is None:
            if len(self._voice_blocks) != 1:
                return   # whisper path, or an utterance already being dropped
            self._voice_cfg = read_voice_cfg()   # config edits apply per utterance
            choice = self._voice_cfg.get("voice_engine", "auto")
            if resolve_voice_engine(choice) != "whistle" or np is None:
                return
            self._partial = self._partial_sent = None
            self._typer = self._start_typer()
            utt_holder: list = []

            def on_partial(committed: str, pending: str) -> None:
                # Worker thread -> event loop; stale utterances are ignored.
                if self._loop is not None and utt_holder:
                    self._loop.call_soon_threadsafe(
                        self._set_partial, utt_holder[0], committed, pending)

            self._utt = whistle_engine().start(
                on_partial, self._voice_cfg.get("voice_language"),
                self._voice_cfg.get("voice_keywords") or None,
                final_pass=self._typer is None)
            utt_holder.append(self._utt)
        pcm = ima_adpcm_decode([block], rate)
        if pcm is None or not len(pcm):
            return
        if rate == 8000:
            pcm = np.repeat(pcm, 2)   # Whistle expects 16 kHz (see below)
        self._utt.feed(pcm)

    def _start_typer(self) -> "LiveTyper | None":
        """A live typer when injection is on and permitted, else None (the
        transcript is then typed — or reported — once at release)."""
        if not self._voice_cfg.get("voice_inject"):
            return None
        injector = load_injector()
        if injector is None or not accessibility_ok(injector, prompt=True):
            return None
        prev = None
        if self._last_target and time.time() - self._last_target[1] < CONTINUE_PROMPT_S:
            prev = self._last_target[0]
        return LiveTyper(injector, prev)

    def _set_partial(self, utt: "WhistleUtterance", committed: str, pending: str) -> None:
        if utt is not self._utt:
            return
        self._partial = (committed, pending)
        if self._typer is not None:
            self._typer.push(committed)   # committed words never change: type now
        self.voice_ready.set()

    async def _send_partial(self) -> None:
        if self._partial is None or self._partial == self._partial_sent:
            return
        if time.time() - self._partial_at < PARTIAL_MIN_INTERVAL:
            return   # the next audio block or pass re-wakes the loop
        self._partial_sent = self._partial
        self._partial_at = time.time()
        await self.write_bridge(partial_payload(*self._partial), quiet=True)

    def _abort_live(self) -> None:
        if self._utt is not None:
            self._utt.abort()
            self._utt = None
        if self._typer is not None:
            self._typer.cancel()
            self._typer = None
        self._partial = self._partial_sent = None

    def close_voice(self) -> None:
        """Session teardown: never leave the Whistle worker blocked on a feed."""
        self._abort_live()

    def _reset_voice_stream(self) -> None:
        self._abort_live()
        self._voice_buf.clear()
        self._voice_blocks.clear()
        self._voice_last_seq = None
        self._voice_rate = SAMPLE_RATE_VOICE
        self._voice_seq_gaps = 0
        self._voice_overrun = False

    async def _voice_error(self, message: str, utt=None, typer=None) -> None:
        if utt is not None:
            utt.abort()
        if typer is not None:
            typer.cancel()
        await self.write_bridge({"t": "err", "m": message})

    async def _transcribe_voice(self) -> None:
        audio_bytes = bytes(self._voice_buf)
        blocks = self._voice_blocks[:]
        rate = self._voice_rate
        seq_gaps = self._voice_seq_gaps
        overrun = self._voice_overrun
        cfg = self._voice_cfg or read_voice_cfg()
        # Detach before the reset, which would otherwise abort them.
        utt, self._utt = self._utt, None
        typer, self._typer = self._typer, None
        self._partial = self._partial_sent = None
        self._reset_voice_stream()
        if not audio_bytes:
            log("Voice: end-of-utterance with no audio")
            await self._voice_error("No audio received", utt, typer)
            return
        log(f"Voice: transport blocks={len(blocks)} bytes={len(audio_bytes)} "
            f"rate={rate} gaps={seq_gaps} overrun={int(overrun)}")
        if overrun or seq_gaps:
            typed = typer is not None and typer.pushed_any
            await self._voice_error(
                "Audio dropped - check the typed text" if typed
                else "Audio link dropped data - please retry", utt, typer)
            return
        dur = sum(max(0, len(b) - 4) * 2 for b in blocks) / rate
        if dur < 0.3:   # a tap, not speech — models would only hallucinate
            log(f"Voice: {dur:.2f}s utterance — too short, skipped")
            await self._voice_error("Too short - hold to talk", utt, typer)
            return
        started = time.time()
        text: "str | None" = None
        committed = ""
        engine = "whisper"
        if utt is not None and utt.future is not None:
            utt.close()
            try:
                result = await asyncio.wait_for(asyncio.wrap_future(utt.future),
                                                timeout=VOICE_FINAL_TIMEOUT)
                if result is not None:
                    text, committed = result
            except asyncio.TimeoutError:
                utt.abort()
                log("Voice: Whistle timed out")
            except Exception as e:   # engine/runtime errors surface here
                log(f"Voice: Whistle failed: {e}")
                if typer is not None and typer.pushed_any:
                    # Words are already on screen; re-typing a whisper pass
                    # would duplicate them.
                    await self._voice_error(
                        "Transcription failed - check the typed text", typer=typer)
                    return
                utt = None   # fall through to faster-whisper below
            engine = "whistle"
        if utt is None or utt.future is None:
            if typer is not None:          # nothing live to finish; type at the end
                typer.cancel()
                typer = None
            pcm = ima_adpcm_decode(blocks, rate)
            if pcm is None or len(pcm) == 0:
                await self._voice_error("Voice decode failed")
                return
            if rate == 8000:
                # faster-whisper treats numpy waveforms as 16 kHz. Repeat samples
                # to preserve time; this cannot restore >4 kHz detail, which is why
                # 8 kHz is reserved for small-MTU compatibility links.
                pcm = np.repeat(pcm, 2)
            # Off the event loop so BLE callbacks keep flowing during the pass.
            text = await asyncio.to_thread(whisper_transcribe, pcm)
            engine = "whisper"
        log(f"Voice: {dur:.1f}s @ {rate} Hz — {engine} final in "
            f"{(time.time() - started) * 1000:.0f} ms")
        if not text:
            # The engines log their own failures; None with no log line
            # just means no speech was heard.
            await self._voice_error("No speech recognized", typer=typer)
            return

        submit = bool(cfg.get("voice_autosubmit"))
        inj = await self._deliver_text(text, committed, typer, cfg, submit)
        words = len(text.split())
        await self.write_bridge({
            "t": "vt", "tx": tail_text(device_text(text), FINAL_MAX_CHARS),
            "inj": inj, "n": words, "m": inject_status_message(inj, words, submit)})

    async def _deliver_text(self, text: str, committed: str,
                            typer: "LiveTyper | None", cfg: dict,
                            submit: bool) -> str:
        """Type the transcript (live tail or whole) and return the outcome:
        off | ok | none | focus | perm | partial | unavailable."""
        if typer is not None:
            inj = await typer.finish(committed, submit)
            # Remember the window so a follow-up utterance continues the
            # same unsent prompt with a separating space.
            self._last_target = ((typer.target, time.time())
                                 if inj == "ok" and not submit else None)
        elif not cfg.get("voice_inject"):
            return "off"
        else:
            injector = load_injector()
            if injector is None:
                log("Voice: pyobjc not installed — cannot type the transcript")
                return "unavailable"
            if not accessibility_ok(injector, prompt=True):
                return "perm"
            ok = await asyncio.to_thread(injector.type_prompt, text, submit=submit)
            inj = "ok" if ok else "perm"
        if inj == "ok":
            log(f"Voice: typed into focused window (autosubmit={submit})")
        else:
            log(f"Voice: typing outcome: {inj}")
        return inj

    async def _handle_bridge(self, msg: dict) -> None:
        cmd = msg.get("c")
        if cmd == "ls":
            seen: set[str] = set()
            merged = []
            for cfg in read_config_dirs():
                for s in list_recent_sessions(limit=20, config_dir=cfg):
                    if s.session_id in seen:
                        continue
                    seen.add(s.session_id)
                    merged.append(s)
            merged.sort(key=lambda s: s.mtime, reverse=True)
            await self.write_bridge(sessions_payload(merged[:10]))
            return
        if cmd == "rs":
            sid = str(msg.get("i") or "")
            if not sid:
                await self.write_bridge({"t": "err", "m": "Missing session id"})
                return
            cwd = None
            for cfg in read_config_dirs():
                cwd = find_session_cwd(sid, config_dir=cfg)
                if cwd:
                    break
            if not cwd:
                await self.write_bridge({"t": "err", "m": "Session not found"})
                return
            if resume_session_mac(sid, cwd):
                log(f"Resumed Claude session {sid[:8]}… in {cwd}")
                label = resume_terminal_label(read_resume_terminal_setting())
                await self.write_bridge({"t": "ok", "m": f"Opening in {label}…"})
            else:
                await self.write_bridge({"t": "err", "m": "Could not launch claude"})
            return
        log(f"Unknown bridge cmd: {msg!r}")

    async def write_payload(self, payload: dict) -> bool:
        data = json.dumps(payload, separators=(",", ":")).encode()
        log(f"Sending: {data.decode()}")
        try:
            await self.client.write_gatt_char(RX_CHAR_UUID, data, response=False)
            return True
        except BleakError as e:
            log(f"Write failed: {e}")
            return False


def _is_encryption_error(exc: BaseException) -> bool:
    """True if a connect error is a macOS bonding/encryption mismatch.

    macOS reports a stale bond as CBErrorDomain Code=15 ("Failed to encrypt
    the connection..."). Match on the message text so we don't depend on how
    bleak wraps the underlying CoreBluetooth error.
    """
    s = str(exc).lower()
    return "code=15" in s or "encrypt" in s


# blueutil talks to Bluetooth via IOBluetooth, which on recent macOS needs its
# OWN Bluetooth TCC grant (separate from the daemon's CoreBluetooth grant).
# Without it, blueutil *hangs* instead of erroring — so every call is bounded
# by a timeout and a hang is reported as a permission problem, not a crash.
BLUEUTIL_TIMEOUT = 8


def _blueutil(*args: str) -> str | None:
    """Run `blueutil <args>`, returning stdout, or None on failure/timeout.

    A timeout almost always means blueutil lacks Bluetooth permission (it
    blocks rather than failing), so we surface that cause explicitly.
    """
    try:
        return subprocess.run(
            ["blueutil", *args],
            capture_output=True, text=True,
            timeout=BLUEUTIL_TIMEOUT, check=True,
        ).stdout
    except subprocess.TimeoutExpired:
        log(f"blueutil {' '.join(args)} timed out — it likely lacks Bluetooth "
            "permission. Grant it under System Settings > Privacy & Security > "
            "Bluetooth (run `blueutil --paired` once from Terminal to prompt).")
        return None
    except (subprocess.SubprocessError, OSError) as e:
        log(f"blueutil {' '.join(args)} failed: {e}")
        return None


def unpair_macos() -> bool:
    """Forget a stale macOS bond for DEVICE_NAME so the device can re-pair.

    A Code=15 "failed to encrypt" connect error means macOS holds bonding
    keys that no longer match the ESP32's (e.g. after a firmware reflash or
    the on-device bond-clear gesture). The firmware pairs "just works" (no
    MITM), so once the stale bond is gone the next connect re-bonds silently
    with no GUI prompt.

    CoreBluetooth exposes no unpair API, so we shell out to `blueutil`. The
    daemon only knows the peripheral's CoreBluetooth UUID, not the BD_ADDR
    that blueutil needs, so we map by name via `blueutil --paired`. Returns
    True if a bond was removed. Mirrors the Linux daemon's `bluetoothctl
    remove` self-heal.
    """
    if not shutil.which("blueutil"):
        log("Stale bond detected but `blueutil` is not installed; cannot "
            "auto-recover. Run `brew install blueutil`, or forget "
            f"'{DEVICE_NAME}' in System Settings > Bluetooth and reconnect.")
        return False

    out = _blueutil("--paired")
    if out is None:
        return False

    # Each line looks like: "address: <bdaddr>, ... name: "Clawdmeter", ..."
    addr = None
    for line in out.splitlines():
        if f'name: "{DEVICE_NAME}"' in line:
            m = re.search(r"address:\s*([0-9a-fA-F:-]+)", line)
            if m:
                addr = m.group(1)
                break
    if not addr:
        log(f"No paired '{DEVICE_NAME}' found to unpair (already forgotten?)")
        return False

    if _blueutil("--unpair", addr) is None:
        return False
    log(f"Unpaired stale bond for '{DEVICE_NAME}' [{addr}]; re-pairing on "
        "next connect")
    return True


async def connect_and_run(target, stop_event: asyncio.Event) -> bool:
    """Connect to a target and poll until disconnected or stopped.

    ``target`` is either an address string (Linux) or a BLEDevice carrying
    live CoreBluetooth details (macOS). Returns True if the connection was
    used successfully (so the caller keeps the cached address), False if the
    connection failed and the cache should be invalidated.
    """
    display = target if isinstance(target, str) else target.address
    log(f"Connecting to {display}...")
    client = BleakClient(target)
    try:
        # Bound the connect the same way #84 bounded the refresh subscribe.
        # On macOS the OS auto-connects the firmware's HID link, so
        # CoreBluetooth can hand us a half-open peripheral whose GATT connect
        # handshake never completes. BleakClient's own timeout governs
        # discovery, not connectPeripheral, so an unbounded await here wedges
        # the single-threaded daemon forever at "Connecting..." (observed ~13h,
        # device stuck on stale data). wait_for raises TimeoutError, which the
        # handler below already treats as a connection failure -> drop the
        # cached address and rescan.
        await asyncio.wait_for(client.connect(), timeout=CONNECT_TIMEOUT)
    except (BleakError, asyncio.TimeoutError) as e:
        log(f"Connection failed: {e}")
        if sys.platform == "darwin" and _is_encryption_error(e):
            log("Encryption failed — likely a stale macOS bond; self-healing")
            unpair_macos()
        return False

    if not client.is_connected:
        log("Connection failed (no error but not connected)")
        return False

    log("Connected")
    session = Session(client)
    await session.setup_refresh_subscription()
    await session.setup_bridge_subscription()
    await session.setup_voice_subscription()

    last_poll = 0.0
    used_successfully = False
    try:
        while client.is_connected and not stop_event.is_set():
            await session.drain_bridge()
            await session.drain_voice()
            now = time.time()
            elapsed = now - last_poll
            if session.refresh_requested.is_set() or elapsed >= POLL_INTERVAL:
                session.refresh_requested.clear()
                # Pure free-ride: read whatever access token(s) Claude Code
                # currently holds across the configured config dirs and NEVER
                # refresh them ourselves. Claude Code (the token's owner) does all
                # refreshing; refreshing here would race its rotation and feed the
                # OAuth endpoint's rate limit (429). When no dir has a usable token
                # we signal "No data" so the device idles instead of holding stale
                # numbers until the CLI re-seeds it.
                payload, dead = await poll_active()
                if payload is not None:
                    if await session.write_payload(payload):
                        last_poll = time.time()
                        used_successfully = True
                elif dead:
                    # No live token in any config dir (missing, or a 401/expired
                    # token) -> show "No data" now instead of stale numbers. Guard
                    # last_poll on the write result (like the data path) so a
                    # failed beat retries next tick instead of throttling what may
                    # be a healthy link for a full POLL_INTERVAL.
                    log("No usable token; signalling no-data to device — run "
                        "`claude login` or use the CLI to let Claude Code renew it")
                    if await session.write_payload({"ok": False}):
                        last_poll = time.time()
                else:
                    # Transient poll failure (a live token that didn't answer this
                    # cycle) -> stay silent and retry next tick.
                    log("No usable config dir this cycle")

            await session.wait_for_wake(TICK)
    finally:
        session.close_voice()
        try:
            await client.disconnect()
        except BleakError:
            pass

    log("Device disconnected" if not stop_event.is_set() else "Stopping")
    return used_successfully


LOCK_FILE = CONFIG_FILE.parent / "daemon.lock"


def acquire_single_instance(lock_path: Path = LOCK_FILE, wait: "bool | None" = None,
                            poll: float = 5.0):
    """Hold an exclusive lock so only one daemon talks to the board.

    Two daemons both receive every voice notification, so both transcribe
    and both type — duplicate text, plus a terminal-launched copy fails the
    Accessibility check. Under launchd (parent pid 1) a busy lock means wait
    for the other copy to exit; interactively, explain and return None.
    Returns the open lock file (keep it referenced) or None.
    """
    try:
        import fcntl
    except ImportError:          # not POSIX: nothing to lock
        return True
    if wait is None:
        wait = os.getppid() == 1
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    fh = open(lock_path, "a+")
    announced = False
    while True:
        try:
            fcntl.flock(fh, fcntl.LOCK_EX | fcntl.LOCK_NB)
            break
        except BlockingIOError:
            fh.seek(0)
            holder = fh.read().strip() or "?"
            if not wait:
                log(f"Another ClawdDeck daemon is already running (pid {holder}); "
                    "exiting. Stop it first: launchctl unload "
                    "~/Library/LaunchAgents/com.user.claude-usage-daemon.plist")
                fh.close()
                return None
            if not announced:
                log(f"Waiting for the other daemon (pid {holder}) to exit...")
                announced = True
            time.sleep(poll)
    fh.seek(0)
    fh.truncate()
    fh.write(str(os.getpid()))
    fh.flush()
    return fh


async def main(exit_after: "float | None" = None) -> None:
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()
    if exit_after:
        # Installer permission-priming run: end on our own instead of
        # relying on the user to press Ctrl+C (a forgotten foreground copy
        # keeps competing with the launchd daemon).
        loop.call_later(exit_after, lambda: (log(f"--exit-after {exit_after:g}s "
                                                  "reached"), stop_event.set()))

    def _stop(*_args: object) -> None:
        log("Daemon stopping")
        stop_event.set()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, _stop)
        except NotImplementedError:
            signal.signal(sig, _stop)

    log("=== Claude Usage Tracker Daemon (BLE, macOS) ===")
    log(f"Poll interval: {POLL_INTERVAL}s")

    backoff = 1
    skip_addr: str | None = None  # macOS: a peripheral to skip for one cycle
    while not stop_event.is_set():
        # Apply any pending skip exactly once, then clear it so the next
        # cycle re-tries retrieveConnected (the device may have recovered).
        target = await discover_target(skip_addr=skip_addr)
        skip_addr = None
        if not target:
            log(f"Device not found, retrying in {backoff}s...")
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)
            continue

        addr = target if isinstance(target, str) else target.address
        ok = await connect_and_run(target, stop_event)
        if not ok:
            if sys.platform == "darwin":
                # No string cache to drop; instead skip this stale handle on
                # the next retrieveConnected so the scan fallback is reachable.
                skip_addr = addr
            else:
                log("Invalidating cached address")
                SAVED_ADDR_FILE.unlink(missing_ok=True)
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)
        else:
            backoff = 1


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="ClawdDeck BLE daemon (macOS)")
    parser.add_argument("--exit-after", type=float, metavar="SECONDS",
                        help="stop after SECONDS (installer permission priming)")
    args = parser.parse_args()
    _lock = acquire_single_instance()
    if _lock is None:
        sys.exit(1)
    try:
        asyncio.run(main(exit_after=args.exit_after))
    except KeyboardInterrupt:
        sys.exit(0)
