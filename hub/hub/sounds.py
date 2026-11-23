"""Sons dos avisos do AI Monitor: arquivos escolhidos no notebook.

Coloque em ``sounds_dir`` (padrão ``~/.dellmonitor/sounds``) arquivos com estes nomes,
em qualquer formato que o FFmpeg/PyAV leia (wav, mp3, ogg, m4a...):

    needyou.*   precisa de você
    finished.*  terminou
    new.*       sessão nova

O Hub converte para PCM16 mono 24 kHz (o que o robô toca), corta em ``max_s`` e
guarda em cache até o arquivo mudar. Trocar um som = trocar o arquivo; sem gravar
o robô. Os sons padrão vêm com o Hub (``hub/sounds/``); um arquivo seu com o mesmo
nome em ``sounds_dir`` tem prioridade. Sem nenhum, o robô usa os toques embutidos.
"""
from __future__ import annotations

import logging
from pathlib import Path

log = logging.getLogger("hub.sounds")

NAMES = ("needyou", "finished", "new")
# Versões por ferramenta: "<nome>-<fonte>.*" (ex.: finished-claude.mp3, "Claude terminou").
# Sem a versão da ferramenta, toca o "<nome>.*" genérico.
SOURCES = ("hermes", "claude", "codex", "timon", "orca")
EXTS = (".wav", ".mp3", ".ogg", ".m4a", ".flac", ".aac", ".opus", ".webm")
RATE = 24000
BUILTIN_DIR = Path(__file__).resolve().parent.parent / "sounds"
_DEFAULT = object()


def decode_pcm24k(path: Path, max_s: float = 6.0, gain: float = 1.0) -> bytes:
    import av
    import numpy as np

    out = []
    limit = int(RATE * max_s)
    got = 0
    with av.open(str(path)) as cont:
        stream = next(s for s in cont.streams if s.type == "audio")
        res = av.AudioResampler(format="s16", layout="mono", rate=RATE)
        for frame in cont.decode(stream):
            for f in res.resample(frame):
                a = f.to_ndarray().reshape(-1)
                out.append(a)
                got += a.size
            if got >= limit:
                break
        for f in res.resample(None):
            out.append(f.to_ndarray().reshape(-1))
    pcm = np.concatenate(out)[:limit] if out else np.zeros(0, dtype=np.int16)
    if gain != 1.0:
        pcm = np.clip(pcm.astype(np.float32) * gain, -32768, 32767).astype(np.int16)
    fade = min(240, pcm.size // 2)  # 10 ms nas pontas: sem estalo
    if fade:
        pcm = pcm.astype(np.float32)
        ramp = np.linspace(0.0, 1.0, fade)
        pcm[:fade] *= ramp
        pcm[-fade:] *= ramp[::-1]
        pcm = pcm.astype(np.int16)
    return pcm.astype("<i2").tobytes()


class SoundBank:
    def __init__(self, sounds_dir: str, max_s: float = 6.0, gain: float = 1.0, builtin_dir=_DEFAULT) -> None:
        self.dir = Path(sounds_dir).expanduser()
        b = BUILTIN_DIR if builtin_dir is _DEFAULT else builtin_dir
        self.dirs = [self.dir] + ([Path(b)] if b else [])
        self.max_s = float(max_s)
        self.gain = float(gain)
        self._cache: dict[str, tuple[Path, float, bytes]] = {}

    def find(self, name: str, source: str = "") -> Path | None:
        if name not in NAMES:
            return None
        stems = ([f"{name}-{source}"] if source in SOURCES else []) + [name]
        for stem in stems:  # a versão da ferramenta primeiro, depois a genérica
            for d in self.dirs:  # o seu primeiro, depois o padrão do Hub
                if not d.is_dir():
                    continue
                for ext in EXTS:
                    p = d / f"{stem}{ext}"
                    if p.is_file():
                        return p
        return None

    def available(self) -> list[str]:
        return [n for n in NAMES if self.find(n)]

    def pcm(self, name: str, source: str = "") -> bytes | None:
        p = self.find(name, source)
        if p is None:
            return None
        mt = p.stat().st_mtime
        key = p.stem
        hit = self._cache.get(key)
        if hit and hit[0] == p and hit[1] == mt:
            return hit[2]
        try:
            data = decode_pcm24k(p, self.max_s, self.gain)
        except Exception as exc:  # noqa: BLE001
            log.warning("som %s: nao consegui ler %s (%s)", name, p.name, exc)
            return None
        self._cache[key] = (p, mt, data)
        log.info("som %s: %s (%.1f s)", name, p.name, len(data) / 2 / RATE)
        return data
