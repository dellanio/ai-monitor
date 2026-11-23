"""hub-stack-chan (AI Monitor) — orchestrator.

Wires the sources (Hermes, Claude Code) to the device server: sources
write into HubState, the Hub pushes snapshots/approvals to the device,
and device answers (approve / message) are routed back to the right source.
"""
from __future__ import annotations

import asyncio
import logging

from .claude_source import ClaudeSource
from .config import Config
from .device_server import DeviceServer
from .hermes_client import HermesClient
from .host_stats import HostStats
from .state import HubState, Session
from .timon_source import TimonSource
from .codex_source import CodexSource


class Hub:
    def __init__(self, config: Config) -> None:
        self.config = config
        self.log = logging.getLogger("dellmonitor.hub")
        self.state = HubState()
        self.device = DeviceServer(
            self.state,
            config.device.host,
            config.device.port,
            mdns=config.device.mdns,
            log=self.log.getChild("device"),
            token=config.device.token,
            allow_actions=config.device.allow_actions,
            allow_messages=config.device.allow_messages,
        )
        self.device.on_approve = self.on_device_approve
        self.device.on_message = self.on_device_message
        self._host_stats_task: asyncio.Task | None = None
        self.sources = []
        if config.hermes.enabled:
            self.sources.append(HermesClient(self, config.hermes))
        if config.claude.enabled:
            self.sources.append(ClaudeSource(self, config.claude))
        if config.timon.enabled:
            self.sources.append(TimonSource(self, config.timon))
        if config.codex.enabled:
            self.sources.append(CodexSource(self, config.codex))
        self.voice = None
        if config.voice.enabled:
            from .voice import VoiceServer

            self.voice = VoiceServer(config.voice, config.device.token)

    # -- lifecycle -------------------------------------------------------
    async def start(self) -> None:
        await self.device.start()
        if self.config.host.enabled:
            self._host_stats_task = asyncio.create_task(self._host_stats_loop())
        for source in self.sources:
            try:
                await source.start()
            except Exception as exc:
                self.log.error("source %s failed to start: %s", source.name, exc)
        if self.voice is not None:
            try:
                await self.voice.start()
            except Exception as exc:
                self.log.error("voz nao iniciou: %s", exc)
                self.voice = None
        self.log.info("Hub up: device ws on :%s", self.device.actual_port)

    async def stop(self) -> None:
        if self._host_stats_task is not None:
            self._host_stats_task.cancel()
            self._host_stats_task = None
        for source in self.sources:
            try:
                await source.stop()
            except Exception:
                pass
        if self.voice is not None:
            await self.voice.stop()
        await self.device.stop()

    async def _host_stats_loop(self) -> None:
        """Sample the PC's CPU/RAM/GPU/net/battery and push to the device."""
        # psutil/nvidia-smi bloqueiam (no Windows, process_iter leva segundos):
        # tudo roda numa thread para não travar o WebSocket do aparelho.
        stats = await asyncio.to_thread(HostStats)
        if not stats.available:
            self.log.warning("psutil not installed; host stats disabled")
            return
        while True:
            try:
                sample = await asyncio.to_thread(stats.collect)
                if sample is not None:
                    self.state.set_host_stats(sample)
            except Exception as exc:
                self.log.debug("host stats collect failed: %s", exc)
            await asyncio.sleep(self.config.host.interval_s)

    # -- state funnel (used by sources) ----------------------------------
    def upsert_session(self, session: Session) -> None:
        self.state.upsert_session(session)

    async def push_sessions(self) -> None:
        await self.device.broadcast(self.state.sessions_payload())

    async def push_finished(self, session_id: str, title: str, source: str = "") -> None:
        await self.device.push_finished(session_id, title, source)

    def source(self, name: str):
        for s in self.sources:
            if s.name == name:
                return s
        return None

    # -- device answers --------------------------------------------------
    async def on_device_approve(self, request_id: str, choice: str) -> None:
        ap = self.state.remove_approval(request_id)
        if ap is None:
            self.log.debug("approve for unknown request %s ignored", request_id)
            return
        if ap.answer is not None and not ap.answer.done():
            ap.answer.set_result(choice)
        if ap.source == "hermes":
            hermes = self.source("hermes")
            if hermes is not None:
                await hermes.respond_approval(request_id, choice)
        # Claude: the blocked PreToolUse HTTP handler awaits ap.answer.
        if ap.session_id:
            self.state.set_session_status(ap.session_id, "working")
        await self.device.push_approval_cancelled(request_id)
        await self.push_sessions()
        self.log.info("approval %s answered: %s", request_id, choice)

    async def on_device_message(self, session_id: str, text: str) -> None:
        if not session_id or not text:
            return
        session = self.state.sessions.get(session_id)
        if session is None:
            self.log.warning("message for unknown session %s ignored", session_id)
            return
        if session.source == "hermes":
            hermes = self.source("hermes")
            if hermes is not None:
                await hermes.prompt_submit(session_id, text)
        else:
            # v0.1: sending text back to Claude Code is not implemented
            # (it would need terminal injection, like Miblo does for tmux).
            self.log.warning("message to %s session %s not supported in v0.1", session.source, session_id)
