# AI Monitor v0.2 — monitor de sessões de IA com a Nous-Girl

> **Leia antes de gravar.** Este firmware **substitui** o Nous-Girl/XiaoZhi
> que está no Stack-chan hoje: some a voz, o "Hey Jarvis" e a conversa com
> a Hermione. O rosto da Nous-Girl vem junto (tela ociosa e avatar), mas
> sem voz. Para voltar, siga
> `%LOCALAPPDATA%/StackChan-Recovery/RESTAURAR.md` (backup completo da
> flash, verificado por SHA-256). M5Burner/UIFlow2 **não** restaura o
> Nous-Girl.

## 1. Compilar e gravar (CoreS3)

```bash
cd firmware
pio run                  # compila
pio run -t upload        # grava a app + bootloader + partições pelo USB
```

Não grave binário solto em `0x0`: o `firmware.bin` da v0.1 é só a app
(foi removido do pacote). Use o `pio run -t upload`.

## 2. Primeira configuração

O aparelho abre a rede `AIMonitor-Setup`. Conecte, abra
`http://192.168.4.1`, escolha a rede, a senha, o IP do Hub (porta 8790)
e o **token do Hub** (aparece no console do Hub na 1a execução).
Sem Hub configurado, roda em modo demonstração.

## 3. Rodar o Hub no notebook (onde rodam Claude Code, Hermes e Timon)

```bash
cd hub
python3 -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt
./run.sh
```

Padrão seguro: pareamento por token, **só leitura** (aprovar pelo toque e
enviar texto ficam desligados; ver `hub/README.md`). Token do Hermes em
arquivo (`file:`), nunca no YAML.

## O que mudou na v0.2

- Hub: pareamento por token, só leitura por padrão, `PreToolUse` não
  bloqueia mais, aprovação via `PermissionRequest` (opt-in), timeout que
  devolve a decisão ao terminal, teste que falhava corrigido.
- Firmware: tema espaço (roxo/azul), quadro em memória sem cintilação,
  rosto animado da Nous-Girl por estado (pensando, ouvindo, feliz, triste,
  dormindo, olhadinhas), avatar no cabeçalho, lista paginada, botões de
  aprovação só quando o Hub permite, envio do token, sessões que somem
  quando o Hub as remove, reconexão do WebSocket corrigida.
- Arte da Nous-Girl: homenagem de fã, uso pessoal e não comercial
  (`firmware/assets/nousgirl/TRIBUTE-README.md`).
