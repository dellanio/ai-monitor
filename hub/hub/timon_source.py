"""Fonte Timon: missões dos agentes do Timon (orquestrador local).

Lê ``GET /api/missions`` do daemon do Timon (padrão ``127.0.0.1:3010``) a
cada ``poll_interval_s`` e converte em sessões do protocolo:

* ``pending_action_at`` preenchido  -> ``needs-you`` (a missão espera você)
* ``turn_active`` ou ``running``     -> ``working`` (ou ``idle`` se o turno
  acabou e a missão segue viva esperando mensagem)
* ``completed``/``failed`` recentes  -> ``finished`` (por ``finished_keep_s``)
* interrompidas/antigas              -> somem da tela

Somente leitura e somente loopback: o token padrão (``~/.timon/daemon-token``)
é a credencial de operador do Timon; o Hub só faz GET e recusa mandar o token
para fora da máquina.
"""
from __future__ import annotations

import asyncio
import time
from urllib.parse import urlparse

import aiohttp

from .config import resolve_secret
from .sources import Source
from .state import Session

PREFIX = "timon:"
_LOOPBACK = {"127.0.0.1", "localhost", "::1", "[::1]"}


def mission_to_session(m: dict, now_ms: float, finished_keep_s: float) -> Session | None:
    """Converte uma missão do Timon em Session (ou None = não mostrar)."""
    mid = m.get("id")
    if not mid:
        return None
    status = str(m.get("status") or "")
    title = (m.get("title") or m.get("task_title") or m.get("mission") or "missão").strip()
    title = " ".join(title.split())[:80]
    agent = m.get("agent_name") or "timon"
    model = m.get("model") or ""
    role = m.get("agent_role") or m.get("task_type") or ""
    base = " · ".join(x for x in (role, model) if x)

    if m.get("pending_action_at"):
        st, detail = "needs-you", "esperando sua resposta no Timon"
    elif m.get("turn_active") or (m.get("running") and not m.get("turn_complete")):
        tools = m.get("span_tools") or 0
        st, detail = "working", (f"{tools} ferramentas" + (f" · {base}" if base else "")) if tools else (base or "trabalhando")
    elif m.get("running"):
        st, detail = "idle", "aguardando mensagem"
    elif status in ("completed", "failed"):
        ended = m.get("ended_at") or m.get("last_event_at") or 0
        if not ended or (now_ms - float(ended)) / 1000.0 > finished_keep_s:
            return None
        st = "finished"
        detail = "concluída" if status == "completed" else f"falhou (código {m.get('exit_code')})"
    else:
        return None  # interrompida, cancelada ou desconhecida

    cost = m.get("total_cost_usd")
    return Session(
        id=PREFIX + str(mid),
        title=title,
        source="timon",
        profile=agent,
        status=st,
        detail=detail[:120],
        input_tokens=int(m.get("tokens_in") or 0),
        output_tokens=int(m.get("tokens_out") or 0),
        cost_usd=float(cost) if cost is not None else None,
    )


class TimonSource(Source):
    name = "timon"

    def __init__(self, hub, cfg) -> None:
        super().__init__(hub, cfg)
        self._task: asyncio.Task | None = None
        self._session: aiohttp.ClientSession | None = None
        self._token: str | None = None
        self._last_error: str | None = None
        self.polls = 0

    async def start(self) -> None:
        host = urlparse(self.cfg.api_url).hostname or ""
        if host not in _LOOPBACK:
            self.log.error("timon.api_url precisa ser loopback (o token é de operador): %s", self.cfg.api_url)
            return
        try:
            self._token = resolve_secret(self.cfg.token)
        except OSError as exc:
            self.log.warning("token do Timon indisponível (%s); fonte Timon desligada", exc)
            return
        self._session = aiohttp.ClientSession(timeout=aiohttp.ClientTimeout(total=8))
        self._task = asyncio.create_task(self._loop())

    async def stop(self) -> None:
        if self._task is not None:
            self._task.cancel()
            try:
                await self._task
            except (asyncio.CancelledError, Exception):
                pass
            self._task = None
        if self._session is not None:
            await self._session.close()
            self._session = None

    async def _fetch(self) -> list[dict] | None:
        assert self._session is not None
        headers = {"Authorization": f"Bearer {self._token}"} if self._token else {}
        url = self.cfg.api_url.rstrip("/") + "/api/missions"
        try:
            async with self._session.get(url, headers=headers) as resp:
                if resp.status != 200:
                    self._warn_once(f"HTTP {resp.status} em /api/missions")
                    return None
                data = await resp.json(content_type=None)
        except (aiohttp.ClientError, asyncio.TimeoutError, ValueError) as exc:
            self._warn_once(f"Timon fora do ar: {exc.__class__.__name__}")
            return None
        self._last_error = None
        return data.get("missions") if isinstance(data, dict) else None

    def _warn_once(self, msg: str) -> None:
        if msg != self._last_error:
            self.log.warning(msg)
            self._last_error = msg

    async def _loop(self) -> None:
        while True:
            missions = await self._fetch()
            if missions is not None:
                await self._apply(missions)
            self.polls += 1
            await asyncio.sleep(self.cfg.poll_interval_s)

    async def _apply(self, missions: list[dict]) -> None:
        now_ms = time.time() * 1000
        state = self.hub.state
        seen: set[str] = set()
        finished_now: list[Session] = []
        for m in missions:
            s = mission_to_session(m, now_ms, self.cfg.finished_keep_s)
            if s is None:
                continue
            seen.add(s.id)
            old = state.sessions.get(s.id)
            if old and old.status == s.status and old.detail == s.detail and old.title == s.title:
                continue
            if s.status == "finished" and old is not None and old.status != "finished":
                finished_now.append(s)
            self.hub.upsert_session(s)
        gone = [sid for sid, s in state.sessions.items() if s.source == "timon" and sid not in seen]
        for sid in gone:
            state.remove_session(sid)
        for s in finished_now:
            await self.hub.push_finished(s.id, s.title)
