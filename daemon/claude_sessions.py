"""List and resume Claude Code sessions from ~/.claude/projects transcripts."""

from __future__ import annotations

import json
import os
import re
import shlex
import shutil
import subprocess
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

CONFIG_FILE = Path.home() / ".config" / "claude-usage-monitor" / "config"
RESUME_TERMINALS = ("iterm", "terminal")
DEFAULT_RESUME_TERMINAL = "iterm"


@dataclass(frozen=True)
class ClaudeSession:
    session_id: str
    title: str
    project: str
    cwd: str
    mtime: float


_CAVEAT_RE = re.compile(r"^<local-command")
_CMD_RE = re.compile(r"^<command")


def _project_cwd(project_dir: str) -> str:
    """`-Users-me-git-Foo` -> `/Users/me/git/Foo`."""
    if not project_dir.startswith("-"):
        return project_dir
    return "/" + project_dir[1:].replace("-", "/")


def _short_project(cwd: str) -> str:
    base = os.path.basename(cwd.rstrip("/"))
    return base or cwd


def _text_from_user_content(content) -> str | None:
    if isinstance(content, str):
        text = content.strip()
    elif isinstance(content, list):
        parts: list[str] = []
        for block in content:
            if isinstance(block, dict) and block.get("type") == "text":
                parts.append(str(block.get("text", "")))
        text = " ".join(parts).strip()
    else:
        return None
    if not text:
        return None
    if _CAVEAT_RE.match(text) or _CMD_RE.match(text):
        return None
    text = re.sub(r"\s+", " ", text)
    return text


def _title_from_jsonl(path: Path) -> str:
    try:
        with path.open(encoding="utf-8", errors="replace") as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                try:
                    obj = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if obj.get("type") != "user":
                    continue
                msg = obj.get("message") or {}
                text = _text_from_user_content(msg.get("content"))
                if text:
                    return text
    except OSError:
        pass
    return path.stem[:8]


def list_recent_sessions(
    *,
    config_dir: Path | None = None,
    limit: int = 10,
) -> list[ClaudeSession]:
    """Return recent resumable sessions, newest first (matches `claude -r` picker)."""
    roots: list[Path] = []
    if config_dir is not None:
        roots.append(config_dir / "projects")
    else:
        roots.append(Path.home() / ".claude" / "projects")

    rows: list[ClaudeSession] = []
    seen: set[str] = set()
    for root in roots:
        if not root.is_dir():
            continue
        for proj in root.iterdir():
            if not proj.is_dir():
                continue
            cwd = _project_cwd(proj.name)
            project = _short_project(cwd)
            for transcript in proj.glob("*.jsonl"):
                sid = transcript.stem
                if sid in seen:
                    continue
                seen.add(sid)
                title = _title_from_jsonl(transcript)
                rows.append(
                    ClaudeSession(
                        session_id=sid,
                        title=title,
                        project=project,
                        cwd=cwd,
                        mtime=transcript.stat().st_mtime,
                    )
                )

    rows.sort(key=lambda s: s.mtime, reverse=True)
    return rows[:limit]


def _applescript_escape(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"')


def read_resume_terminal_setting(config_file: Path | None = None) -> str:
    """Which macOS terminal opens `claude --resume` from the Sessions app.

    Values: iterm | terminal. Re-read each call (config is polled live).
    """
    path = config_file or CONFIG_FILE
    val = ""
    try:
        if path.exists():
            for line in path.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if not line or "=" not in line:
                    continue
                key, raw = line.split("=", 1)
                if key.strip().lower() == "resume_terminal":
                    val = raw.strip().lower()
    except OSError:
        pass
    if val in RESUME_TERMINALS:
        return val
    return DEFAULT_RESUME_TERMINAL


def resume_terminal_label(terminal: str | None = None) -> str:
    t = (terminal or read_resume_terminal_setting()).lower()
    return {"iterm": "iTerm", "terminal": "Terminal"}.get(t, "Terminal")


def _resume_script_iterm(shell_cmd: str) -> str:
    cmd = _applescript_escape(shell_cmd)
    return (
        'tell application "iTerm"\n'
        "    activate\n"
        "    set newWindow to (create window with default profile)\n"
        "    tell current session of newWindow\n"
        f'        write text "{cmd}"\n'
        "    end tell\n"
        "end tell\n"
    )


def _resume_script_terminal(shell_cmd: str) -> str:
    cmd = _applescript_escape(shell_cmd)
    return (
        'tell application "Terminal"\n'
        "    activate\n"
        f'    do script "{cmd}"\n'
        "end tell\n"
    )


def resume_session_mac(
    session_id: str,
    cwd: str,
    *,
    terminal: str | None = None,
) -> bool:
    """Open a new terminal window and run `claude --resume <id>` in `cwd`."""
    claude = shutil.which("claude")
    if not claude:
        return False

    term = (terminal or read_resume_terminal_setting()).lower()
    if term not in RESUME_TERMINALS:
        term = DEFAULT_RESUME_TERMINAL

    shell_cmd = (
        f"cd {shlex.quote(cwd)} && "
        f"{shlex.quote(claude)} --resume {shlex.quote(session_id)}"
    )
    if term == "iterm":
        script = _resume_script_iterm(shell_cmd)
    else:
        script = _resume_script_terminal(shell_cmd)

    try:
        subprocess.run(
            ["osascript", "-e", script],
            check=True,
            capture_output=True,
            text=True,
        )
        return True
    except (subprocess.CalledProcessError, OSError):
        return False


def find_session_cwd(session_id: str, *, config_dir: Path | None = None) -> str | None:
    for s in list_recent_sessions(limit=500, config_dir=config_dir):
        if s.session_id == session_id:
            return s.cwd
    return None


def sessions_payload(sessions: list[ClaudeSession]) -> dict:
    """Compact JSON for the 512-byte BLE bridge buffer."""
    items = []
    for s in sessions:
        items.append(
            {
                "i": s.session_id,
                "l": s.title[:40],
                "p": s.project[:24],
            }
        )
    return {"t": "ss", "n": items}


def format_age(mtime: float) -> str:
    delta = max(0, datetime.now(timezone.utc).timestamp() - mtime)
    if delta < 3600:
        return f"{int(delta // 60)}m ago"
    if delta < 86400:
        return f"{int(delta // 3600)}h ago"
    return f"{int(delta // 86400)}d ago"
