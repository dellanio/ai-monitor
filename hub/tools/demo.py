r"""Demonstração do AI Monitor para gravar vídeo (Hermes + Claude).

Roteiro: 1) Hermes e Claude começam (voz); 2) o Claude pede uma decisão
(tela âmbar + "Claude precisa de você"); 3) os dois terminam (voz);
4) rostos da Nous-Girl; 5) tela principal; 6) tela Computador (CPU/RAM).
Usa só sessões falsas "demo-*" pelo Hub local; no fim (ou Ctrl+C) apaga tudo.

    hub\.venv\Scripts\python.exe hub\tools\demo.py
    hub\.venv\Scripts\python.exe hub\tools\demo.py --limpar
"""
import json
import sys
import time
import urllib.request

URL = "http://127.0.0.1:8767/hooks/claude"
ROSTOS = [  # (mood, nome) — ver firmware/src/face.h
    (0, "neutra"), (2, "feliz"), (3, "ouvindo"), (1, "pensando"),
    (4, "triste"), (6, "olhando para a esquerda"), (7, "olhando para a direita"), (5, "dormindo"),
]


def ev(event, **kw):
    data = json.dumps({"hook_event_name": event, **kw}).encode()
    req = urllib.request.Request(URL, data=data, headers={"Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            r.read()
    except OSError as exc:  # Hub lento num passo: segue o roteiro
        print(f"  (aviso: {event} demorou/falhou: {exc})", flush=True)


def passo(seg, texto):
    print(f"  {texto}", flush=True)
    time.sleep(seg)


def main():
    ev("DemoClear")
    if "--limpar" in sys.argv:
        ev("DemoScreen", screen="list")
        print("demo apagada")
        return
    try:
        ev("DemoScreen", screen="list")
        espera = 5
        if "--espera" in sys.argv:
            espera = int(sys.argv[sys.argv.index("--espera") + 1])
        print(f"Demo do AI Monitor. Comece a filmar: inicio em {espera} s.", flush=True)
        time.sleep(espera)
        print("\n[1] Sessoes comecando")
        ev("DemoStart", session_id="demo-hermes", source="hermes", title="Resumo diario do Second Brain",
           profile="hermes", detail="lendo as notas do dia")
        passo(5, "Hermes comecou")
        ev("DemoStart", session_id="demo-claude", source="claude", title="Refatorar o login do app",
           profile="portal-imobiliario", detail="editando auth.ts")
        passo(6, "Claude comecou")

        print("\n[2] Decisao pendente")
        ev("DemoApproval", session_id="demo-claude", source="claude", command="git push origin main",
           description="Publicar a correcao do login em producao?")
        passo(12, "tela ambar + 'Claude precisa de voce'")
        ev("DemoApprovalEnd", session_id="demo-claude", source="claude")
        passo(3, "decisao resolvida, volta para a lista")

        print("\n[3] Terminando")
        ev("DemoFinish", session_id="demo-hermes", source="hermes", title="Resumo diario do Second Brain")
        passo(5, "Hermes terminou")
        ev("DemoFinish", session_id="demo-claude", source="claude", title="Refatorar o login do app")
        passo(5, "Claude terminou")

        print("\n[4] Rostos da Nous-Girl")
        for mood, nome in ROSTOS:
            ev("DemoScreen", screen="face", mood=mood)
            passo(3.5, nome)

        print("\n[5] Tela principal")
        ev("DemoScreen", screen="list")
        passo(6, "lista de sessoes")

        print("\n[6] Computador (CPU / RAM)")
        ev("DemoScreen", screen="stats")
        passo(8, "uso do notebook ao vivo")
        ev("DemoScreen", screen="list")
    except KeyboardInterrupt:
        pass
    finally:
        ev("DemoClear")
        print("\nfim: sessoes da demo apagadas", flush=True)


if __name__ == "__main__":
    main()
