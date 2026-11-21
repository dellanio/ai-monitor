"""Gera src/nous_avatar.h: rosto da Nous-Girl (homenagem) em 36x36, luminância 8 bits.

Uso: python tools/gen_avatar.py   (requer Pillow)
O firmware tinge a luminância com a paleta do tema, então o avatar acompanha
as cores do estado (trabalhando, precisa de você...).
"""
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SIZE = 36
im = Image.open(ROOT / "assets/nousgirl/neutral.gif")
im.seek(0)
face = im.convert("L").crop((44, 36, 194, 186)).resize((SIZE, SIZE), Image.LANCZOS)
px = list(face.tobytes())
rows = []
for y in range(SIZE):
    rows.append("  " + ",".join(str(v) for v in px[y * SIZE:(y + 1) * SIZE]) + ",")
out = (
    "// GERADO por tools/gen_avatar.py — nao editar.\n"
    "// Arte de homenagem a Nous-Girl: uso pessoal, nao comercial\n"
    "// (ver assets/nousgirl/TRIBUTE-README.md).\n#pragma once\n#include <stdint.h>\n"
    f"#define NOUS_AVATAR_SIZE {SIZE}\n"
    f"static const uint8_t NOUS_AVATAR_LUMA[{SIZE * SIZE}] = {{\n" + "\n".join(rows) + "\n};\n"
)
(ROOT / "src/nous_avatar.h").write_text(out, encoding="utf-8")
print("ok", SIZE)
