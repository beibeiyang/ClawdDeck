"""Tests for Claude session listing and resume-terminal config."""

from pathlib import Path

from daemon import claude_sessions as cs


def test_read_resume_terminal_defaults_to_iterm(tmp_path, monkeypatch):
    monkeypatch.setattr(cs, "CONFIG_FILE", tmp_path / "config")
    assert cs.read_resume_terminal_setting() == "iterm"


def test_read_resume_terminal_from_config(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("resume_terminal = terminal\n")
    monkeypatch.setattr(cs, "CONFIG_FILE", cfg)
    assert cs.read_resume_terminal_setting() == "terminal"


def test_read_resume_terminal_unknown_falls_back(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("resume_terminal = kitty\n")
    monkeypatch.setattr(cs, "CONFIG_FILE", cfg)
    assert cs.read_resume_terminal_setting() == "iterm"


def test_resume_terminal_label():
    assert cs.resume_terminal_label("iterm") == "iTerm"
    assert cs.resume_terminal_label("terminal") == "Terminal"


def test_iterm_script_uses_iterm_app():
    script = cs._resume_script_iterm("cd /tmp && claude --resume abc")
    assert 'tell application "iTerm"' in script
    assert "write text" in script
    assert "Terminal" not in script.split("iTerm")[0]


def test_terminal_script_uses_terminal_app():
    script = cs._resume_script_terminal("cd /tmp && claude --resume abc")
    assert 'tell application "Terminal"' in script
    assert "do script" in script
