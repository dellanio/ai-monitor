// AI Monitor v0.2 — cliente do Hub (WebSocket) + modo demonstração.
// O mock só roda quando NENHUM Hub foi configurado: com Hub configurado e
// fora do ar, a tela mostra "sem conexão" em vez de sessões inventadas.
#include "hub_client.h"
#include "config.h"

#include <WebSocketsClient.h>
#include <ArduinoJson.h>

static WebSocketsClient ws;
static HubCallbacks g_cb;
static bool wsStarted = false;
static uint32_t lastPingMs = 0;

// ---------------------------------------------------------------------------
// Modo mock
// ---------------------------------------------------------------------------
static uint32_t mockLastStepMs = 0;
static uint32_t mockLastAlertMs = 0;
static int mockPhase = 0;  // 0=working, 1=needs-you, 2=finished, 3=working...

static void mockSeed(AppState& st) {
  if (st.sessionCount > 0) return;
  Session a;
  a.id = "mock-hermes-1";
  a.title = "Refatorar o monitor";
  a.source = "hermes";
  a.profile = "default";
  a.detail = "tool: terminal (pytest)";
  a.status = ST_WORKING;
  upsertSession(st, a);
  Session b;
  b.id = "mock-claude-1";
  b.title = "Revisar PR #42";
  b.source = "claude";
  b.profile = "";
  b.detail = "lendo diff de ui.cpp";
  b.status = ST_WORKING;
  upsertSession(st, b);
  Session c;
  c.id = "mock-codex-1";
  c.title = "Docs do projeto";
  c.source = "codex";
  c.profile = "";
  c.detail = "aguardando vez";
  c.status = ST_IDLE;
  upsertSession(st, c);
  st.limits.valid = true;
  st.limits.fiveHourUsedPct = 62;
  st.limits.weeklyUsedPct = 34;
  st.limits.fiveHourResets = "zera as 18:00";
  st.limits.weeklyResets = "zera segunda";
  st.limits.planRemaining = "$12.40 restantes";
}

// Cicla as sessões falsas: working -> needs-you (alerta) -> finished ->
// working... Um alerta novo a cada MOCK_ALERT_EVERY_MS.
static int mockHostCycle = 0;  // para variar as estatísticas do PC

// Estatísticas do PC simuladas (modo mock): variam a cada ciclo e, de
// vez em quando, mostram um estado PESADO ou BAIXA para demonstrar a
// tela COMPUTADOR sem Hub.
static void mockHostStats(AppState& st) {
  mockHostCycle++;
  st.host.valid = true;
  if (mockHostCycle % 5 == 0) {
    // PESADO: CPU 92, RAM 78, Chrome 41%, GPU 71.
    st.host.cpuPct = 92;
    st.host.memPct = 78;
    st.host.hasGpu = true;
    st.host.gpuPct = 71;
    st.host.topName = "Chrome";
    st.host.topPct = 41;
    st.host.hasNet = true;
    st.host.netDownMbps = 48.0f;
    st.host.netUpMbps = 1.2f;
    st.host.hasBattery = false;
    st.host.battMins = -1;
  } else if (mockHostCycle % 5 == 3) {
    // BAIXA: bateria 12%, faltam 25 min.
    st.host.cpuPct = 18;
    st.host.memPct = 52;
    st.host.hasGpu = true;
    st.host.gpuPct = 9;
    st.host.topName = "hermes";
    st.host.topPct = 12;
    st.host.hasNet = true;
    st.host.netDownMbps = 2.4f;
    st.host.netUpMbps = 0.4f;
    st.host.hasBattery = true;
    st.host.battPct = 12;
    st.host.battMins = 25;
  } else {
    // TRANQUILO: CPU 25-64%, RAM 40-67%.
    st.host.cpuPct = 25 + (mockHostCycle * 13) % 40;
    st.host.memPct = 40 + (mockHostCycle * 7) % 28;
    st.host.hasGpu = true;
    st.host.gpuPct = 5 + (mockHostCycle * 11) % 30;
    st.host.topName = (mockHostCycle % 2 == 0) ? "Chrome" : "Code";
    st.host.topPct = 12 + (mockHostCycle * 5) % 25;
    st.host.hasNet = true;
    st.host.netDownMbps = 0.5f + (float)((mockHostCycle * 3) % 48);
    st.host.netUpMbps = 0.3f + (float)(mockHostCycle % 10) * 0.2f;
    st.host.hasBattery = false;
    st.host.battMins = -1;
  }
}

static void mockStep(AppState& st) {
  uint32_t now = millis();
  if (now - mockLastStepMs < 8000UL) return;
  mockLastStepMs = now;
  mockSeed(st);
  mockHostStats(st);

  // A cada ciclo, avança a fase da primeira sessão.
  Session* s = findSession(st, "mock-hermes-1");
  if (!s) return;
  mockPhase = (mockPhase + 1) % 4;
  switch (mockPhase) {
    case 0:
      s->status = ST_WORKING;
      s->detail = "tool: terminal (pytest)";
      break;
    case 1:
      // Alerta de permissão (respeita o intervalo mínimo entre alertas).
      if (now - mockLastAlertMs < MOCK_ALERT_EVERY_MS) break;
      mockLastAlertMs = now;
      s->status = ST_NEEDYOU;
      s->detail = "pedindo permissao: rm -rf /tmp/build";
      st.approval.active = true;
      st.approval.requestId = "mock-srq-1";
      st.approval.sessionId = s->id;
      st.approval.command = "rm -rf /tmp/build";
      st.approval.description = "Remove diretorio de build";
      st.approval.choices = "once,session,always,deny";
      st.approval.receivedAt = now;
      if (g_cb.onApproval) g_cb.onApproval(st.approval);
      break;
    case 2:
      s->status = ST_FINISHED;
      s->detail = "turno concluido";
      st.approval.active = false;
      if (g_cb.onFinished) g_cb.onFinished(*s);
      break;
    default:
      s->status = ST_WORKING;
      s->detail = "novo turno: revisar testes";
      break;
  }
  s->updatedAt = now;

  // A segunda sessão oscila de leve para a lista não ficar estática.
  Session* b = findSession(st, "mock-claude-1");
  if (b) {
    b->status = (b->status == ST_WORKING) ? ST_FINISHED : ST_WORKING;
    b->detail = (b->status == ST_WORKING) ? "editando ui.cpp"
                                          : "turno concluido";
    b->updatedAt = now;
  }
}

// ---------------------------------------------------------------------------
// Protocolo JSON do Hub (ver ../PROTOCOL.md)
// ---------------------------------------------------------------------------
void hubHandleJson(AppState& st, const String& payload, HubCallbacks& cb) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("[hub] JSON invalido: ");
    Serial.println(err.c_str());
    return;
  }
  const char* type = doc["type"] | "";
  String t = type;

  if (t == "sessions") {
    static Session incoming[MAX_SESSIONS];
    int n = 0;
    JsonArray arr = doc["sessions"].as<JsonArray>();
    for (JsonObject o : arr) {
      if (n >= MAX_SESSIONS) break;
      Session s;
      s.id = o["id"] | "";
      s.title = o["title"] | "";
      s.source = o["source"] | "";
      s.profile = o["profile"] | "";
      s.detail = o["detail"] | "";
      s.status = statusFromString(o["status"] | "idle");
      s.inputTokens = o["input_tokens"] | 0;
      s.outputTokens = o["output_tokens"] | 0;
      s.costUsd = o["cost_usd"] | 0.0f;
      s.hasUsage = !o["input_tokens"].isNull();
      if (s.id.length() == 0) continue;
      incoming[n++] = s;
    }
    replaceSessions(st, incoming, n);
  } else if (t == "approval") {
    Approval ap;
    ap.active = true;
    ap.requestId = doc["request_id"] | "";
    ap.sessionId = doc["session_id"] | "";
    ap.source = doc["source"] | "";
    ap.command = doc["command"] | "";
    ap.description = doc["description"] | "";
    JsonArray ch = doc["choices"].as<JsonArray>();
    String joined;
    for (JsonVariant v : ch) {
      if (joined.length()) joined += ",";
      joined += v.as<const char*>();
    }
    ap.choices = joined.length() ? joined : "once,deny";
    ap.receivedAt = millis();
    st.approval = ap;
    Session* s = findSession(st, ap.sessionId);
    if (s) {
      s->status = ST_NEEDYOU;
      s->updatedAt = millis();
    }
    if (cb.onApproval) cb.onApproval(ap);
  } else if (t == "approval_cancelled") {
    String rid = doc["request_id"] | "";
    if (st.approval.active && st.approval.requestId == rid) {
      st.approval.active = false;
    }
    if (cb.onApprovalCancelled) cb.onApprovalCancelled(rid);
  } else if (t == "demo") {
    String scr = doc["screen"] | "";
    st.demoMood = (int8_t)(doc["mood"] | -1);
    st.demoScreen = scr == "list" ? 0 : scr == "stats" ? 1 : scr == "face" ? 2 : -1;
    if (st.demoScreen != 2) st.demoMood = -1;
    Serial.printf("[demo] tela %s, rosto %d\n", scr.c_str(), st.demoMood);
  } else if (t == "finished") {
    String sid = doc["session_id"] | "";
    Session* s = findSession(st, sid);
    Serial.printf("[hub] finished %s (%s)\n", sid.c_str(), s ? "na lista" : "fora da lista");
    if (s) {
      s->status = ST_FINISHED;
      s->detail = "turno concluido";
      s->updatedAt = millis();
      if (cb.onFinished) cb.onFinished(*s);
    } else if (cb.onFinished) {  // ainda não chegou na lista (ou ficou fora do retrato): avisa mesmo assim
      Session tmp;
      tmp.id = sid;
      tmp.title = doc["title"] | "";
      tmp.source = doc["source"] | "";
      tmp.status = ST_FINISHED;
      cb.onFinished(tmp);
    }
  } else if (t == "usage") {
    String sid = doc["session_id"] | "";
    Session* s = findSession(st, sid);
    if (s) {
      s->inputTokens = doc["input_tokens"] | s->inputTokens;
      s->outputTokens = doc["output_tokens"] | s->outputTokens;
      s->costUsd = doc["cost_usd"] | s->costUsd;
      s->hasUsage = true;
    }
    // Limites do plano (5h/semana) podem vir junto.
    if (!doc["limits"].isNull()) {
      JsonObject lim = doc["limits"];
      st.limits.valid = true;
      st.limits.fiveHourUsedPct = lim["five_hour_pct"] | 0;
      st.limits.weeklyUsedPct = lim["weekly_pct"] | 0;
      st.limits.fiveHourResets = lim["five_hour_resets"] | "";
      st.limits.weeklyResets = lim["weekly_resets"] | "";
      st.limits.planRemaining = lim["plan_remaining"] | "";
    }
  } else if (t == "pong") {
    // resposta ao nosso ping de aplicação
  } else if (t == "cron") {
    // v0.1: jobs de cron recebidos, mas ainda sem tela própria.
    // Ficam disponíveis para a UI futura (ver PROTOCOL.md).
    Serial.println("[hub] cron recebido (sem tela em v0.1)");
  } else if (t == "host") {
    // Estatísticas do PC para o rodapé e a tela COMPUTADOR.
    // Campos best-effort (GPU, rede, bateria) podem vir null.
    st.host.valid = true;
    st.host.cpuPct = doc["host_cpu_pct"] | 0;
    st.host.memPct = doc["host_mem_pct"] | 0;
    st.host.hasGpu = !doc["host_gpu_pct"].isNull();
    st.host.gpuPct = doc["host_gpu_pct"] | 0;
    st.host.topName = doc["host_top_name"] | "";
    st.host.topPct = doc["host_top_pct"] | -1;
    st.host.hasNet = !doc["host_net_down_mbps"].isNull();
    st.host.netDownMbps = doc["host_net_down_mbps"] | 0.0f;
    st.host.netUpMbps = doc["host_net_up_mbps"] | 0.0f;
    st.host.hasBattery = !doc["host_batt_pct"].isNull();
    st.host.battPct = doc["host_batt_pct"] | 0;
    st.host.battMins = doc["host_batt_mins"] | -1;
  } else if (t == "hello") {
    st.hubActions = doc["actions"] | false;
    Serial.printf("[hub] hello do Hub (aprovar pelo toque: %s)\n",
                  st.hubActions ? "sim" : "nao");
  } else {
    Serial.print("[hub] tipo desconhecido: ");
    Serial.println(t);
  }
}

// ---------------------------------------------------------------------------
// WebSocket
// ---------------------------------------------------------------------------
static uint32_t lastRxMs = 0;

static void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  // O AppState é acessado via ponteiro global simples (definido em main.cpp).
  extern AppState* g_state;
  if (!g_state) return;
  switch (type) {
    case WStype_CONNECTED:
      g_state->hubConnected = true;
      g_state->hubEverConnected = true;
      g_state->snapshots = 0;
      g_state->mockMode = false;
      lastRxMs = millis();
      Serial.println("[hub] WS conectado");
      ws.sendTXT("{\"type\":\"hello\",\"device\":\"dellmonitor\",\"version\":\""
                 DELLMONITOR_VERSION "\"}");
      break;
    case WStype_DISCONNECTED:
      g_state->hubConnected = false;
      g_state->hubActions = false;
      g_state->approval.active = false;
      Serial.printf("[hub] WS desconectado (up %lus, heap %u)\n", millis() / 1000, ESP.getFreeHeap());
      break;
    case WStype_TEXT: {
      lastRxMs = millis();
      String msg((char*)payload, length);
      hubHandleJson(*g_state, msg, g_cb);
      break;
    }
    default:
      break;
  }
}

void hubBegin(AppState& st, HubCallbacks cb) {
  g_cb = cb;
  if (st.hubHost.length() == 0) {
    st.mockMode = true;
    Serial.println("[hub] sem Hub configurado -> modo demonstracao");
    return;
  }
  st.mockMode = false;
  ws.onEvent(onWsEvent);
  ws.setReconnectInterval(HUB_RECONNECT_MS);
  // Sem o heartbeat da biblioteca: ele derrubava a conexão (1006) a cada
  // ~30 s na tela de lista mesmo com Wi-Fi forte. Vida do link = o Hub manda
  // algo a cada 2 s (host); 30 s de silêncio -> reconecta.
  // O token vai na URL: o Hub recusa o handshake (401) sem ele.
  String path = "/?token=" + st.hubToken;
  Serial.printf("[hub] conectando em %s:%u\n", st.hubHost.c_str(), st.hubPort);
  ws.begin(st.hubHost.c_str(), st.hubPort, path.c_str());
  wsStarted = true;
}

void hubLoop(AppState& st) {
  if (st.mockMode) {
    mockStep(st);
    return;
  }
  if (!wsStarted) return;
  // A biblioteca conecta e reconecta dentro do loop(): precisa rodar
  // sempre, inclusive desconectado (a v0.1 nunca chamava aqui).
  ws.loop();
  uint32_t now = millis();
  if (st.hubConnected && now - lastRxMs > 30000UL) {
    Serial.println("[hub] 30 s sem nada do Hub -> reconectando");
    ws.disconnect();
    return;
  }
  if (st.hubConnected && now - lastPingMs > 15000UL) {
    lastPingMs = now;
    ws.sendTXT("{\"type\":\"ping\"}");
  }
}

static bool sendJson(String json) {
  if (!ws.isConnected()) return false;
  return ws.sendTXT(json);
}

bool hubSendApproval(AppState& st, const String& requestId,
                     const String& choice) {
  // Só com Hub conectado E com aprovação pelo toque ligada no Hub.
  if (!st.hubConnected || !st.hubActions) return false;
  JsonDocument doc;
  doc["type"] = "approve";
  doc["request_id"] = requestId;
  doc["choice"] = choice;
  String out;
  serializeJson(doc, out);
  return sendJson(out);
}

bool hubSendMessage(AppState& st, const String& sessionId,
                    const String& text) {
  if (!st.hubConnected) return false;
  JsonDocument doc;
  doc["type"] = "message";
  doc["session_id"] = sessionId;
  doc["text"] = text;
  String out;
  serializeJson(doc, out);
  return sendJson(out);
}
