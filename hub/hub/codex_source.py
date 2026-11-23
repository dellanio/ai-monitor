"""Codex / ChatGPT source (somente leitura).

Lê os registros de sessão do Codex (``~/.codex/sessions/AAAA/MM/DD/rollout-*.jsonl``)
que o app Codex (ChatGPT), o ``codex`` do terminal e o Orca gravam:

- ``event_msg/task_started``  -> sessão "trabalhando";
- ``event_msg/task_complete`` -> "terminou" (dispara o aviso "ChatGPT terminou");
- ``event_msg/turn_aborted``  -> "parada".

Só entram sessões interativas: subagentes (``source`` = objeto), ``codex exec`` e
automações (ex.: OpenBot) ficam de fora para não encher a tela nem o alto-falante.
O Codex não registra os pedidos de permissão nesses arquivos, então não há
"precisa de você" vindo daqui.
"""
from __future__ import annotations

import asyncio
import json
import os
import time
from datetime import datetime, timedelta
from pathlib import Path

from .sources import Source
from .state import Session

MAX_TITLE_LEN = 60
# originator -> fonte mostrada/falada
ORIGINATORS = {
    "Codex Desktop": "codex",
    "codex-tui": "codex",
    "codex_cli_rs": "codex",
    "orca_desktop": "orca",
}


def _user_text(payload: dict) -> str:
    parts = []
    for c in payload.get("content") or []:
        if isinstance(c, dict) and c.get("type") in ("input_text", "text"):
            parts.append(str(c.get("text", "")))
    raw = "\n".join(parts)
    # Com anexos, o app Codex põe a lista de arquivos antes de "## My request...:".
    for marker in ("## My request for Codex:", "## My request:"):
        if marker in raw:
            raw = raw.split(marker, 1)[1]
            break
    text = " ".join(raw.split())
    # Blocos injetados pelo Codex (<environment_context>, AGENTS.md...) não são o pedido.
    if not text or text.startswith("<") or text.startswith("# AGENTS.md"):
        return ""
    return text


class _Track:
    __slots__ = ("offset", "rest", "sid", "source", "title", "cwd", "status", "skip", "meta_done")

    def __init__(self) -> None:
        self.offset = 0
        self.rest = b""
        self.sid = ""
        self.source = "codex"
        self.title = ""
        self.cwd = ""
        self.status = ""
        self.skip = False
        self.meta_done = False


class CodexSource(Source):
    name = "codex"

    def __init__(self, hub, cfg) -> None:
        super().__init__(hub, cfg)
        self.root = Path(os.path.expanduser(cfg.sessions_root))
        self.poll_interval = float(cfg.poll_interval_s)
        self._tracks: dict[str, _Track] = {}
        self._task: asyncio.Task | None = None
        self._first = True

    async def start(self) -> None:
        self._task = asyncio.create_task(self._loop())

    async def stop(self) -> None:
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except asyncio.CancelledError:
                pass

    async def _loop(self) -> None:
        while True:
            try:
                finished = self._scan()  # leitura incremental: pouco trabalho por passada
                for sid, title, source in finished:
                    await self.hub.push_finished(sid, title, source)
            except Exception as exc:  # noqa: BLE001
                self.log.debug("Codex scan failed: %s", exc)
            self._first = False
            await asyncio.sleep(self.poll_interval)

    def _day_dirs(self) -> list[Path]:
        today = datetime.now()
        out = []
        for d in (today - timedelta(days=1), today):
            p = self.root / f"{d:%Y}" / f"{d:%m}" / f"{d:%d}"
            if p.is_dir():
                out.append(p)
        return out

    def _scan(self) -> list[tuple[str, str, str]]:
        """Lê só o que foi acrescentado desde a última passada. Devolve os
        "terminou" novos (não no primeiro scan, para não falar o histórico)."""
        now = time.time()
        window = float(self.cfg.recent_hours) * 3600
        stale = float(self.cfg.stale_after_s)
        finished: list[tuple[str, str, str]] = []
        for day in self._day_dirs():
            for f in day.glob("rollout-*.jsonl"):
                try:
                    st = f.stat()
                except OSError:
                    continue
                if now - st.st_mtime > window:
                    continue
                key = str(f)
                tr = self._tracks.get(key)
                if tr is None:
                    tr = self._tracks[key] = _Track()
                if tr.skip:
                    continue
                if st.st_size > tr.offset:
                    self._read_new(f, tr, finished)
                if tr.skip or not tr.sid:
                    continue
                if tr.status == "working" and now - st.st_mtime > stale:
                    tr.status = "idle"
                    self.hub.state.set_session_status(tr.sid, "idle", "parada")
        return [] if self._first else finished

    def _read_new(self, f: Path, tr: _Track, finished: list) -> None:
        try:
            with open(f, "rb") as fh:
                fh.seek(tr.offset)
                data = fh.read()
        except OSError:
            return
        tr.offset += len(data)
        chunk = tr.rest + data
        lines = chunk.split(b"\n")
        tr.rest = lines.pop()  # linha incompleta fica para a próxima
        changed = False
        for raw in lines:
            if not raw.strip():
                continue
            try:
                rec = json.loads(raw)
            except ValueError:
                continue
            rtype = rec.get("type")
            p = rec.get("payload") or {}
            if rtype == "session_meta" and not tr.meta_done:
                tr.meta_done = True
                src = p.get("source")
                orig = str(p.get("originator") or "")
                if not isinstance(src, str) or orig not in ORIGINATORS:
                    tr.skip = True  # subagente, exec ou automação
                    return
                tr.source = ORIGINATORS[orig]
                tr.sid = "codex-" + str(p.get("id") or p.get("session_id") or f.stem)[-36:]
                tr.cwd = str(p.get("cwd") or "")
                changed = True
            elif not tr.meta_done:
                continue
            elif rtype == "response_item" and p.get("type") == "message" and p.get("role") == "user":
                if not tr.title:
                    tr.title = _user_text(p)[:MAX_TITLE_LEN]
                    changed = changed or bool(tr.title)
            elif rtype == "event_msg":
                et = p.get("type")
                if et == "task_started":
                    tr.status = "working"
                    changed = True
                elif et == "task_complete":
                    tr.status = "finished"
                    changed = True
                    finished.append((tr.sid, tr.title or self._profile(tr), tr.source))
                elif et == "turn_aborted":
                    tr.status = "idle"
                    changed = True
        if changed and tr.sid and tr.status:
            detail = {"working": "trabalhando", "finished": "turno concluido", "idle": "parada"}[tr.status]
            self.hub.upsert_session(Session(
                id=tr.sid,
                title=tr.title or self._profile(tr),
                source=tr.source,
                profile=self._profile(tr),
                status=tr.status,
                detail=detail,
            ))

    @staticmethod
    def _profile(tr: _Track) -> str:
        return (os.path.basename(tr.cwd.rstrip("/\\")) or "codex")[-40:]
