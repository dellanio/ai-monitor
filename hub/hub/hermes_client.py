"""Hermes source (priority 1).

Talks to the Hermes dashboard (``hermes dashboard``, default
``127.0.0.1:9119``) exactly as mapped in ``../hermes-api-map.md``:

* WebSocket ``/api/ws`` (JSON-RPC 2.0) — live sessions, events, approvals.
* REST — session lists, cron jobs, status.
* ``state.db`` is NOT read here; REST already covers it (map §7).

Approval flow (map §4): the server sends a server->client request
``{"method": "approval", "id": "srq-...", "params": {...}}``; the Hub
forwards it to the device and answers the same ``id`` with
``{"result": {"choice": ...}}`` when the user taps approve/deny.
On (re)connect the Hub replays open requests via ``session.events.since``
so an approval never silently disappears from the screen.
"""
from __future__ import annotations

import asyncio
import json
import time

import aiohttp
from websockets.asyncio.client import connect as ws_connect
from websockets.exceptions import ConnectionClosed

from .sources import Source
from .state import HERMES_STATUS_MAP, PendingApproval, Session

# Methods the Hub calls that are not part of the documented monitor set.
_KNOWN_METHODS = {
    "client.capabilities",
    "session.active_list",
    "session.events.since",
    "session.resume",
    "session.usage",
    "usage.bars",
    "prompt.submit",
    "gateway.ping",
}


class HermesClient(Source):
    name = "hermes"

    def __init__(self, hub, cfg) -> None:
        super().__init__(hub, cfg)
        self.ws_url = cfg.ws_url
        self.rest_base = cfg.rest_base.rstrip("/")
        self.token = self._resolve_token(cfg.token)
        self.reconnect_delay = float(cfg.reconnect_delay_s)
        self.ping_interval = float(cfg.ping_interval_s)
        self.cron_refresh = float(cfg.cron_refresh_s)
        self._ws = None
        self._reader_task: asyncio.Task | None = None
        self._ping_task: asyncio.Task | None = None
        self._cron_task: asyncio.Task | None = None
        self._run_task: asyncio.Task | None = None
        self._stop = False
        self._rpc_id = 0
        self._pending_rpc: dict = {}
        self._ping_failures = 0
        self._session = aiohttp.ClientSession()
        self._disabled = False  # set when the dashboard demands auth we lack
        self._known_sessions: set[str] = set()
        self._activity_task: asyncio.Task | None = None
        # sid -> (message_count, last_active, status) do último poll
        self._activity: dict[str, tuple] = {}
        self._activity_primed = False
        self.activity_poll = float(getattr(cfg, "activity_poll_s", 8) or 8)
        self.finish_skip_sources = set(getattr(cfg, "finish_skip_sources", None) or ("oneshot", "acp", "cron"))

    # -- lifecycle -------------------------------------------------------
    async def start(self) -> None:
        self._run_task = asyncio.create_task(self._run_loop())

    async def stop(self) -> None:
        self._stop = True
        if self._activity_task:
            self._activity_task.cancel()
        if self._run_task:
            self._run_task.cancel()
            try:
                await self._run_task
            except asyncio.CancelledError:
                pass
        await self._session.close()

    @staticmethod
    def _resolve_token(token: str | None) -> str | None:
        """Accept a literal token or ``file:<path>`` (same convention as
        HERMES_DASHBOARD_SESSION_TOKEN)."""
        if not token:
            return None
        if token.startswith("file:"):
            from pathlib import Path

            p = Path(token[5:]).expanduser()
            if not p.exists():
                return None  # sem arquivo = fonte Hermes desligada
            return p.read_text(encoding="utf-8").strip()
        return token

    # -- REST ------------------------------------------------------------
    def _rest_headers(self) -> dict:
        if self.token:
            return {"Authorization": f"Bearer {self.token}"}
        return {}

    async def _rest_get(self, path: str):
        url = f"{self.rest_base}{path}"
        try:
            async with self._session.get(url, headers=self._rest_headers(), timeout=aiohttp.ClientTimeout(total=10)) as resp:
                if resp.status != 200:
                    self.log.debug("REST %s -> HTTP %s", path, resp.status)
                    return None
                return await resp.json()
        except Exception as exc:
            self.log.debug("REST %s failed: %s", path, exc)
            return None

    async def _safety_check(self) -> bool:
        """Verify the dashboard actually answers the routes the Hub needs.

        LIVE FINDING (2026-11-21, dashboard 0.22.7): ``/api/status`` says
        ``auth_required: false`` on loopback, but every non-public ``/api/``
        route (sessions, cron) returns 401 and ``/api/ws`` rejects the
        handshake with 403 unless the request carries the dashboard's
        per-process session token. The token is minted fresh at each
        dashboard start and is only reachable if the dashboard was launched
        with a known ``HERMES_DASHBOARD_SESSION_TOKEN``. So the real check
        is a probe of a sensitive route, not ``/api/status``.
        """
        probe = await self._rest_get("/api/profiles/sessions?limit=1")
        if probe is not None:
            return True
        # Distinguish "dashboard down" (retry later) from "token missing"
        # (retrying will never help).
        status = await self._rest_status("/api/profiles/sessions?limit=1")
        if status == 401:
            if self.token:
                self.log.error(
                    "dashboard answered 401 even with a token configured; "
                    "the token is wrong or stale (it changes on every "
                    "dashboard restart unless HERMES_DASHBOARD_SESSION_TOKEN "
                    "is set). Hermes source disabled for this run."
                )
            else:
                self.log.error(
                    "dashboard requires its session token (HTTP 401) and no "
                    "token is configured. Start the dashboard with a known "
                    "token, e.g. HERMES_DASHBOARD_SESSION_TOKEN=<token> "
                    "hermes dashboard, and set the same value in "
                    "config.yaml under hermes.token (or hermes.token: "
                    "file:<path>). Hermes source disabled for this run."
                )
            self._disabled = True
            return False
        if status is None:
            self.log.warning("dashboard unreachable; continuing on WS only (will retry)")
        return True

    async def _rest_status(self, path: str):
        url = f"{self.rest_base}{path}"
        try:
            async with self._session.get(url, headers=self._rest_headers(), timeout=aiohttp.ClientTimeout(total=10)) as resp:
                return resp.status
        except Exception:
            return None

    # -- connection loop -------------------------------------------------
    async def _run_loop(self) -> None:
        if not await self._safety_check():
            return
        while not self._stop and not self._disabled:
            try:
                await self._connect_and_serve()
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                if "403" in str(exc) or "Forbidden" in str(exc):
                    self.log.error(
                        "dashboard rejected the WS handshake (403): the session "
                        "token is missing or wrong. Start the dashboard with "
                        "HERMES_DASHBOARD_SESSION_TOKEN set and configure the "
                        "same token as hermes.token."
                    )
                else:
                    self.log.warning("Hermes connection lost: %s", exc)
            if not self._stop:
                await asyncio.sleep(self.reconnect_delay)

    async def _connect_and_serve(self) -> None:
        url = self.ws_url
        if self.token and "token=" not in url:
            sep = "&" if "?" in url else "?"
            url = f"{url}{sep}token={self.token}"
        self.log.info("connecting to Hermes dashboard WS %s", self.ws_url)
        async with ws_connect(url, max_size=8 * 1024 * 1024) as ws:
            self._ws = ws
            self._pending_rpc = {}
            try:
                self._reader_task = asyncio.create_task(self._read_loop(ws))
                await self._rpc("client.capabilities", {"server_requests": True})
                await self._initial_sync()
                self._ping_task = asyncio.create_task(self._ping_loop())
                self._cron_task = asyncio.create_task(self._cron_loop())
                if self._activity_task is None or self._activity_task.done():
                    self._activity_task = asyncio.create_task(self._activity_loop())
                await self._reader_task
            finally:
                for task in (self._ping_task, self._cron_task):
                    if task:
                        task.cancel()
                self._fail_pending_rpcs(ConnectionError("Hermes WS closed"))
                self._ws = None
                self._ping_task = None
                self._cron_task = None

    def _fail_pending_rpcs(self, exc: Exception) -> None:
        for fut in list(self._pending_rpc.values()):
            if not fut.done():
                fut.set_exception(exc)
        self._pending_rpc.clear()

    # -- JSON-RPC --------------------------------------------------------
    async def _rpc(self, method: str, params: dict | None = None, timeout: float = 20.0):
        if self._ws is None:
            raise ConnectionError("Hermes WS not connected")
        self._rpc_id += 1
        rid = self._rpc_id
        loop = asyncio.get_running_loop()
        fut = loop.create_future()
        self._pending_rpc[rid] = fut
        await self._ws.send(json.dumps({"jsonrpc": "2.0", "id": rid, "method": method, "params": params or {}}))
        try:
            return await asyncio.wait_for(fut, timeout)
        finally:
            self._pending_rpc.pop(rid, None)

    async def _ping_loop(self) -> None:
        while True:
            await asyncio.sleep(self.ping_interval)
            try:
                await self._rpc("gateway.ping", {}, timeout=10)
                self._ping_failures = 0
            except Exception:
                self._ping_failures += 1
                self.log.warning("gateway.ping failed (%d in a row)", self._ping_failures)
                if self._ping_failures >= 3 and self._ws is not None:
                    await self._ws.close()
                    return

    async def _cron_loop(self) -> None:
        while True:
            await asyncio.sleep(self.cron_refresh)
            await self._refresh_cron()

    # -- atividade de todas as sessões (Telegram, desktop, CLI...) -------
    # O WS do painel só manda eventos das sessões abertas pelo próprio socket;
    # as demais são acompanhadas pelo REST: quando a última mensagem vira
    # resposta final do assistente, a sessão "terminou" (plim no robô).
    async def _activity_loop(self) -> None:
        while not self._stop:
            try:
                await self._poll_activity()
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                self.log.debug("activity poll failed: %s", exc)
            await asyncio.sleep(self.activity_poll)

    @staticmethod
    def classify_tail(msg: dict | None) -> tuple[str, str]:
        """Estado da sessão pela última mensagem gravada."""
        if not msg:
            return "idle", ""
        role = msg.get("role")
        calls = msg.get("tool_calls") or []
        if role == "assistant" and not calls:
            return "finished", ""
        if role == "assistant" and calls:
            names = []
            for c in calls if isinstance(calls, list) else []:
                fn = (c or {}).get("function") or {}
                names.append(fn.get("name") or (c or {}).get("name") or "")
            if "clarify" in names:
                return "needs-you", "pergunta para voce"
            return "working", f"tool: {names[0]}" if names and names[0] else "trabalhando"
        return "working", "trabalhando"

    async def _poll_activity(self) -> None:
        data = await self._rest_get("/api/sessions?limit=20")
        rows = (data or {}).get("sessions") if isinstance(data, dict) else None
        if not isinstance(rows, list):
            return
        primed = self._activity_primed
        for row in rows:
            sid = row.get("id")
            if not sid:
                continue
            key = (row.get("message_count"), row.get("last_active"))
            prev = self._activity.get(sid)
            if prev is not None and prev[:2] == key:
                continue
            if prev is None and row.get("ended_at") and primed is False:
                self._activity[sid] = (*key, "idle")
                continue
            tail = await self._rest_get(
                f"/api/sessions/{sid}/messages?limit=1&order=latest&inline_images=false")
            msgs = (tail or {}).get("messages") if isinstance(tail, dict) else None
            status, detail = self.classify_tail(msgs[-1] if msgs else None)
            old = prev[2] if prev else None
            self._activity[sid] = (*key, status)
            if row.get("ended_at") and prev is None and status != "finished":
                continue
            src = row.get("source") or ""
            title = (row.get("title") or row.get("preview") or sid).strip().replace("\n", " ")[:80]
            s = self.hub.state.sessions.get(sid)
            if s is None:
                if row.get("ended_at") and not primed:
                    continue
                self._known_sessions.add(sid)
                self.hub.upsert_session(Session(
                    id=sid, title=title, source="hermes",
                    profile=row.get("profile_name") or row.get("profile"),
                    status=status if status != "finished" else "idle", detail=detail,
                    input_tokens=int(row.get("input_tokens") or 0),
                    output_tokens=int(row.get("output_tokens") or 0),
                    cost_usd=row.get("estimated_cost_usd")))
            else:
                if status != "finished":
                    self.hub.state.set_session_status(sid, status, detail)
            if (status == "finished" and primed and old != "finished" and src not in self.finish_skip_sources
                    and (s is None or s.status != "finished")):
                self.hub.state.set_session_status(sid, "finished")
                await self.hub.push_finished(sid, title)
                self.log.info("hermes: sessao terminou: %s (%s)", title, src)
            elif status == "finished" and s is not None and s.status != "finished":
                self.hub.state.set_session_status(sid, "idle")
        if self._activity_primed:
            await self.hub.push_sessions()
        self._activity_primed = True

    # -- sync ------------------------------------------------------------
    async def _initial_sync(self) -> None:
        await self._refresh_active_sessions()
        await self._refresh_rest_sessions()
        await self._refresh_cron()
        await self._refresh_usage_bars()
        await self._recover_open_requests()

    async def _refresh_active_sessions(self) -> None:
        try:
            result = await self._rpc("session.active_list", {})
        except Exception as exc:
            self.log.warning("session.active_list failed: %s", exc)
            return
        live_ids = set()
        for item in (result or {}).get("sessions", []):
            sid = item.get("id")
            if not sid:
                continue
            live_ids.add(sid)
            self._known_sessions.add(sid)
            self.hub.upsert_session(
                Session(
                    id=sid,
                    title=item.get("title") or item.get("preview") or sid,
                    source="hermes",
                    profile=item.get("profile"),
                    status=HERMES_STATUS_MAP.get(item.get("status", "working"), "working"),
                    detail=self._detail_from_active(item),
                    input_tokens=0,
                    output_tokens=0,
                )
            )
        self._live_ids = live_ids

    @staticmethod
    def _detail_from_active(item: dict) -> str:
        preview = (item.get("preview") or "").strip().replace("\n", " ")
        return preview[:120]

    async def _refresh_rest_sessions(self) -> None:
        data = await self._rest_get("/api/profiles/sessions?order=recent&limit=20")
        if not isinstance(data, dict):
            return
        live = getattr(self, "_live_ids", set())
        for row in data.get("sessions", []):
            sid = row.get("id")
            if not sid or row.get("ended_at"):
                continue  # history only; live state comes from the WS
            if sid in live:
                # Enrich the live session with stored title/cost.
                s = self.hub.state.sessions.get(sid)
                if s is not None:
                    if row.get("title"):
                        s.title = row["title"]
                    if row.get("estimated_cost_usd") is not None:
                        s.cost_usd = row["estimated_cost_usd"]
                    s.input_tokens = int(row.get("input_tokens") or 0)
                    s.output_tokens = int(row.get("output_tokens") or 0)
                    self.hub.state.touch()
                continue
            self._known_sessions.add(sid)
            self.hub.upsert_session(
                Session(
                    id=sid,
                    title=row.get("title") or sid,
                    source="hermes",
                    profile=row.get("profile_name") or row.get("profile"),
                    status="idle",
                    detail=(row.get("last_activity_description") or "")[:120],
                    input_tokens=int(row.get("input_tokens") or 0),
                    output_tokens=int(row.get("output_tokens") or 0),
                    cost_usd=row.get("estimated_cost_usd"),
                )
            )

    async def _refresh_cron(self) -> None:
        data = await self._rest_get("/api/cron/jobs?profile=all")
        if data is None:
            return
        # Live: the endpoint returns a bare list []; the map showed {"jobs": [...]}.
        if isinstance(data, list):
            jobs = data
        else:
            jobs = data.get("jobs", [])
        cleaned = []
        for j in jobs:
            cleaned.append(
                {
                    "id": j.get("id", ""),
                    "name": j.get("name", j.get("id", "")),
                    "next_run_at": j.get("next_run_at"),
                    "last_status": j.get("last_status"),
                    "timezone": j.get("timezone"),
                }
            )
        self.hub.state.cron_jobs = cleaned
        self.hub.state.touch()

    async def _refresh_usage_bars(self) -> None:
        try:
            result = await self._rpc("usage.bars", {})
        except Exception:
            return
        if not result or not result.get("available"):
            return  # provider without billing: hide the limits section
        limits = {}
        for key in ("plan_name", "renews_at", "renews_display", "subscription_remaining_display"):
            if result.get(key) is not None:
                limits[key] = result[key]
        if limits:
            self.hub.state.limits = limits
            self.hub.state.touch()

    async def _recover_open_requests(self) -> None:
        """Re-display approvals that were open when we (re)connected (map §4.3)."""
        for sid in list(self._known_sessions):
            open_requests = None
            try:
                result = await self._rpc("session.events.since", {"session_id": sid})
                open_requests = (result or {}).get("open_requests")
            except Exception:
                try:
                    snapshot = await self._rpc("session.resume", {"session_id": sid})
                    open_requests = (snapshot or {}).get("open_requests")
                except Exception:
                    continue
            for req in open_requests or []:
                if req.get("method") == "approval":
                    await self._register_approval_request(req.get("id", ""), req.get("params", {}))

    # -- read loop -------------------------------------------------------
    async def _read_loop(self, ws) -> None:
        try:
            async for raw in ws:
                try:
                    msg = json.loads(raw)
                except (ValueError, TypeError):
                    continue
                await self._handle_frame(msg)
        except ConnectionClosed:
            # Normal path: server closed the socket; return so the caller
            # reconnects. (Do not re-raise: nobody retrieves this task.)
            self.log.info("Hermes WS closed")
            return
        except asyncio.CancelledError:
            raise

    async def _handle_frame(self, msg: dict) -> None:
        method = msg.get("method")
        if method == "event":
            await self._handle_event(msg.get("params", {}))
            return
        if method in ("approval", "clarify") and isinstance(msg.get("id"), str):
            await self._register_approval_request(msg["id"], msg.get("params", {}), kind=method)
            return
        if "id" in msg and ("result" in msg or "error" in msg):
            rid = msg["id"]
            fut = self._pending_rpc.get(rid)
            if fut is not None and not fut.done():
                if "error" in msg:
                    fut.set_exception(RuntimeError(f"Hermes RPC error: {msg['error']}"))
                else:
                    fut.set_result(msg.get("result"))
            return
        if method:
            self.log.debug("unhandled Hermes frame method: %s", method)

    async def _handle_event(self, params: dict) -> None:
        etype = params.get("type")
        sid = params.get("session_id")
        payload = params.get("payload", {}) or {}

        if etype == "message.complete":
            if sid:
                self.hub.state.set_session_status(sid, "finished")
                s = self.hub.state.sessions.get(sid)
                await self.hub.push_finished(sid, s.title if s else sid)
                await self.hub.push_sessions()
        elif etype == "tool.start":
            if sid:
                name = payload.get("name") or payload.get("tool") or ""
                self.hub.state.set_session_status(sid, "working", f"tool: {name}"[:120])
        elif etype == "status.update":
            if sid and payload.get("text"):
                self.hub.state.set_session_status(sid, "working", str(payload["text"])[:120])
        elif etype == "session.usage":
            if sid:
                s = self.hub.state.sessions.get(sid)
                if s is not None:
                    s.input_tokens = int(payload.get("input_tokens") or s.input_tokens)
                    s.output_tokens = int(payload.get("output_tokens") or s.output_tokens)
                    self.hub.state.touch()
        elif etype in ("approval.cancelled", "request.cancel"):
            req_id = payload.get("request_id") or payload.get("id")
            if req_id:
                self.hub.state.remove_approval(req_id)
                await self.hub.device.push_approval_cancelled(req_id)
        elif etype == "sessions.changed":
            await self._refresh_rest_sessions()
        elif etype == "cron.changed":
            await self._refresh_cron()
        elif etype == "session.title":
            if sid and payload.get("title"):
                s = self.hub.state.sessions.get(sid)
                if s is not None:
                    s.title = payload["title"]
                    self.hub.state.touch()
        elif etype == "session.info":
            if sid and payload.get("status"):
                self.hub.state.set_session_status(
                    sid, HERMES_STATUS_MAP.get(payload["status"], "working")
                )

    # -- approvals -------------------------------------------------------
    async def _register_approval_request(self, req_id: str, params: dict, kind: str = "approval") -> None:
        if not req_id:
            return
        if req_id in self.hub.state.pending:
            # Replay after a reconnect: re-display the open approval so it
            # never silently disappears from the screen (map §4.3).
            ap = self.hub.state.pending[req_id]
            if ap.kind == "approval":
                await self.hub.device.push_approval(ap)
            return
        if kind == "clarify":
            # v0.1: a clarify is a question, not a command approval — the
            # device has no keyboard to answer it. Show it as needs-you in
            # the session list; the user answers in the terminal.
            sid = params.get("session_id", "")
            question = params.get("question") or params.get("text") or "pergunta do agente"
            self.hub.state.set_session_status(sid, "needs-you", f"pergunta: {str(question)[:100]}")
            self.log.info("clarify request %s shown as needs-you (answer in terminal)", req_id)
            return
        ap = PendingApproval(
            request_id=req_id,
            session_id=params.get("session_id", ""),
            command=params.get("command", ""),
            description=params.get("description", ""),
            choices=params.get("choices") or ["once", "deny"],
            source="hermes",
            kind="approval",
        )
        self.hub.state.add_approval(ap)
        self.hub.state.set_session_status(ap.session_id, "needs-you", f"pedindo permissao: {ap.command}"[:120])
        await self.hub.device.push_approval(ap)
        self.log.info("approval %s from Hermes: %s", req_id, ap.command)

    async def respond_approval(self, request_id: str, choice: str) -> None:
        """Answer a server->client approval request (map §4.2 step 5)."""
        if self._ws is None:
            self.log.warning("cannot answer approval %s: WS not connected", request_id)
            return
        await self._ws.send(
            json.dumps({"jsonrpc": "2.0", "id": request_id, "result": {"choice": choice}})
        )
        self.log.info("answered approval %s with %s", request_id, choice)

    async def prompt_submit(self, session_id: str, text: str) -> None:
        try:
            await self._rpc("prompt.submit", {"session_id": session_id, "text": text}, timeout=20)
        except Exception as exc:
            self.log.warning("prompt.submit failed: %s", exc)
