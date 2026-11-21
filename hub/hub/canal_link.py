"""Ligação ao canal de voz do Hermes (voz-hermes, motor `codex` = voz da assinatura ChatGPT).

Cópia de voz-hermes/server/voz/xiaozhi/canal.py (branch pkg/xiaozhi-assinatura). O áudio do
aparelho (PCM16 16 kHz) vai para o canal (`/voz/ws`, o mesmo do Matrix e da bandeja), que roda o
motor de voz em tempo real da assinatura e delega pedidos ao Hermes principal. A conversa fica
aberta por `keep_s` depois do último uso, porque abrir o motor leva vários segundos.
"""
from __future__ import annotations

import asyncio
import json
import logging
import os
import time
from typing import Any, Protocol

log = logging.getLogger("hub.voice.canal")

DEFAULT_URL = "ws://127.0.0.1:4850/voz/ws"
CHUNK = 3200  # 100 ms de PCM16 16 kHz por mensagem, como a ponte do Discord
MAX_PENDING = 16000 * 2 * 8  # até 8 s de fala guardados enquanto o motor abre


class Sink(Protocol):
    """Quem recebe os eventos do canal (a conexão Xiaozhi atual do aparelho)."""

    async def on_canal_event(self, ev: dict[str, Any]) -> None: ...
    async def on_canal_audio(self, pcm24k: bytes, meta: dict[str, Any]) -> None: ...


class CanalLink:
    def __init__(self, name: str, session_id: str, url: str, keep_s: float, engine: str = "codex") -> None:
        self.name = name
        self.session_id = session_id
        self.url = url
        self.keep_s = keep_s
        self.engine = engine
        self.ws = None
        self.ready = asyncio.Event()
        self.failed: str | None = None
        self.sink: Sink | None = None
        self.pending = bytearray()
        self._flusher: asyncio.Task | None = None  # esvazia `pending` em ritmo de fala
        self._meta: dict[str, Any] = {}
        self._reader: asyncio.Task | None = None
        self._opener: asyncio.Task | None = None
        self._closer: asyncio.Task | None = None
        self.opened_at = 0.0

    # ---- ciclo de vida ----
    def attach(self, sink: Sink) -> None:
        self.sink = sink
        if self._closer and not self._closer.done():
            self._closer.cancel()
        self._closer = None
        if self.ws is None and (self._opener is None or self._opener.done()):
            self.ready.clear()
            self.failed = None
            self._opener = asyncio.create_task(self._open(), name=f"canal-open-{self.name}")

    def detach(self, sink: Sink) -> None:
        if self.sink is not sink:
            return
        self.sink = None
        self.pending.clear()
        if self._closer is None or self._closer.done():
            self._closer = asyncio.create_task(self._close_later(), name=f"canal-keep-{self.name}")

    async def _close_later(self) -> None:
        await asyncio.sleep(self.keep_s)
        log.info("canal %s: %.0f s sem aparelho, fechando a conversa", self.name, self.keep_s)
        await self.close()

    async def close(self) -> None:
        ws, self.ws = self.ws, None
        self.ready.clear()
        for t in (self._reader, self._opener):
            if t and not t.done() and t is not asyncio.current_task():
                t.cancel()
        if ws is not None:
            try:
                await ws.close()
            except Exception:  # noqa: BLE001
                pass

    async def _open(self) -> None:
        from websockets.asyncio.client import connect

        t0 = time.monotonic()
        try:
            ws = await connect(self.url, max_size=8 * 1024 * 1024, open_timeout=10, ping_interval=20)
            await ws.send(json.dumps({"type": "hello", "client": "aimonitor", "engine": self.engine,
                                      "session": self.session_id}))
            while True:
                raw = await asyncio.wait_for(ws.recv(), 60)
                if isinstance(raw, bytes):
                    continue
                ev = json.loads(raw)
                kind = ev.get("type")
                if kind == "ready":
                    log.info("canal %s pronto em %.0f ms (motor %s, hermes %s)", self.name,
                             (time.monotonic() - t0) * 1000, ev.get("engine"), ev.get("hermes"))
                    break
                if kind == "error":
                    raise RuntimeError(str(ev.get("message") or "erro no canal")[:200])
                if kind == "engine.fallback":
                    log.warning("canal %s: %s", self.name, str(ev.get("message"))[:200])
            self.ws = ws
            self.opened_at = time.monotonic()
            self._reader = asyncio.create_task(self._read(ws), name=f"canal-read-{self.name}")
            self.ready.set()
            if self.pending:
                # O motor descarta áudio acumulado demais: o que foi dito enquanto o canal abria
                # sai um pouco mais rápido que a fala real (1,25x) até alcançar o microfone ao vivo.
                self._flusher = asyncio.create_task(self._flush_paced(ws), name=f"canal-flush-{self.name}")
        except Exception as exc:  # noqa: BLE001
            self.failed = f"{type(exc).__name__}: {str(exc)[:160]}"
            log.warning("canal %s indisponível: %s", self.name, self.failed)
            self.ready.set()
            sink = self.sink
            if sink is not None:
                await sink.on_canal_event({"type": "error", "code": "canal", "message": self.failed})

    async def _read(self, ws) -> None:
        try:
            async for raw in ws:
                sink = self.sink
                if isinstance(raw, bytes):
                    if sink is not None:
                        await sink.on_canal_audio(raw, self._meta)
                    continue
                try:
                    ev = json.loads(raw)
                except json.JSONDecodeError:
                    continue
                if ev.get("type") == "audio":
                    self._meta = ev
                    continue
                if sink is not None:
                    await sink.on_canal_event(ev)
        except Exception as exc:  # noqa: BLE001
            log.info("canal %s: leitura encerrada (%s)", self.name, type(exc).__name__)
        finally:
            if self.ws is ws:
                self.ws = None
                self.ready.clear()
                sink = self.sink
                if sink is not None:
                    await sink.on_canal_event({"type": "error", "code": "canal_closed", "message": "canal fechou"})

    async def _flush_paced(self, ws) -> None:
        try:
            while self.pending and self.ws is ws:
                chunk, self.pending = bytes(self.pending[:CHUNK]), self.pending[CHUNK:]
                await ws.send(chunk)
                await asyncio.sleep(0.08)  # 100 ms de áudio a cada 80 ms
        except Exception:  # noqa: BLE001
            pass

    # ---- envio ----
    async def push_pcm(self, pcm16k: bytes) -> None:
        if self.ws is None or (self._flusher is not None and not self._flusher.done()):
            self.pending += pcm16k
            if len(self.pending) > MAX_PENDING:
                del self.pending[: len(self.pending) - MAX_PENDING]
            return
        await self._send_pcm(pcm16k)

    async def _send_pcm(self, data: bytes) -> None:
        ws = self.ws
        if ws is None:
            return
        for i in range(0, len(data), CHUNK):
            await ws.send(data[i : i + CHUNK])

    async def control(self, ev: dict[str, Any]) -> None:
        ws = self.ws
        if ws is not None:
            try:
                await ws.send(json.dumps(ev))
            except Exception:  # noqa: BLE001
                pass


class Links:
    """Registro de conversas por aparelho."""

    def __init__(self, url: str | None = None, keep_s: float | None = None) -> None:
        self.url = url or os.environ.get("VOZ_CANAL_URL", DEFAULT_URL)
        self.keep_s = float(keep_s if keep_s is not None else os.environ.get("VOZ_CANAL_KEEP_S", "300"))
        self.engine = os.environ.get("VOZ_CANAL_ENGINE", "codex")
        self._by_name: dict[str, CanalLink] = {}

    def get(self, name: str, session_id: str) -> CanalLink:
        link = self._by_name.get(name)
        if link is None:
            link = self._by_name[name] = CanalLink(name, session_id, self.url, self.keep_s, self.engine)
        return link
