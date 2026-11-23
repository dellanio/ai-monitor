"""Shared Hub state: the unified session model and pending approvals.

Everything the device sees comes from here, in the exact shapes of
``firmware/PROTOCOL.md`` (v0.1). Sources (Hermes, Claude Code, ...) write
into this state; the device server reads from it and pushes snapshots.
"""
from __future__ import annotations

import asyncio
import time
from dataclasses import dataclass, field

PROTOCOL_STATUSES = ("working", "needs-you", "finished", "idle")


def device_text(s: str | None) -> str:
    """As fontes do firmware só têm ASCII: tira acentos e emojis
    ("Calçada 👋" -> "Calcada")."""
    import unicodedata

    if not s:
        return ""
    folded = unicodedata.normalize("NFKD", str(s)).encode("ascii", "ignore").decode("ascii")
    return " ".join(folded.split())

# Hermes live status -> protocol status (PROTOCOL.md, from hermes-api-map.md §3.2)
HERMES_STATUS_MAP = {
    "idle": "idle",
    "starting": "working",
    "working": "working",
    "streaming": "working",
    "resuming": "working",
    "waiting": "needs-you",
}


@dataclass
class Session:
    id: str
    title: str
    source: str  # "hermes" | "claude" | ...
    profile: str | None = None
    status: str = "working"
    detail: str = ""
    input_tokens: int = 0
    output_tokens: int = 0
    cost_usd: float | None = None
    updated_at: float = field(default_factory=time.time)

    def to_protocol(self) -> dict:
        d: dict = {
            "id": self.id,
            "title": device_text(self.title),
            "source": self.source,
            "status": self.status,
            "detail": device_text(self.detail),
        }
        if self.profile:
            d["profile"] = device_text(self.profile)
        if self.input_tokens:
            d["input_tokens"] = self.input_tokens
        if self.output_tokens:
            d["output_tokens"] = self.output_tokens
        if self.cost_usd is not None:
            d["cost_usd"] = round(self.cost_usd, 4)
        return d


@dataclass
class PendingApproval:
    request_id: str
    session_id: str
    source: str  # "hermes" | "claude"
    command: str
    description: str = ""
    choices: list = field(default_factory=lambda: ["once", "session", "always", "deny"])
    kind: str = "approval"  # "approval" (decision expected) | "clarify" (display-only)
    created_at: float = field(default_factory=time.time)
    # Resolved with the choice string ("once"|"session"|"always"|"deny")
    # by Hub.on_device_approve(). None when nobody awaits the answer.
    answer: asyncio.Future | None = None
    dedupe_key: str | None = None

    def to_protocol(self) -> dict:
        return {
            "type": "approval",
            "session_id": self.session_id,
            "request_id": self.request_id,
            "source": self.source,
            "command": device_text(self.command),
            "description": device_text(self.description),
            "choices": list(self.choices),
        }


class HubState:
    """Mutable state shared by sources and the device server."""

    def __init__(self) -> None:
        self.sessions: dict[str, Session] = {}
        self.pending: dict[str, PendingApproval] = {}
        self.limits: dict = {}
        self.cron_jobs: list[dict] = []
        # Host stats (PC running the AIs; None = unknown/unavailable).
        self.host_cpu_pct: int | None = None
        self.host_mem_pct: int | None = None
        self.host_gpu_pct: int | None = None
        self.host_top_name: str | None = None
        self.host_top_pct: int | None = None
        self.host_net_down_mbps: float | None = None
        self.host_net_up_mbps: float | None = None
        self.host_batt_pct: int | None = None
        self.host_batt_mins: int | None = None
        self.version = 0
        self._listeners: list = []

    # -- listeners -------------------------------------------------------
    def add_listener(self, fn) -> None:
        """fn() is called (sync) on every state change."""
        self._listeners.append(fn)

    def touch(self) -> None:
        self.version += 1
        for fn in list(self._listeners):
            try:
                fn()
            except Exception:
                pass

    # -- sessions --------------------------------------------------------
    def upsert_session(self, session: Session) -> None:
        old = self.sessions.get(session.id)
        if old is not None:
            # Keep the freshest tokens/cost when the update has none.
            if not session.input_tokens and old.input_tokens:
                session.input_tokens = old.input_tokens
            if not session.output_tokens and old.output_tokens:
                session.output_tokens = old.output_tokens
            if session.cost_usd is None and old.cost_usd is not None:
                session.cost_usd = old.cost_usd
        session.updated_at = time.time()
        self.sessions[session.id] = session
        self.touch()

    def remove_session(self, session_id: str) -> None:
        if self.sessions.pop(session_id, None) is not None:
            self.touch()

    def set_session_status(self, session_id: str, status: str, detail: str = "") -> None:
        s = self.sessions.get(session_id)
        if s is None:
            return
        s.status = status
        if detail:
            s.detail = detail
        s.updated_at = time.time()
        self.touch()

    # -- approvals -------------------------------------------------------
    def add_approval(self, ap: PendingApproval) -> None:
        self.pending[ap.request_id] = ap
        self.touch()

    def remove_approval(self, request_id: str) -> PendingApproval | None:
        ap = self.pending.pop(request_id, None)
        if ap is not None:
            self.touch()
        return ap

    # -- host stats ------------------------------------------------------
    _HOST_KEYS = (
        "host_cpu_pct", "host_mem_pct", "host_gpu_pct", "host_top_name",
        "host_top_pct", "host_net_down_mbps", "host_net_up_mbps",
        "host_batt_pct", "host_batt_mins",
    )

    def set_host_stats(self, stats: dict) -> None:
        for key in self._HOST_KEYS:
            if key in stats:
                setattr(self, key, stats[key])
        self.touch()

    def host_payload(self) -> dict | None:
        """PROTOCOL.md `host` frame, or None until the first sample."""
        if self.host_cpu_pct is None:
            return None
        return {
            "type": "host",
            "host_cpu_pct": self.host_cpu_pct,
            "host_mem_pct": self.host_mem_pct,
            "host_gpu_pct": self.host_gpu_pct,
            "host_top_name": self.host_top_name,
            "host_top_pct": self.host_top_pct,
            "host_net_down_mbps": self.host_net_down_mbps,
            "host_net_up_mbps": self.host_net_up_mbps,
            "host_batt_pct": self.host_batt_pct,
            "host_batt_mins": self.host_batt_mins,
        }

    # -- protocol payloads ----------------------------------------------
    DEVICE_MAX_SESSIONS = 20  # = MAX_SESSIONS do firmware

    def sessions_payload(self) -> dict:
        """Retrato para o aparelho: no máximo 20, o que importa primeiro
        (precisa de você > trabalhando > terminou > parada; depois o mais
        recente) e textos curtos — o ESP32 não aguenta frames enormes."""
        prio = {"needs-you": 0, "working": 1, "finished": 2, "idle": 3}
        sessions = sorted(self.sessions.values(),
                          key=lambda s: (prio.get(s.status, 9), -s.updated_at))
        out = []
        for s in sessions[: self.DEVICE_MAX_SESSIONS]:
            d = s.to_protocol()
            d["title"] = d["title"][:60]
            d["detail"] = d["detail"][:90]
            if "profile" in d:
                d["profile"] = d["profile"][:32]
            out.append(d)
        return {"type": "sessions", "sessions": out}

    def usage_payloads(self) -> list[dict]:
        out = []
        for s in self.sessions.values():
            if s.input_tokens or s.output_tokens or s.cost_usd is not None:
                d: dict = {
                    "type": "usage",
                    "session_id": s.id,
                    "input_tokens": s.input_tokens,
                    "output_tokens": s.output_tokens,
                }
                if s.cost_usd is not None:
                    d["cost_usd"] = round(s.cost_usd, 4)
                if self.limits:
                    d["limits"] = dict(self.limits)
                out.append(d)
        return out

    def cron_payload(self) -> dict:
        return {"type": "cron", "jobs": list(self.cron_jobs)}
