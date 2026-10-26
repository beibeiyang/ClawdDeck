"""macOS keystroke injection for voice dictation (ClawdDeck daemon).

Types a transcript into the FOCUSED window — whatever terminal the user has
in front — so the text becomes a prompt for any CLI agent (Claude Code,
Codex, Cursor, aider, plain shells). CGEvent unicode-string posting via
PyObjC; no external helper binary.

Permissions: the daemon's interpreter needs Accessibility (System Settings →
Privacy & Security → Accessibility → add/enable the python binary). The
grant attaches to the interpreter path, not this script.

Typing model (per the design session 20261018_170703_1b4191):
- newlines flattened to single spaces (a transcript is one prompt line)
- safety prefix " x" typed + backspaced: if Enter misfires mid-typing, the
  victim command is " x" — not the transcript
- burst pacing (fast for the first chars, then settled) keeps TUIs and
  shells happy without a visible crawl
- submit=False is the default: type and leave the cursor blinking; the
  user reviews and hits Enter themselves. Autosubmit is an explicit opt-in.
"""

import time

CHAR_DELAY = 0.010          # settled per-char delay (TUI-safe)
BURST_N = 20                # chars typed at burst speed before settling
BURST_DELAY = 0.002
BACKSPACE_CODE = 51         # kVK_Delete
RETURN_CODE = 36            # kVK_Return

_quartz = None              # lazy PyObjC import (daemon stays up w/o it)


def _load_quartz():
    global _quartz
    if _quartz is None:
        import Quartz  # noqa: N813 - pyobjc-framework-Quartz
        _quartz = Quartz
    return _quartz


def preflight(prompt: bool = False) -> bool:
    """True when the Accessibility permission is granted to this process.

    prompt=True asks macOS to show its "allow accessibility" dialog for the
    responsible app (once per launch is plenty; macOS dedupes repeats).
    """
    try:
        if prompt:
            from ApplicationServices import (AXIsProcessTrustedWithOptions,
                                             kAXTrustedCheckOptionPrompt)
            return bool(AXIsProcessTrustedWithOptions(
                {kAXTrustedCheckOptionPrompt: True}))
        from ApplicationServices import AXIsProcessTrusted
        return bool(AXIsProcessTrusted())
    except ImportError:
        return False


def responsible_app_hint() -> str:
    """Which app macOS checks for the Accessibility grant, in plain words.

    TCC attributes a process to whoever launched it: under launchd that is
    the interpreter's own Python.app; started from a terminal it is the
    terminal app (Terminal, iTerm, ...), whose grant is what counts.
    """
    import os
    import sys
    if os.getppid() == 1:
        return os.path.join(sys.base_prefix, "Resources", "Python.app")
    return "the terminal app this daemon was started from"


def frontmost_pid() -> "int | None":
    """PID of the app that currently receives keystrokes (None if unknown)."""
    try:
        from AppKit import NSWorkspace
        app = NSWorkspace.sharedWorkspace().frontmostApplication()
        return int(app.processIdentifier()) if app is not None else None
    except Exception:   # AppKit missing or no window server
        return None


def _kbd_event(src, code: int, down: bool):
    q = _load_quartz()
    e = q.CGEventCreateKeyboardEvent(src, code, down)
    return e


def _post_unicode(text: str) -> None:
    q = _load_quartz()
    src = q.CGEventSourceCreate(q.kCGEventSourceStateHIDSystemState)
    kd = q.CGEventCreateKeyboardEvent(src, 0, True)
    ku = q.CGEventCreateKeyboardEvent(src, 0, False)
    q.CGEventKeyboardSetUnicodeString(kd, len(text), text)
    q.CGEventKeyboardSetUnicodeString(ku, len(text), text)
    q.CGEventPost(q.kCGSessionEventTap, kd)
    q.CGEventPost(q.kCGSessionEventTap, ku)


def _mash_backspaces(n: int) -> None:
    q = _load_quartz()
    src = q.CGEventSourceCreate(q.kCGEventSourceStateHIDSystemState)
    for _ in range(n):
        for down in (True, False):
            q.CGEventPost(q.kCGSessionEventTap, _kbd_event(src, BACKSPACE_CODE, down))
        time.sleep(BURST_DELAY)


def type_text(text: str, safety: bool = False) -> bool:
    """Type `text` verbatim into the focused window (no Enter, no
    whitespace flattening beyond newlines). Used for live dictation, where
    words arrive in pieces and the caller owns the spacing between them."""
    if not preflight():
        return False
    text = text.replace("\r", " ").replace("\n", " ")
    if not text:
        return True
    if safety:
        _post_unicode(" x")
        time.sleep(0.05)
        _mash_backspaces(2)
    q = _load_quartz()
    src = q.CGEventSourceCreate(q.kCGEventSourceStateHIDSystemState)
    for i, ch in enumerate(text):
        kd = q.CGEventCreateKeyboardEvent(src, 0, True)
        ku = q.CGEventCreateKeyboardEvent(src, 0, False)
        q.CGEventKeyboardSetUnicodeString(kd, 1, ch)
        q.CGEventKeyboardSetUnicodeString(ku, 1, ch)
        q.CGEventPost(q.kCGSessionEventTap, kd)
        q.CGEventPost(q.kCGSessionEventTap, ku)
        time.sleep(BURST_DELAY if i < BURST_N else CHAR_DELAY)
    return True


def press_return() -> bool:
    if not preflight():
        return False
    q = _load_quartz()
    src = q.CGEventSourceCreate(q.kCGEventSourceStateHIDSystemState)
    for down in (True, False):
        q.CGEventPost(q.kCGSessionEventTap, _kbd_event(src, RETURN_CODE, down))
    return True


def type_prompt(text: str, submit: bool = False, safety: bool = True) -> bool:
    """Type `text` into the focused window; optionally press Enter.

    Returns False (and types nothing) when the Accessibility permission is
    missing — callers surface that to the user instead of failing silently.
    """
    if not preflight():
        return False
    text = " ".join(text.split())          # newlines → single spaces
    if not text:
        return True
    if safety:
        _post_unicode(" x")                # junk prefix: even a misfired Enter
        time.sleep(0.05)                   #   submits " x", not the transcript
        _mash_backspaces(2)
    q = _load_quartz()
    src = q.CGEventSourceCreate(q.kCGEventSourceStateHIDSystemState)
    for i in range(len(text)):
        ch = text[i]
        kd = q.CGEventCreateKeyboardEvent(src, 0, True)
        ku = q.CGEventCreateKeyboardEvent(src, 0, False)
        q.CGEventKeyboardSetUnicodeString(kd, 1, ch)
        q.CGEventKeyboardSetUnicodeString(ku, 1, ch)
        q.CGEventPost(q.kCGSessionEventTap, kd)
        q.CGEventPost(q.kCGSessionEventTap, ku)
        time.sleep(BURST_DELAY if i < BURST_N else CHAR_DELAY)
    if submit:
        for down in (True, False):
            q.CGEventPost(q.kCGSessionEventTap, _kbd_event(src, RETURN_CODE, down))
    return True