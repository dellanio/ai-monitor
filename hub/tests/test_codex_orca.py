"""Fonte Codex/ChatGPT e reconhecimento do Orca."""
import json
from datetime import datetime
from types import SimpleNamespace

from hub.claude_source import parse_session_file, source_for
from hub.codex_source import CodexSource
from hub.config import CodexConfig
from hub.state import HubState


def test_orca_is_recognised_by_entrypoint_or_folder(tmp_path):
    assert source_for("C:\\Users\\x\\.orca\\bots\\orcrux") == "orca"
    assert source_for("D:/projetos/orca-bots") == "claude"       # só o nome não basta
    assert source_for("D:/x", entrypoint="orca") == "orca"
    f = tmp_path / "s.jsonl"
    f.write_text(json.dumps({"type": "user", "entrypoint": "orca", "cwd": "C:/y",
                             "message": {"content": "oi"}}) + "\n", encoding="utf-8")
    assert parse_session_file(f)["source"] == "orca"


class FakeHub:
    def __init__(self):
        self.state = HubState()
        self.finished = []
        self.log = __import__("logging").getLogger("t")

    def upsert_session(self, s):
        self.state.upsert_session(s)

    async def push_finished(self, sid, title, source=""):
        self.finished.append((sid, title, source))


def _rollout(day, name, originator, source="vscode"):
    f = day / name
    meta = {"type": "session_meta", "payload": {"id": name[-12:], "originator": originator,
                                                "source": source, "cwd": "C:\\proj\\meu-app"}}
    user = {"type": "response_item", "payload": {"type": "message", "role": "user",
            "content": [{"type": "input_text", "text": "<environment_context>x</environment_context>"}]}}
    user2 = {"type": "response_item", "payload": {"type": "message", "role": "user",
             "content": [{"type": "input_text", "text": "arrume o login"}]}}
    f.write_text("\n".join(json.dumps(r) for r in (meta, user, user2)) + "\n", encoding="utf-8")
    return f


def _append(f, et):
    with open(f, "a", encoding="utf-8") as fh:
        fh.write(json.dumps({"type": "event_msg", "payload": {"type": et}}) + "\n")


def test_codex_source_tracks_turns_and_skips_automation(tmp_path):
    d = datetime.now()
    day = tmp_path / f"{d:%Y}" / f"{d:%m}" / f"{d:%d}"
    day.mkdir(parents=True)
    app = _rollout(day, "rollout-a-000000000001.jsonl", "Codex Desktop")
    orca = _rollout(day, "rollout-b-000000000002.jsonl", "orca_desktop")
    _rollout(day, "rollout-c-000000000003.jsonl", "openbot")
    _rollout(day, "rollout-d-000000000004.jsonl", "Codex Desktop", source={"subagent": "x"})
    hub = FakeHub()
    src = CodexSource(hub, CodexConfig(enabled=True, sessions_root=str(tmp_path)))

    _append(app, "task_started")
    src._first = False
    src._scan()
    sessions = {s.source: s for s in hub.state.sessions.values()}
    assert set(sessions) == {"codex"}                       # openbot/subagente/sem turno fora
    assert sessions["codex"].status == "working"
    assert sessions["codex"].title == "arrume o login"      # ignora o bloco injetado
    assert sessions["codex"].profile == "meu-app"

    _append(app, "task_complete")
    _append(orca, "task_started")
    _append(orca, "task_complete")
    done = src._scan()
    assert sorted(s for _, _, s in done) == ["codex", "orca"]
    assert all(s.status == "finished" for s in hub.state.sessions.values())
    assert len(hub.state.sessions) == 2


def test_title_skips_the_attachment_list():
    from hub.codex_source import _user_text
    p = {"content": [{"type": "input_text", "text": "\n# Files mentioned by the user:\n\n## a.png: C:/a.png\n\n## My request:\nArrume a tela"}]}
    assert _user_text(p) == "Arrume a tela"


def test_first_scan_does_not_announce_history(tmp_path):
    d = datetime.now()
    day = tmp_path / f"{d:%Y}" / f"{d:%m}" / f"{d:%d}"
    day.mkdir(parents=True)
    f = _rollout(day, "rollout-a-000000000001.jsonl", "codex-tui")
    _append(f, "task_started")
    _append(f, "task_complete")
    hub = FakeHub()
    src = CodexSource(hub, CodexConfig(enabled=True, sessions_root=str(tmp_path)))
    assert src._scan() == []                                # histórico: sem aviso falado
    assert list(hub.state.sessions.values())[0].status == "finished"
