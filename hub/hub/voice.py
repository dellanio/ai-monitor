"""Voz do AI Monitor: "Hey Hermes" no notebook + canal de voz da assinatura.

O aparelho manda o microfone (PCM16 16 kHz mono) por um WebSocket próprio (`/voice`, porta
`voice.port`), separado do link do monitor. O Hub:

1. em espera, passa o áudio só pelo detector local de palavra de ativação (Vosk, gramática
   fechada; nada sai do notebook);
2. ao ouvir "Hey Hermes", avisa o aparelho (`listening`) e encaminha o microfone ao canal de voz
   do Hermes (`voz-hermes`, motor `codex`: voz em tempo real da assinatura ChatGPT);
3. devolve a fala do canal ao aparelho (PCM16 24 kHz) e volta à espera depois de `follow_up_s`
   sem você falar.

Protocolo (texto = JSON):
  aparelho -> Hub  binário PCM16 16k | {"type":"played"} (terminou de tocar) | {"type":"stop"}
  Hub -> aparelho  binário PCM16 24k | {"type":"voice","state":"idle|listening|thinking|speaking|end|error"}
"""
from __future__ import annotations

import asyncio
import hmac
import json
import logging
import os
import time
from http import HTTPStatus
from typing import Any
from urllib.parse import parse_qs, urlsplit

from .canal_link import CanalLink

log = logging.getLogger("hub.voice")


def rms(pcm: bytes) -> float:
    """RMS de PCM16, amostrando 1 a cada 4."""
    from array import array

    a = array("h")
    a.frombytes(pcm[: len(pcm) // 2 * 2])
    s = a[::4]
    return (sum(x * x for x in s) / len(s)) ** 0.5 if s else 0.0


def is_speech(pcm: bytes, threshold: float = 900.0) -> bool:
    """Detector simples de fala por energia."""
    return rms(pcm) > threshold


class WakeDetector:
    """Palavra de ativação com Vosk + gramática fechada (frases + [unk])."""

    def __init__(self, model_path: str, phrases: list[str]) -> None:
        from vosk import KaldiRecognizer, Model, SetLogLevel

        SetLogLevel(-1)
        self._Rec = KaldiRecognizer
        self.model = Model(os.path.expanduser(model_path))
        self.phrases = [p.lower() for p in phrases]
        self._grammar = json.dumps(self.phrases + ["hermes", "[unk]"])
        self.reset()

    def reset(self) -> None:
        self.rec = self._Rec(self.model, 16000, self._grammar)

    def _hit(self, text: str) -> bool:
        return any(p in text for p in self.phrases)

    def feed(self, pcm16k: bytes) -> bool:
        if self.rec.AcceptWaveform(pcm16k):
            text = json.loads(self.rec.Result()).get("text", "")
            if text and text.replace("[unk]", "").strip():
                log.info("voz: detector ouviu %r", text)
        else:
            text = json.loads(self.rec.PartialResult()).get("partial", "")
        if text and self._hit(text):
            self.reset()
            return True
        return False


class VoiceSession:
    """Um aparelho conectado ao /voice."""

    def __init__(self, server: "VoiceServer", ws) -> None:
        self.server = server
        self.ws = ws
        self.cfg = server.cfg
        self.active = False          # depois do "Hey Hermes", antes de voltar à espera
        self.last_activity = 0.0     # você falou / ela falou / terminou de tocar
        self.speaking = False
        self.thinking = False        # você falou e a resposta ainda não veio
        self.tools_open = 0          # tarefas delegadas ao Hermes em andamento
        self.busy_at = 0.0           # quando começou a espera pela resposta (teto de 180 s)
        self.wake = server.wake
        self._watch: asyncio.Task | None = None

    async def send_state(self, state: str, **extra: Any) -> None:
        try:
            await self.ws.send(json.dumps({"type": "voice", "state": state, **extra}))
        except Exception:  # noqa: BLE001
            pass

    # --- eventos do canal (Sink) ---
    async def on_canal_event(self, ev: dict[str, Any]) -> None:
        kind = ev.get("type")
        if kind == "user.text":
            self.last_activity = self.busy_at = time.monotonic()
            self.thinking = True
            log.info("voz: voce disse (%d letras)", len(str(ev.get("text") or "")))
            await self.send_state("thinking")
        elif kind == "state" and ev.get("value") == "speaking":
            await self._start_speaking()
        elif kind == "tool":
            self.tools_open += 1
            self.last_activity = self.busy_at = time.monotonic()
        elif kind == "tool.done":
            self.tools_open = max(0, self.tools_open - 1)
            self.last_activity = time.monotonic()
        elif kind == "reply.done":
            self.last_activity = time.monotonic()
            self.thinking = False
            if self.speaking:
                self.speaking = False
                await self.send_state("end", interrupted=bool(ev.get("interrupted")))
        elif kind == "error":
            log.warning("voz: canal %s %s", ev.get("code"), str(ev.get("message"))[:160])
            self.speaking = False
            await self.send_state("error")

    async def on_canal_audio(self, pcm24k: bytes, meta: dict[str, Any]) -> None:
        if not pcm24k:
            return
        await self._start_speaking()
        self.last_activity = time.monotonic()
        try:
            await self.ws.send(pcm24k)
        except Exception:  # noqa: BLE001
            pass

    async def _start_speaking(self) -> None:
        if not self.speaking:
            self.speaking = True
            await self.send_state("speaking")

    # --- aparelho ---
    async def on_mic(self, pcm: bytes) -> None:
        if self.active:
            if is_speech(pcm):  # você está falando: não conta como silêncio
                self.last_activity = time.monotonic()
            await self.server.link.push_pcm(pcm)
            return
        self._level_peak = max(getattr(self, "_level_peak", 0.0), rms(pcm))
        now = time.monotonic()
        if now - getattr(self, "_level_at", 0.0) > 10:
            log.info("voz: nivel do microfone (pico RMS em 10 s): %.0f", self._level_peak)
            self._level_at, self._level_peak = now, 0.0
        if self.wake is not None and await asyncio.to_thread(self.wake.feed, pcm):
            await self.wake_up()

    async def wake_up(self) -> None:
        log.info("voz: Hey Hermes")
        self.active = True
        self.last_activity = time.monotonic()
        self.server.link.attach(self)
        await self.send_state("listening")
        if self._watch is None or self._watch.done():
            self._watch = asyncio.create_task(self._follow_up_watch())

    async def go_idle(self) -> None:
        self.active = False
        self.thinking = False
        self.tools_open = 0
        if self.wake is not None:
            self.wake.reset()
        await self.send_state("idle")
        log.info("voz: de volta a espera")
        # A conversa no canal continua aberta (resposta atrasada de uma tarefa ainda chega);
        # fecha só depois de keep_s sem novo "Hey Hermes".
        self.server.schedule_link_close()

    async def _follow_up_watch(self) -> None:
        while self.active:
            await asyncio.sleep(0.5)
            link = self.server.link
            busy = (self.thinking or self.tools_open) and time.monotonic() - self.busy_at < 180
            if self.speaking or busy or (link.ws is None and link.failed is None):
                # falando, ou o canal ainda abrindo (leva ~10 s): não conta como silêncio
                self.last_activity = time.monotonic()
                continue
            if time.monotonic() - self.last_activity > self.cfg.follow_up_s:
                await self.go_idle()

    async def on_control(self, msg: dict[str, Any]) -> None:
        kind = msg.get("type")
        if kind == "played":
            self.last_activity = time.monotonic()
        elif kind == "stop":
            self.speaking = False
            await self.server.link.control({"type": "interrupt"})
            await self.send_state("end", interrupted=True)
            self.last_activity = time.monotonic()
        elif kind == "wake":  # toque/botão: o mesmo que dizer "Hey Hermes"
            await self.wake_up()

    async def close(self) -> None:
        self.active = False
        if self._watch and not self._watch.done():
            self._watch.cancel()


class VoiceServer:
    def __init__(self, cfg, token: str, wake: WakeDetector | None = None, link: CanalLink | None = None) -> None:
        self.cfg = cfg
        self.token = token
        self.wake = wake
        self.link = link or CanalLink("aimonitor", cfg.session, cfg.canal_url, cfg.keep_s, cfg.engine)
        self.current: VoiceSession | None = None
        self._server = None
        self._link_idle: asyncio.Task | None = None

    def schedule_link_close(self) -> None:
        if self._link_idle and not self._link_idle.done():
            self._link_idle.cancel()

        async def later() -> None:
            await asyncio.sleep(self.cfg.keep_s)
            if self.current is None or not self.current.active:
                log.info("voz: %.0f s sem uso, fechando a conversa no canal", self.cfg.keep_s)
                await self.link.close()

        self._link_idle = asyncio.create_task(later())

    def _authorize(self, connection, request):
        presented = None
        auth = request.headers.get("Authorization", "")
        if auth.lower().startswith("bearer "):
            presented = auth[7:].strip()
        if presented is None:
            presented = (parse_qs(urlsplit(request.path).query).get("token") or [None])[0]
        if presented and hmac.compare_digest(presented.encode(), self.token.encode()):
            return None
        log.warning("voz: aparelho recusado (token) de %s", connection.remote_address)
        return connection.respond(HTTPStatus.UNAUTHORIZED, "token invalido\n")

    async def start(self) -> None:
        from websockets.asyncio.server import serve

        if self.wake is None and self.cfg.wake_model:
            self.wake = await asyncio.to_thread(WakeDetector, self.cfg.wake_model, list(self.cfg.wake_phrases))
        self._server = await serve(self._handler, self.cfg.host, self.cfg.port,
                                   process_request=self._authorize, max_size=1 << 20,
                                   ping_interval=20, ping_timeout=20)
        log.info("voz: ouvindo em %s:%d (canal %s, frases %s)", self.cfg.host, self.cfg.port,
                 self.cfg.canal_url, list(self.cfg.wake_phrases))

    async def stop(self) -> None:
        if self._server is not None:
            self._server.close()
            await self._server.wait_closed()
        await self.link.close()

    async def _handler(self, ws) -> None:
        if self.current is not None:  # um aparelho por vez: o novo assume
            try:
                await self.current.ws.close()
            except Exception:  # noqa: BLE001
                pass
        sess = VoiceSession(self, ws)
        self.current = sess
        log.info("voz: aparelho conectado %s", ws.remote_address)
        await sess.send_state("idle")
        try:
            async for raw in ws:
                if isinstance(raw, bytes):
                    await sess.on_mic(raw)
                    continue
                try:
                    msg = json.loads(raw)
                except (ValueError, TypeError):
                    continue
                await sess.on_control(msg)
        except Exception as exc:  # noqa: BLE001
            log.info("voz: link encerrado (%r)", exc)
        finally:
            await sess.close()
            if self.current is sess:
                self.current = None
                self.link.detach(sess)  # fecha a conversa depois de keep_s
            log.info("voz: aparelho desconectado")
