"""Claude Code source (priority 2).

Two mechanisms, hooks first, files second:

1. **Hooks HTTP endpoint** (``POST /hooks/claude``): the authoritative path.
   ``hooks/claude-hook.py`` is registered in ``~/.claude/settings.json`` as a
   PreToolUse / Stop / SessionStart / Notification hook. A PreToolUse call
   BLOCKS inside the Hub until the user answers on the device (or the
   approval timeout fires) and returns ``{"decision": "allow"|"deny"}``;
   the hook script turns that into Claude's ``permissionDecision``.
   Fail-closed: timeout, Hub down, or no device answer all mean ``deny``.
2. **JSONL watcher**: polls ``~/.claude/projects/*/*.jsonl`` for the session
   list (title from the first user message, working/finished from the last
   record). Heuristic only — hooks are authoritative.
"""
from __future__ import annotations

import asyncio
import hashlib
import json
import os
import time
from pathlib import Path

from aiohttp import web

from .sources import Source
from .state import PendingApproval, Session

MAX_TITLE_LEN = 60


def _text_of_content(content) -> str:
    """Extract readable text from a Claude message content field."""
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        parts = []
        for block in content:
            if isinstance(block, dict) and block.get("type") == "text":
                parts.append(block.get("text", ""))
        return "\n".join(parts)
    return ""


def parse_session_file(path: Path) -> dict | None:
    """Read one Claude Code session JSONL. Returns {title, status, detail}."""
    title = ""
    last_kind = None  # "user" | "assistant-text" | "assistant-tool" | "end_turn"
    last_tool = ""
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    rec = json.loads(line)
                except ValueError:
                    continue
                if rec.get("isSidechain"):
                    continue
                rtype = rec.get("type")
                if rtype == "user":
                    if not title:
                        title = _text_of_content(rec.get("message", {}).get("content", ""))
                    last_kind = "user"
                elif rtype == "assistant":
                    msg = rec.get("message", {}) or {}
                    content = msg.get("content", [])
                    has_tool = any(
                        isinstance(b, dict) and b.get("type") == "tool_use" for b in content
                    ) if isinstance(content, list) else False
                    if has_tool:
                        tool_block = next(
                            (b for b in content if isinstance(b, dict) and b.get("type") == "tool_use"),
                            {},
                        )
                        last_tool = str(tool_block.get("name", ""))
                        last_kind = "assistant-tool"
                    elif msg.get("stop_reason") == "end_turn":
                        last_kind = "end_turn"
                    else:
                        last_kind = "assistant-text"
    except OSError:
        return None
    if not title and not last_kind:
        return None
    status = "working"
    detail = ""
    if last_kind == "end_turn":
        status = "finished"
    elif last_kind == "assistant-tool" and last_tool:
        detail = f"tool: {last_tool}"
    elif last_kind == "user":
        detail = "nova pergunta"
    title = " ".join((title or path.stem).split())[:MAX_TITLE_LEN] or path.stem
    return {"title": title, "status": status, "detail": detail}


class ClaudeSource(Source):
    name = "claude"

    def __init__(self, hub, cfg) -> None:
        super().__init__(hub, cfg)
        self.root = Path(os.path.expanduser(cfg.projects_root))
        self.poll_interval = float(cfg.poll_interval_s)
        self.hook_host = cfg.hook_host
        self.hook_port = int(cfg.hook_port)
        self._watch_task: asyncio.Task | None = None
        self._runner: web.AppRunner | None = None
        self._site: web.TCPSite | None = None
        self.actual_hook_port = self.hook_port
        self._mtimes: dict[str, float] = {}

    async def start(self) -> None:
        self._watch_task = asyncio.create_task(self._watch_loop())
        await self._start_hook_server()

    async def stop(self) -> None:
        if self._watch_task:
            self._watch_task.cancel()
            try:
                await self._watch_task
            except asyncio.CancelledError:
                pass
        if self._site:
            await self._site.stop()
        if self._runner:
            await self._runner.cleanup()

    # -- JSONL watcher ---------------------------------------------------
    async def _watch_loop(self) -> None:
        while True:
            try:
                self._scan()
            except Exception as exc:
                self.log.debug("Claude scan failed: %s", exc)
            await asyncio.sleep(self.poll_interval)

    @staticmethod
    def _project_name(dirname: str) -> str:
        # "D--projetos-pessoal-dell-monitor-esp32" -> "dell-monitor-esp32"
        parts = [p for p in dirname.split("--") if p]
        return (parts[-1] if parts else dirname)[-40:]

    def _scan(self) -> None:
        """Só sessões mexidas nas últimas ``recent_hours``; arquivo sem mudança
        não é relido. Parado há mais de ``stale_after_s`` sem fim de turno
        vira "idle" (sessão abandonada não fica "trabalhando" para sempre)."""
        if not self.root.is_dir():
            return
        now = time.time()
        window = float(getattr(self.cfg, "recent_hours", 6.0)) * 3600
        stale = float(getattr(self.cfg, "stale_after_s", 600.0))
        seen: set[str] = set()
        for project_dir in self.root.iterdir():
            if not project_dir.is_dir():
                continue
            for f in project_dir.glob("*.jsonl"):
                try:
                    mtime = f.stat().st_mtime
                except OSError:
                    continue
                if now - mtime > window:
                    continue
                sid = f.stem
                seen.add(sid)
                existing = self.hub.state.sessions.get(sid)
                cached = self._mtimes.get(sid)
                if cached == mtime and existing is not None:
                    if existing.status == "working" and now - mtime > stale:
                        self.hub.state.set_session_status(sid, "idle", "parada")
                    continue
                self._mtimes[sid] = mtime
                parsed = parse_session_file(f)
                if parsed is None:
                    continue
                status, detail = parsed["status"], parsed["detail"]
                if status == "working" and now - mtime > stale:
                    status, detail = "idle", "parada"
                # A finished session stays finished; a new user message reopens it.
                if existing is not None and existing.source == "claude":
                    if existing.status == "finished" and status == "finished":
                        continue
                self.hub.upsert_session(
                    Session(
                        id=sid,
                        title=parsed["title"],
                        source="claude",
                        profile=self._project_name(project_dir.name),
                        status=status,
                        detail=detail,
                    )
                )
        for sid in [s.id for s in self.hub.state.sessions.values()
                    if s.source == "claude" and s.id not in seen and s.status != "needs-you"]:
            self._mtimes.pop(sid, None)
            self.hub.state.remove_session(sid)

    # -- hooks HTTP endpoint ---------------------------------------------
    async def _start_hook_server(self) -> None:
        app = web.Application()
        app.router.add_post("/hooks/claude", self._handle_hook)
        self._runner = web.AppRunner(app)
        await self._runner.setup()
        self._site = web.TCPSite(self._runner, self.hook_host, self.hook_port)
        await self._site.start()
        if self._site._server and self._site._server.sockets:
            self.actual_hook_port = self._site._server.sockets[0].getsockname()[1]
        self.log.info(
            "Claude hooks endpoint: http://%s:%s/hooks/claude",
            self.hook_host,
            self.actual_hook_port,
        )

    async def _handle_hook(self, request: web.Request) -> web.Response:
        try:
            payload = await request.json()
        except Exception:
            return web.json_response({"decision": "ok"}, status=400)
        event = payload.get("hook_event_name", "")
        session_id = payload.get("session_id", "") or self._session_id_from_transcript(payload)
        if event == "PreToolUse":
            # Só status: NUNCA bloqueia nem decide (dispara em toda ferramenta).
            if session_id:
                self._ensure_session(payload, session_id)
                tool = str(payload.get("tool_name", "ferramenta"))
                self.hub.state.set_session_status(session_id, "working", f"usando {tool}"[:120])
            return web.json_response({"decision": "none"})
        if event == "PermissionRequest":
            return await self._handle_permission_request(payload, session_id)
        if event == "Stop":
            if session_id:
                self.hub.state.set_session_status(session_id, "finished")
                s = self.hub.state.sessions.get(session_id)
                await self.hub.push_finished(session_id, s.title if s else session_id)
                await self.hub.push_sessions()
            return web.json_response({"decision": "ok"})
        if event == "SessionStart":
            if session_id:
                cwd = payload.get("cwd", "")
                self.hub.upsert_session(
                    Session(
                        id=session_id,
                        title=os.path.basename(cwd.rstrip("/")) or session_id,
                        source="claude",
                        profile=os.path.basename(cwd.rstrip("/")) or None,
                        status="working",
                        detail="sessao iniciada",
                    )
                )
            return web.json_response({"decision": "ok"})
        if event == "SessionEnd":
            if session_id:
                self.hub.state.set_session_status(session_id, "finished")
            return web.json_response({"decision": "ok"})
        if event == "Notification":
            # permission_prompt: the decision path is the PreToolUse hook
            # (or the terminal). Just flag the session as needs-you.
            if payload.get("notification_type") == "permission_prompt" and session_id:
                self.hub.state.set_session_status(
                    session_id, "needs-you", str(payload.get("message", ""))[:120]
                )
            return web.json_response({"decision": "ok"})
        return web.json_response({"decision": "ok"})

    def _ensure_session(self, payload: dict, session_id: str) -> None:
        """Hooks podem chegar antes do watcher achar o JSONL: cria a sessão."""
        if session_id in self.hub.state.sessions:
            return
        cwd = str(payload.get("cwd", "")).rstrip("/\\")
        name = os.path.basename(cwd) or session_id[:8]
        self.hub.upsert_session(Session(id=session_id, title=name, source="claude",
                                        profile=name, status="working", detail=""))

    @staticmethod
    def _session_id_from_transcript(payload: dict) -> str:
        tp = payload.get("transcript_path", "")
        if tp:
            return Path(tp).stem
        return ""

    async def _handle_permission_request(self, payload: dict, session_id: str) -> web.Response:
        """Disparado só quando o Claude Code ia mesmo perguntar ao usuário.

        Com ``device.allow_actions`` desligado (padrão) apenas marca a sessão
        como "precisa de você" e devolve ``none``: o prompt normal do Claude
        Code continua valendo. Ligado, espera o toque no aparelho; sem
        resposta no prazo devolve ``none`` (o prompt do terminal segue).
        """
        tool_name = payload.get("tool_name", "tool")
        if session_id:
            self._ensure_session(payload, session_id)
        if not self.hub.config.device.allow_actions:
            if session_id:
                self.hub.state.set_session_status(
                    session_id, "needs-you", f"pedindo permissao: {tool_name}"[:120])
            return web.json_response({"decision": "none"})
        tool_input = payload.get("tool_input", {}) or {}
        try:
            input_summary = json.dumps(tool_input, ensure_ascii=False)[:300]
        except (TypeError, ValueError):
            input_summary = str(tool_input)[:300]
        dedupe_key = hashlib.sha256(
            f"{session_id}|{tool_name}|{input_summary}".encode()
        ).hexdigest()[:16]
        # Reuse an open approval for the same tool call (Notification +
        # PreToolUse can both fire for one permission prompt).
        for ap in self.hub.state.pending.values():
            if ap.source == "claude" and ap.dedupe_key == dedupe_key and ap.answer is not None:
                choice = await self._await_answer(ap)
                return web.json_response(self._decision_response(choice))

        loop = asyncio.get_running_loop()
        ap = PendingApproval(
            request_id=f"claude-{dedupe_key}-{int(time.time())}",
            session_id=session_id,
            source="claude",
            command=f"{tool_name}: {input_summary}",
            description=f"Claude Code quer usar {tool_name}",
            choices=["once", "deny"],
            kind="approval",
            answer=loop.create_future(),
            dedupe_key=dedupe_key,
        )
        self.hub.state.add_approval(ap)
        self.hub.state.set_session_status(session_id, "needs-you", f"pedindo permissao: {tool_name}"[:120])
        await self.hub.device.push_approval(ap)
        self.log.info("Claude approval %s: %s", ap.request_id, tool_name)
        choice = await self._await_answer(ap)
        return web.json_response(self._decision_response(choice))

    async def _await_answer(self, ap: PendingApproval) -> str:
        """Espera o toque; no timeout devolve "timeout" (o terminal decide)."""
        timeout = float(self.hub.config.approval.timeout_s)
        try:
            choice = await asyncio.wait_for(asyncio.shield(ap.answer), timeout)
            return str(choice)
        except (asyncio.TimeoutError, asyncio.CancelledError):
            self.log.warning("approval %s timed out after %ss -> terminal decides", ap.request_id, timeout)
            self.hub.state.remove_approval(ap.request_id)
            await self.hub.device.push_approval_cancelled(ap.request_id)
            return "timeout"

    @staticmethod
    def _decision_response(choice: str) -> dict:
        # once/session/always = deixar esta chamada rodar; deny bloqueia;
        # timeout/qualquer outra coisa = não decidir (prompt normal).
        if choice in ("once", "session", "always"):
            return {"decision": "allow", "choice": choice}
        if choice == "deny":
            return {"decision": "deny", "choice": choice}
        return {"decision": "none", "choice": choice}
