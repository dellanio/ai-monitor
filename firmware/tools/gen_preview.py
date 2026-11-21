"""Gera docs/preview.html: simulação no navegador das telas do firmware v0.2.

Uso: python tools/gen_preview.py   (requer Pillow)
Reproduz o layout de src/ui.cpp em um canvas 320x240 (aproximado: fontes
do navegador, não as do M5GFX). Os quadros da Nous-Girl são os GIFs reais.
"""
import base64
import io
import json
from pathlib import Path

from PIL import Image, ImageSequence

ROOT = Path(__file__).resolve().parents[1]
MOODS = ["neutral", "thinking", "happy", "listening", "sad", "sleeping", "look_left", "look_right"]
frames = {}
for m in MOODS:
    g = Image.open(ROOT / f"assets/nousgirl/{m}.gif")
    lst = []
    for fr in ImageSequence.Iterator(g):
        buf = io.BytesIO()
        fr.convert("L").save(buf, "PNG", optimize=True)
        lst.append([base64.b64encode(buf.getvalue()).decode(), fr.info.get("duration", 100)])
    frames[m] = lst

tpl = (ROOT / "tools/preview_template.html").read_text(encoding="utf-8")
out = ROOT.parent / "docs/preview.html"
out.parent.mkdir(exist_ok=True)
out.write_text(tpl.replace("/*FRAMES*/null", json.dumps(frames)), encoding="utf-8")
print(out, out.stat().st_size)
