"""Voz do AI Monitor: palavra de ativação local + ponte para o canal (canal falso)."""
from __future__ import annotations

import asyncio
import json
import os
import wave
from pathlib import Path

import pytest

from hub.config import VoiceConfig
from hub.voice import VoiceServer, WakeDetector

MODEL = os.environ.get("AIMON_WAKE_MODEL", os.path.expanduser("~/models/vosk-model-small-en-us-0.15"))
FIX = Path(os.environ.get("AIMON_WAKE_FIXTURES", "tests/fixtures/wake"))
needs_model = pytest.mark.skipif(not Path(MODEL).exists() or not (FIX / "pt_hey.wav").exists(),
                                 reason="modelo Vosk/fixtures de voz ausentes")


def _pcm(name: str) -> bytes:
    with wave.open(str(FIX / name)) as w:
        return w.readframes(w.getnframes())


def _detect(det: WakeDetector, pcm: bytes) -> bool:
    det.reset()
    return any(det.feed(pcm[i:i + 1920]) for i in range(0, len(pcm), 1920))


@needs_model
def test_wake_word_hey_hermes_pt_and_en_and_no_false_positive():
    det = WakeDetector(MODEL, ["hey hermes", "hi hermes"])
    assert _detect(det, _pcm("pt_hey.wav"))
    assert _detect(det, _pcm("en_hey.wav"))
    assert not _detect(det, _pcm("neg1.wav"))
    assert not _detect(det, _pcm("neg2.wav"))


class FakeLink:
    def __init__(self):
        self.ws = object()
        self.failed = None
        self.pushed = bytearray()
        self.controls = []
        self.sink = None

    def attach(self, sink):
        self.sink = sink

    def detach(self, sink):
        self.sink = None

    async def push_pcm(self, pcm):
        self.pushed += pcm

    async def control(self, ev):
        self.controls.append(ev)

    async def close(self):
        pass


class FakeWake:
    def __init__(self):
        self.fire = False

    def feed(self, pcm):
        return self.fire

    def reset(self):
        pass


def test_voice_session_routes_mic_only_after_wake_and_forwards_reply():
    async def run():
        from websockets.asyncio.client import connect

        cfg = VoiceConfig(enabled=True, host="127.0.0.1", port=0, wake_model="", follow_up_s=0.8)
        wake, link = FakeWake(), FakeLink()
        srv = VoiceServer(cfg, "token-de-teste-123", wake=wake, link=link)
        from websockets.asyncio.server import serve

        srv._server = await serve(srv._handler, "127.0.0.1", 0, process_request=srv._authorize)
        port = srv._server.sockets[0].getsockname()[1]
        try:
            # sem token: recusado
            with pytest.raises(Exception):
                async with connect(f"ws://127.0.0.1:{port}/voice"):
                    pass
            async with connect(f"ws://127.0.0.1:{port}/voice?token=token-de-teste-123") as ws:
                assert json.loads(await ws.recv())["state"] == "idle"
                await ws.send(b"\1\0" * 960)          # antes do "Hey Hermes": não vai ao canal
                await asyncio.sleep(0.1)
                assert len(link.pushed) == 0
                wake.fire = True
                await ws.send(b"\2\0" * 960)
                assert json.loads(await ws.recv())["state"] == "listening"
                wake.fire = False
                await ws.send(b"\3\0" * 960)          # depois: vai ao canal
                await asyncio.sleep(0.1)
                assert link.pushed == b"\3\0" * 960
                sess = srv.current
                await sess.on_canal_event({"type": "user.text", "text": "oi"})
                assert json.loads(await ws.recv())["state"] == "thinking"
                await sess.on_canal_audio(b"\0\1" * 2400, {})
                assert json.loads(await ws.recv())["state"] == "speaking"
                assert await ws.recv() == b"\0\1" * 2400
                await sess.on_canal_event({"type": "reply.done", "text": "ola"})
                assert json.loads(await ws.recv())["state"] == "end"
                await ws.send(json.dumps({"type": "played"}))
                # sem você falar por follow_up_s: volta à espera
                assert json.loads(await asyncio.wait_for(ws.recv(), 3))["state"] == "idle"
                await ws.send(b"\4\0" * 960)
                await asyncio.sleep(0.1)
                assert link.pushed == b"\3\0" * 960
                # toque para parar a fala
                await ws.send(json.dumps({"type": "stop"}))
                assert json.loads(await ws.recv())["state"] == "end"
                assert {"type": "interrupt"} in link.controls
        finally:
            srv._server.close()

    asyncio.run(run())
