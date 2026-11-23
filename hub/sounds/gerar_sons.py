"""Gera os sons do AI Monitor (+ alternativas em opcoes/).

Destino: argumento 1 ou ~/.dellmonitor/sounds. ``--ferramentas`` gera só as falas por
ferramenta ("Claude terminou", "Hermes precisa de você", ...), que o Hub toca como
``<aviso>-<fonte>.*`` antes do som genérico.
"""
import asyncio
import wave
from pathlib import Path

import edge_tts
import numpy as np

import sys

_args = [a for a in sys.argv[1:] if not a.startswith("--")]
D = Path(_args[0]) if _args else Path.home() / ".dellmonitor" / "sounds"
SO_FERRAMENTAS = "--ferramentas" in sys.argv
O = D / "opcoes"
O.mkdir(parents=True, exist_ok=True)
SR = 24000


def bell(notes, dur=0.9, gap=0.11, vol=0.55):
    """Sino suave: fundamental + parciais com decaimento exponencial."""
    total = int(SR * (gap * (len(notes) - 1) + dur))
    out = np.zeros(total)
    for i, f in enumerate(notes):
        t = np.arange(int(SR * dur)) / SR
        tone = (np.sin(2 * np.pi * f * t) + 0.35 * np.sin(2 * np.pi * 2.01 * f * t) * np.exp(-t * 6)
                + 0.15 * np.sin(2 * np.pi * 3.0 * f * t) * np.exp(-t * 9))
        env = np.exp(-t * 4.5) * np.minimum(1, t / 0.004)
        s = int(SR * gap * i)
        out[s:s + t.size] += tone * env
    out = out / np.abs(out).max() * vol
    return (out * 32767).astype("<i2")


def save_wav(path, pcm):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR); w.writeframes(pcm.tobytes())


async def tts(text, path, voice="pt-BR-FranciscaNeural", rate="+5%"):
    await edge_tts.Communicate(text, voice, rate=rate).save(str(path))


# fonte do Hub -> nome falado
FERRAMENTAS = {"hermes": "Hermes", "claude": "Claude", "codex": "ChatGPT", "timon": "Timon", "orca": "Orca"}
ACOES = {"finished": "{n} terminou.", "needyou": "{n} precisa de você.", "new": "{n} começou."}


async def ferramentas():
    for src, nome in FERRAMENTAS.items():
        for aviso, frase in ACOES.items():
            for old in D.glob(f"{aviso}-{src}.*"):
                old.unlink()
            await tts(frase.format(n=nome), D / f"{aviso}-{src}.mp3")


async def main():
    if SO_FERRAMENTAS:
        await ferramentas()
        for p in sorted(D.glob("*-*.mp3")):
            print(p.name, p.stat().st_size)
        return
    # alternativas
    save_wav(O / "sino-terminou.wav", bell([1046.5, 1318.5, 1568.0]))          # dó-mi-sol subindo
    save_wav(O / "sino-precisa.wav", bell([1568.0, 1174.7, 1568.0, 1174.7], gap=0.16))
    save_wav(O / "sino-nova.wav", bell([880.0], dur=0.5, vol=0.35))
    await tts("Terminei!", O / "voz-terminei.mp3")
    await tts("Preciso de você.", O / "voz-preciso-de-voce.mp3")
    await tts("Pronto, terminei.", O / "voz-pronto-terminei.mp3")
    await tts("Ei, preciso da sua ajuda.", O / "voz-preciso-da-sua-ajuda.mp3")
    await tts("Nova sessão.", O / "voz-nova-sessao.mp3")
    # escolhidos (o Hub toca needyou.* / finished.* / new.*)
    for name, src in (("finished", "voz-terminei.mp3"), ("needyou", "voz-preciso-de-voce.mp3"),
                      ("new", "sino-nova.wav")):
        for old in D.glob(f"{name}.*"):
            old.unlink()
        (D / f"{name}{Path(src).suffix}").write_bytes((O / src).read_bytes())
    for p in sorted(D.rglob("*.*")):
        print(p.relative_to(D), p.stat().st_size)


asyncio.run(main())
