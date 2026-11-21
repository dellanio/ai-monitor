"""Leitor mínimo de partição NVS (ESP-IDF) — só strings e inteiros.

Uso: python nvs_read.py <imagem_flash.bin> <offset_hex> <tamanho_hex> [--show-secrets]
Sem --show-secrets, valores de chaves com 'pass'/'pswd'/'tok' saem mascarados.
"""
import struct
import sys

PAGE = 4096
TYPES = {0x01: "u8", 0x11: "i8", 0x02: "u16", 0x12: "i16", 0x04: "u32", 0x14: "i32",
         0x08: "u64", 0x18: "i64", 0x21: "str", 0x41: "blob", 0x42: "blob_data", 0x48: "blob_idx"}


def read_nvs(data: bytes):
    namespaces = {0: ""}
    entries = []
    for p in range(0, len(data), PAGE):
        page = data[p:p + PAGE]
        state = struct.unpack_from("<I", page, 0)[0]
        if state not in (0xFFFFFFFE, 0xFFFFFFFC, 0xFFFFFFF8):
            continue
        bitmap = page[32:64]
        i = 0
        while i < 126:
            bits = (bitmap[i // 4] >> ((i % 4) * 2)) & 3
            if bits != 2:
                i += 1
                continue
            off = 64 + i * 32
            ns, typ, span, _chunk = page[off], page[off + 1], page[off + 2], page[off + 3]
            key = page[off + 8:off + 24].split(b"\0")[0].decode("ascii", "replace")
            raw = page[off + 24:off + 32]
            t = TYPES.get(typ, hex(typ))
            if t in ("str", "blob_data", "blob"):
                size = struct.unpack_from("<H", raw, 0)[0]
                blob = page[off + 32:off + 32 + size]
                val = blob.rstrip(b"\0").decode("utf-8", "replace") if t == "str" else blob
            elif t.startswith("u") or t.startswith("i"):
                n = {"8": 1, "16": 2, "32": 4, "64": 8}[t[1:]]
                fmt = {1: "B", 2: "H", 4: "I", 8: "Q"}[n]
                if t.startswith("i"):
                    fmt = fmt.lower()
                val = struct.unpack_from("<" + fmt, raw, 0)[0]
            else:
                val = raw
            if ns == 0 and t == "u8":
                namespaces[val] = key
            else:
                entries.append((ns, key, t, val))
            i += max(1, span)
    return [(namespaces.get(ns, f"ns{ns}"), k, t, v) for ns, k, t, v in entries]


def main():
    img, off, size = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
    show = "--show-secrets" in sys.argv
    with open(img, "rb") as f:
        f.seek(off)
        data = f.read(size)
    for ns, k, t, v in read_nvs(data):
        secret = any(s in k.lower() for s in ("pass", "pswd", "tok", "key", "secret"))
        if isinstance(v, bytes):
            v = f"<{len(v)} bytes>"
        elif secret and not show:
            v = f"<oculto, {len(str(v))} chars>"
        print(f"{ns:16} {k:16} {t:9} {v}")


if __name__ == "__main__":
    main()
