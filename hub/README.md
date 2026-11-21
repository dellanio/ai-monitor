# hub-stack-chan (AI Monitor) v0.2

O Hub é o programa que roda no computador e faz a ponte entre as IAs e o
Stack-chan. O aparelho é uma tela burra: é o Hub que observa as sessões
(Hermes, Claude Code) e empurra o estado para o dispositivo via WebSocket
na rede local — e recebe os toques de aprovar/negar e entrega a resposta
à IA certa. Fala o protocolo documentado em
`../firmware/PROTOCOL.md`, exatamente como o firmware espera.

```
[Stack-chan ESP32-S3] ⇄ ws://<pc>:8790 ⇄ [Hub] ⇄ 127.0.0.1:9119 ⇄ [hermes dashboard]
                                              └─ ~/.claude/projects + hooks HTTP :8767 ⇄ [Claude Code]
```

## ⚠️ Antes de tudo: o token do dashboard (leia isto)

**Verificado ao vivo em 2026-11-21 (dashboard 0.22.7):** o mapa da API
(`../hermes-api-map.md` §1) dizia que em loopback não há autenticação.
**Isso está errado para quase todas as rotas.** `/api/health` e
`/api/status` são públicos, mas `/api/profiles/sessions`,
`/api/cron/jobs` e o WebSocket `/api/ws` exigem o **token de sessão do
dashboard**: sem ele, REST devolve **401** e o WS recusa o handshake
com **403**. O token é sorteado a cada subida do dashboard e morre com o
processo — a única forma de o Hub conhecê-lo é subir o dashboard com um
token conhecido:

```bash
# 1. Suba o dashboard com um token fixo (escolha um valor só seu):
HERMES_DASHBOARD_SESSION_TOKEN=<um-token-secreto-só-seu> hermes dashboard

# 2. Diga o mesmo token ao Hub, em config.yaml:
#    hermes:
#      token: "<um-token-secreto-só-seu>"
#    (ou hermes.token: "file:/caminho/do/arquivo-com-o-token")
```

Sem o token, o Hub registra o erro explicando o problema e desliga a
fonte Hermes nesta execução (não fica tentando em loop). O resto (Claude
Code, servidor do dispositivo) continua funcionando.

## Instalar e rodar

```bash
cd ~/workspace/miblo-clone/hub
python3 -m venv .venv && source .venv/bin/activate   # recomendado
pip install -r requirements.txt
cp config.example.yaml config.yaml   # config.yaml fica fora do git; ajuste caminhos (tokens via file:)
./run.sh --config config.local.yaml
# ou: python3 -m hub --config config.yaml
```

O Hub escuta o dispositivo em `0.0.0.0:8790` (configurável) e anuncia
`hub-stack-chan._dellmonitor-hub._tcp.local.` via mDNS. No firmware,
aponte o Hub no portal de configuração (host = IP do PC, porta 8790).

## Segurança (v0.2)

- **Pareamento obrigatório.** O WebSocket do aparelho exige `?token=`;
  sem ele o handshake recebe 401. O token fica em
  `~/.dellmonitor/device_token` (gerado na 1a execução, impresso no
  console) ou em `device.token` / `DELLMONITOR_DEVICE_TOKEN`. Digite-o no
  portal de Wi-Fi do aparelho.
- **Só leitura por padrão.** `device.allow_actions: false` faz o Hub
  ignorar `approve`; `device.allow_messages: false` ignora `message`
  (que viraria `prompt.submit` no Hermes). Ligue só se precisar.
- **Token do Hermes fora do YAML.** Use `token: "file:..."`.
- O Hub escuta a LAN (`0.0.0.0:8790`): rode-o no PC/mini PC da mesma rede
  do aparelho e não exponha a porta na internet.

## Ligar o Claude Code (hooks)

O Hub expõe `POST http://127.0.0.1:8767/hooks/claude`. Mescle em
`~/.claude/settings.json` (não apague outras chaves):

```json
{
  "hooks": {
    "PermissionRequest": [
      { "hooks": [{ "type": "command", "command": "python3 /caminho/hub/hooks/claude-hook.py", "timeout": 180 }] }
    ],
    "Stop": [
      { "hooks": [{ "type": "command", "command": "python3 /caminho/hub/hooks/claude-hook.py" }] }
    ],
    "SessionStart": [
      { "hooks": [{ "type": "command", "command": "python3 /caminho/hub/hooks/claude-hook.py" }] }
    ],
    "Notification": [
      { "hooks": [{ "type": "command", "command": "python3 /caminho/hub/hooks/claude-hook.py" }] }
    ]
  }
}
```

Como funciona:

- `PermissionRequest` só dispara quando o Claude Code **ia perguntar** ao
  usuário (as regras de permissão dele continuam valendo). Em modo só
  leitura o Hub marca a sessão como "precisa de você" e devolve sem
  decidir: o prompt normal aparece no terminal. Com `allow_actions: true`
  o aparelho mostra Negar / Depois / Aprovar 1x; sem toque em
  `approval.timeout_s` (120 s) o terminal decide.
- **Não registre `PreToolUse`.** Ele dispara em toda ferramenta; a v0.1
  travava cada chamada esperando o toque. Se ainda estiver registrado, o
  Hub agora só atualiza o status e nunca bloqueia.
- `Stop` marca a sessão como terminada (aviso azul e rosto feliz).
- Hub fora do ar: o hook sai em silêncio e nada muda no Claude Code.

## Timon (missões dos agentes)

Com `timon.enabled: true` o Hub lê `GET /api/missions` do daemon local do
Timon (`127.0.0.1:3010`) a cada 5 s e mostra cada missão com a letra **T**
(dourada):

| Missão no Timon | Na tela |
|---|---|
| `pending_action_at` preenchido | precisa de você (âmbar) |
| turno ativo | trabalhando |
| viva, turno acabou | parada |
| concluída/falhou nos últimos 10 min | terminou (aviso azul) |
| interrompida ou antiga | some |

O token é `~/.timon/daemon-token`, credencial de **operador** do Timon: o Hub
só faz GET e recusa `api_url` fora do loopback. Hub e Timon ficam na mesma
máquina (o notebook).

## Estatísticas do PC (tela COMPUTADOR)

O Hub amostra o computador a cada `host.interval_s` (padrão 2 s) e envia
o frame `host` ao dispositivo (ver `../firmware/PROTOCOL.md`):

- **CPU% e RAM%** — sempre, via `psutil` (dependência em
  `requirements.txt`).
- **GPU%** — best-effort, via `nvidia-smi`. Sem GPU NVIDIA ou sem o
  comando, o campo chega `null` e o firmware mostra `GPU --`.
- **Processo top** — o processo com mais CPU no momento (nome + %).
- **Rede** — download/upload em MB/s, taxa a partir de
  `net_io_counters` (a primeira amostra é `0.0`).
- **Bateria** — % e minutos restantes (`sensors_battery`). Desktop sem
  bateria envia `null`; carregando, os minutos são `null`.

O rodapé da lista mostra `CPU 34% MEM 61%` (ou `CPU --% MEM --%` sem
dados); tocar no rodapé abre a tela COMPUTADOR, com o selo de estado
(TRANQUILO / PESADO / BAIXA, decidido no firmware). Sem `psutil`
instalado, o Hub registra um aviso e as estatísticas ficam desligadas —
o resto continua funcionando.

## Testar sem o aparelho

```bash
pip install -r requirements-dev.txt
python3 -m pytest tests/ -v
```

Os testes usam um Hermes falso (mock do `/api/ws`) e um dispositivo
falso, e cobrem: sessão working → finished; aprovação do dispositivo
chegando ao Hermes; reconexão com aprovação aberta (replay via
`session.events.since`); watcher de JSONL do Claude Code; hook
PreToolUse com allow e com timeout (deny); estatísticas do PC — coleta
com psutil falso (CPU/RAM, processo top, taxa de rede, bateria) e
broadcast do frame `host` com todos os campos.

Teste ao vivo contra o dashboard de verdade (precisa dele no ar com o
token — ver seção do token):

```bash
HERMES_PROBE_TOKEN=<o-mesmo-token> python3 tests/live_e2e_check.py
```

## O que foi verificado ao vivo (2026-11-21)

Com o dashboard 0.22.7 (commit `29a61cb7`, o mesmo do mapa) no ar:

- `GET /api/health` → 200, `auth_required: false`.
- `GET /api/status` → 200, `gateway_running: false`, `active_sessions: 0`.
- **Sem token:** `GET /api/cron/jobs` e `/api/profiles/sessions` → **401**;
  handshake do `/api/ws` → **403**.
- **Com token** (`X-Hermes-Session-Token` no REST, `?token=` no WS):
  cron → 200 `[]`; sessions → 200 lista vazia; WS conecta, primeiro frame
  é `gateway.ready` no formato do mapa; `client.capabilities` aceito
  (a lista de `server_requests` inclui `approval` e `clarify`);
  `session.active_list` → `{"sessions": []}` (lista vazia, não falha);
  `usage.bars` → `{"ok": true, "available": false}`.
- **Hub de verdade contra o dashboard de verdade:** com o token, o Hub
  conecta, mantém o WS estável (6 s+ sem erros) e o dispositivo falso
  recebe `hello`, `sessions: []` e `cron: []` (`tests/live_e2e_check.py`).
- **NÃO testado ao vivo:** o round-trip de aprovação real (precisa de
  uma sessão de agente viva com um pedido de aprovação — não havia
  gateway rodando nem provedor de LLM configurado aqui) e o formato exato
  de `open_requests` no `session.events.since` (o mock cobre o formato do
  mapa). O Hub trata esses caminhos pelo protocolo documentado e os
  testes de mock passam, mas a confirmação com um agente rodando de
  verdade ainda falta.

## Estrutura

```
hub/
  config.py         config.yaml (+ env overrides)
  state.py          sessões + aprovações pendentes + payloads do PROTOCOL.md
  device_server.py  WS :8790 para o Stack-chan + mDNS
  hermes_client.py  WS JSON-RPC + REST do dashboard (fonte prioridade 1)
  host_stats.py     CPU/RAM/GPU/rede/bateria do PC (psutil + nvidia-smi)
  claude_source.py  watcher de ~/.claude/projects + hooks HTTP :8767 (prioridade 2)
  sources.py        interface Source (base para Codex/outras IAs)
  hub.py            orquestrador: fontes -> estado -> dispositivo -> respostas
  __main__.py       python3 -m hub
hooks/
  claude-hook.py    script do hook do Claude Code (só stdlib)
tests/
  test_hub.py       7 testes (mock Hermes + mock dispositivo + fixtures Claude)
  live_e2e_check.py check manual contra o dashboard real (precisa do token)
```

## Limites honestos da v0.1

1. **Hermes sem token não funciona** — ver a seção do token. O token
   muda a cada restart do dashboard, a menos que
   `HERMES_DASHBOARD_SESSION_TOKEN` esteja fixo no ambiente dele.
2. **Aprovação do Hermes não foi testada com um agente vivo** (ver
   acima). O fluxo segue o mapa §4 e passa nos testes de mock.
3. **Claude Code:** o watcher de JSONL é heurístico (último registro
   decide working/finished); os hooks são a fonte confiável. Enviar
   texto do dispositivo para uma sessão Claude não existe na v0.1 (o
   Miblo faz isso digitando no terminal via tmux — aqui, fora de escopo).
4. **`clarify` do Hermes** (pergunta do agente, não aprovação de
   comando) aparece só como `needs-you` na lista — o dispositivo não
   tem teclado para respondê-la; responda no terminal.
5. **Codex e outras IAs não implementadas** — a interface `Source`
   (`hub/sources.py`) é o ponto de extensão; o próximo passo é um
   `CodexSource` no mesmo molde do `ClaudeSource`.
6. **Sem TLS/autenticação no WS do dispositivo** — o Hub escuta na LAN
   e confia na rede local, como o Miblo. Não exponha a porta 8790 para
   fora da sua rede.
7. **Limites de uso do Hermes** (`usage.bars`) voltam
   `available: false` quando o provider não tem billing — nesse caso o
   dispositivo mostra "sem dados do Hub" (conforme o PROTOCOL.md).
8. **Estatísticas do PC são best-effort**: GPU só via `nvidia-smi`
   (PCs sem GPU NVIDIA mostram `GPU --`); bateria só existe em
   notebooks; a taxa de rede da primeira amostra é `0.0`; o processo
   top depende de uma amostragem prévia do psutil.
