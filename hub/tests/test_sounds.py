"""Sons dos avisos: arquivo no notebook -> PCM 24 kHz -> robô (protocolo "sound")."""
import asyncio
import json
import wave
from types import SimpleNamespace

import numpy as np

from hub.sounds import SoundBank
from hub.voice import VoiceSession


def _wav(path, secs=0.5, sr=44100):
    t = np.arange(int(sr * secs)) / sr
    pcm = (np.sin(2 * np.pi * 440 * t) * 12000).astype("<i2")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr); w.writeframes(pcm.tobytes())


def test_bank_decodes_resamples_caps_and_caches(tmp_path):
    _wav(tmp_path / "finished.wav", secs=0.5)
    _wav(tmp_path / "needyou.wav", secs=9)
    (tmp_path / "outro.wav").write_bytes(b"x")
    b = SoundBank(str(tmp_path), max_s=6, builtin_dir=None)
    assert b.available() == ["needyou", "finished"]
    assert abs(len(b.pcm("finished")) / 2 / 24000 - 0.5) < 0.02
    assert len(b.pcm("needyou")) == 6 * 24000 * 2          # cortado em max_s
    assert b.pcm("finished") is b.pcm("finished")           # cache
    assert b.pcm("new") is None


class FakeWS:
    def __init__(self):
        self.sent = []

    async def send(self, m):
        self.sent.append(m)


def _session(tmp_path, **state):
    server = SimpleNamespace(cfg=SimpleNamespace(), wake=None, sounds=SoundBank(str(tmp_path), builtin_dir=None))
    s = VoiceSession(server, FakeWS())
    for k, v in state.items():
        setattr(s, k, v)
    return s


def test_play_sound_streams_between_start_and_end(tmp_path):
    _wav(tmp_path / "finished.wav", secs=1.0)
    s = _session(tmp_path)
    asyncio.run(s.send_sounds())
    asyncio.run(s.play_sound("finished"))
    first = json.loads(s.ws.sent[0])
    assert first == {"type": "sounds", "names": ["finished"]}
    assert json.loads(s.ws.sent[1])["state"] == "start"
    audio = b"".join(m for m in s.ws.sent if isinstance(m, bytes))
    assert len(audio) == 24000 * 2
    assert max(len(m) for m in s.ws.sent if isinstance(m, bytes)) <= 4800   # robô aceita até ~15 KB
    assert json.loads(s.ws.sent[-1]) == {"type": "sound", "state": "end", "name": "finished"}


def test_busy_conversation_refuses_with_bare_end(tmp_path):
    _wav(tmp_path / "finished.wav")
    s = _session(tmp_path, speaking=True)
    asyncio.run(s.play_sound("finished"))
    assert [json.loads(m)["state"] for m in s.ws.sent] == ["end"]


def test_builtin_sounds_ship_with_hub_and_user_file_wins(tmp_path):
    b = SoundBank(str(tmp_path / "nada"))
    assert b.available() == ["needyou", "finished", "new"]        # padrão do repositório
    assert b.pcm("finished")
    _wav(tmp_path / "finished.wav", secs=0.3)
    b2 = SoundBank(str(tmp_path))
    assert b2.find("finished") == tmp_path / "finished.wav"
    assert b2.find("new").parent.name == "sounds"
