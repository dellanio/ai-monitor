# Protocolo Hub ↔ AI Monitor (v0.1)

Transporte: **WebSocket** (`ws://<ip-do-pc>:<porta>/`, padrão 8790) entre o
Hub (no computador) e o dispositivo (ESP32-S3). JSON, um objeto por frame.
O dispositivo nunca fala direto com o Hermes, Claude Code ou Codex — só
com o Hub, pela rede local.

## Direção Hub → dispositivo

### `hello`
Handshake do Hub ao conectar.
```json
{"type": "hello", "hub": "dellmonitor-hub", "version": 1}
```

### `sessions` — lista completa de sessões (o Hub envia quando muda)
```json
{"type": "sessions", "sessions": [
  {"id": "abc123", "title": "Refatorar o monitor", "source": "hermes",
   "profile": "default", "status": "working",
   "detail": "tool: terminal (pytest)",
   "input_tokens": 12000, "output_tokens": 3400, "cost_usd": 0.42},
  {"id": "def456", "title": "Revisar PR #42", "source": "claude",
   "status": "needs-you", "detail": "pedindo permissao: rm -rf /tmp/build"}
]}
```
`status` aceito: `working`, `needs-you`, `finished`, `idle`.
Mapeamento a partir do Hermes (ver `../hermes-api-map.md`, §3.2):

| Hermes (ao vivo) | status no protocolo |
|---|---|
| `idle` | `idle` |
| `starting`, `working`, `streaming`, `resuming` | `working` |
| `waiting` (aguardando o operador) | `needs-you` |
| `message.complete` (turno terminou) | `finished` |

Para Claude Code / Codex / Gemini, o Hub traduz o estado próprio de cada
ferramenta para os mesmos quatro valores.

### `approval` — pedido de permissão (server→client, espelha o Hermes)
Segue o formato do frame `approval` do Hermes (`hermes-api-map.md`, §4.2),
traduzido para o protocolo do monitor:
```json
{"type": "approval", "session_id": "abc123",
 "request_id": "srq-a1b2c3d4e5f6",
 "command": "rm -rf /tmp/build",
 "description": "Remove diretório de build",
 "choices": ["once", "session", "always", "deny"]}
```
Ao receber, o dispositivo entra na tela de aprovação (âmbar), apita e
acena com a cabeça. O Hub deve reenviar aprovações abertas ao reconectar
(o Hermes devolve `open_requests` em `session.resume` /
`session.events.since` — o Hub repassa).

### `approval_cancelled` — pedido retirado (timeout, outra superfície resolveu)
```json
{"type": "approval_cancelled", "request_id": "srq-a1b2c3d4e5f6"}
```

### `finished` — turno terminou (aviso azul)
```json
{"type": "finished", "session_id": "abc123", "title": "Refatorar o monitor"}
```

### `usage` — tokens/custo e limites do plano
```json
{"type": "usage", "session_id": "abc123",
 "input_tokens": 12000, "output_tokens": 3400, "cost_usd": 0.42,
 "limits": {"five_hour_pct": 62, "weekly_pct": 34,
            "five_hour_resets": "zera as 18:00",
            "weekly_resets": "zera segunda",
            "plan_remaining": "$12.40 restantes"}}
```
Fonte no Hermes: evento `session.usage` e RPC `usage.bars`
(`hermes-api-map.md`, §6). Se o provider não tiver billing, o Hub omite
`limits` e o dispositivo mostra "limites: sem dados do Hub".

### `cron` — jobs de cron (v0.1: recebido, sem tela própria ainda)
```json
{"type": "cron", "jobs": [
  {"id": "daily-brief", "name": "daily-brief",
   "next_run_at": 1795083600, "last_status": "success",
   "timezone": "America/Sao_Paulo"}
]}
```
Fonte: `GET /api/cron/jobs?profile=all` (`hermes-api-map.md`, §2.3).
`next_run_at` é epoch em segundos.

### `host` — estatísticas do computador (tela COMPUTADOR + rodapé)
O Hub envia a cada ~2 s (e ao conectar o dispositivo):
```json
{"type": "host",
 "host_cpu_pct": 34, "host_mem_pct": 61,
 "host_gpu_pct": 9, "host_top_name": "Chrome", "host_top_pct": 41,
 "host_net_down_mbps": 48.0, "host_net_up_mbps": 1.2,
 "host_batt_pct": 12, "host_batt_mins": 25}
```
`host_cpu_pct` / `host_mem_pct` sempre vêm (psutil). Os demais são
best-effort e chegam `null` quando o PC não tem o recurso — o
dispositivo mostra "--" nesses casos:

| campo | fonte | `null` quando |
|---|---|---|
| `host_gpu_pct` | `nvidia-smi` (utilização da GPU) | sem GPU NVIDIA / sem `nvidia-smi` |
| `host_top_name` / `host_top_pct` | processo com mais CPU (psutil) | nenhum processo legível |
| `host_net_down_mbps` / `host_net_up_mbps` | taxa a partir de `net_io_counters` | primeira amostra (ainda sem taxa) |
| `host_batt_pct` / `host_batt_mins` | `sensors_battery` | desktop sem bateria; `batt_mins` também é `null` carregando |

O selo de estado da tela COMPUTADOR é decidido no firmware: **BAIXA**
quando há bateria e ela está < 20% (tem prioridade), **PESADO** quando
CPU >= 85%, senão **TRANQUILO**. Sem amostra, o rodapé mostra
`CPU --% MEM --%`.

## Direção dispositivo → Hub

### `hello`
```json
{"type": "hello", "device": "dellmonitor", "version": "0.1.0"}
```

### `approve` — resposta a um pedido de aprovação
```json
{"type": "approve", "request_id": "srq-a1b2c3d4e5f6", "choice": "once"}
```
`choice`: `once` | `session` | `always` | `deny`. O Hub traduz para a
resposta do frame `approval` do Hermes (`{"result": {"choice": ...}}`,
correlacionada pelo `id` `srq-*`).

### `message` — enviar texto a uma sessão
```json
{"type": "message", "session_id": "abc123", "text": "pode continuar"}
```
O Hub traduz para `prompt.submit` do Hermes.

### `ping`
```json
{"type": "ping"}
```
Keep-alive a cada 15 s (o Hub pode responder com `{"type": "pong"}`).

## Modo mock (sem Hub)

Sem Hub configurado (campo vazio no portal) ou sem conexão, o dispositivo
entra em modo mock: 3 sessões falsas ciclam entre `working` →
`needs-you` → `finished`, com um alerta a cada ~45 s. O mock também
simula as estatísticas do PC (CPU 25–64%, RAM 40–67%, GPU, rede e
processo top) e, de vez em quando, um estado PESADO (CPU 92%) ou BAIXA
(bateria 12%, faltam 25 min), para demonstrar a tela COMPUTADOR sem Hub.
Aprovar no mock só limpa o alerta localmente e mostra "sem Hub: acao nao enviada" — nada sai
do aparelho.
