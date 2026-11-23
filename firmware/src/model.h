// AI Monitor v0.2 — modelo de dados compartilhado entre os módulos.
// Os status alinham com o mapa da API do Hermes
// (~/workspace/miblo-clone/hermes-api-map.md, §3.2):
//   Hermes LiveSessionStatus -> status do monitor
//   idle      -> ST_IDLE
//   starting  -> ST_WORKING
//   waiting   -> ST_NEEDYOU   (agente parado aguardando o operador)
//   working   -> ST_WORKING
//   streaming -> ST_WORKING
//   resuming  -> ST_WORKING
//   message.complete (turno terminou) -> ST_FINISHED
#pragma once

#include <Arduino.h>
#include "config.h"

enum SessionStatus {
  ST_IDLE = 0,
  ST_WORKING,
  ST_NEEDYOU,   // precisa de você (âmbar, pisca)
  ST_FINISHED   // terminou (azul)
};

// Converte o status textual do Hub ("working", "needs-you", "finished",
// "idle", ou o status cru do Hermes) para o enum interno.
SessionStatus statusFromString(const String& s);

const char* statusLabel(SessionStatus st);
uint16_t statusColor(SessionStatus st);

struct Session {
  String id;
  String title;       // ex.: "Refatorar o monitor"
  String source;      // "hermes" | "claude" | "codex" | "gemini" | ...
  String profile;     // perfil do Hermes (ex.: "default", "f2a-bot")
  String detail;      // "o que está fazendo agora" (última tool / status)
  SessionStatus status = ST_IDLE;
  uint32_t updatedAt = 0;   // millis() da última atualização
  // Uso (quando o Hub informar)
  uint32_t inputTokens = 0;
  uint32_t outputTokens = 0;
  float costUsd = 0.0f;
  bool hasUsage = false;
};

struct Approval {
  bool active = false;
  String requestId;
  String sessionId;
  String source;   // ferramenta (hermes, claude...) para o aviso falado
  String command;
  String description;
  String choices;  // ex.: "once,session,always,deny"
  uint32_t receivedAt = 0;
};

struct UsageLimits {
  // Limites de uso (5h / semana), quando o Hub informar.
  bool valid = false;
  int fiveHourUsedPct = 0;    // 0-100
  int weeklyUsedPct = 0;      // 0-100
  String fiveHourResets;      // ex.: "zera às 18:00"
  String weeklyResets;        // ex.: "zera segunda"
  String planRemaining;       // ex.: "$12.40 restantes"
};

// Estatísticas do computador (PC onde rodam as IAs), enviadas pelo Hub
// no frame "host" (ver ../PROTOCOL.md). Campos best-effort chegam como
// "ausentes" quando o PC não tem o recurso (ex.: sem GPU, sem bateria).
struct HostStats {
  bool valid = false;       // CPU/MEM conhecidos (amostra já chegou)
  int cpuPct = 0;           // 0-100
  int memPct = 0;           // 0-100
  bool hasGpu = false;
  int gpuPct = 0;           // 0-100
  String topName;           // processo que mais consome CPU (ex.: "Chrome")
  int topPct = -1;          // -1 = sem dados
  bool hasNet = false;
  float netDownMbps = 0.0f;
  float netUpMbps = 0.0f;
  bool hasBattery = false;
  int battPct = 0;          // 0-100
  int battMins = -1;        // minutos restantes, -1 = desconhecido
};

struct AppState {
  Session sessions[MAX_SESSIONS];
  int sessionCount = 0;
  Approval approval;
  UsageLimits limits;
  HostStats host;           // estatísticas do PC (frame "host" do Hub)
  String lastFinishedTitle;   // última tarefa terminada
  uint32_t lastFinishedMs = 0;  // millis() da última tarefa terminada
  uint32_t lastActivityMs = 0;  // última mudança de sessão ou toque
  bool hubConnected = false;
  bool hubEverConnected = false;
  bool mockMode = false;
  String hubHost;
  uint16_t hubPort = HUB_DEFAULT_PORT;
  String hubToken;              // segredo de pareamento (Preferences "hubtok")
  bool hubActions = false;      // o Hub aceita aprovar pelo toque? (hello)
  String approvalDismissedId;   // aprovação que o usuário mandou "ver depois"
  int listPage = 0;
  uint8_t volumePct = 100;      // volume do alto-falante (Configurações; Preferences "vol")             // página da lista (3 sessões por página)
  // Sons pendentes (o loop toca; o parser só marca).
  // Fila de avisos falados (um por evento, tocados em ordem). Antes eram
  // contadores: vários eventos seguidos viravam um só aviso.
  static const uint8_t SND_Q = 10;
  uint8_t sndKind[SND_Q];       // SND_NEEDYOU / SND_FINISHED / SND_NEW
  String sndSrc[SND_Q];
  uint8_t sndHead = 0, sndCount = 0;
  // Demonstração (mensagem "demo" do Hub): tela pedida e rosto fixo.
  int8_t demoScreen = -1;       // -1 nada; 0 lista, 1 computador, 2 rosto
  int8_t demoMood = -1;         // -1 = rosto normal; senão FaceMood
  uint32_t snapshots = 0;       // retratos recebidos desde a conexão
  String wifiSsid;
  String ipAddress;
  uint32_t lastTouchMs = 0;
  int selectedSession = -1;   // índice da sessão no detalhe, -1 = lista
};

enum SndKind : uint8_t { SND_NEEDYOU = 0, SND_FINISHED = 1, SND_NEW = 2 };
// Enfileira um aviso (descarta o mais antigo se a fila encher).
void sndPush(AppState& st, uint8_t kind, const String& src);
// Tira o próximo aviso; false se a fila está vazia.
bool sndPop(AppState& st, uint8_t& kind, String& src);


Session* findSession(AppState& st, const String& id);
void upsertSession(AppState& st, const Session& s);
void clearFinishedOld(AppState& st);
void replaceSessions(AppState& st, const Session* incoming, int n);
int countStatus(const AppState& st, SessionStatus s);
