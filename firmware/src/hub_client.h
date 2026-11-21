// AI Monitor v0.2 — cliente do Hub (WebSocket) + modo demonstração.
// Protocolo em ../PROTOCOL.md. A conexão leva o token de pareamento.
// Sem Hub configurado, o modo demonstração simula 3 sessões.
#pragma once

#include <Arduino.h>
#include "model.h"

// Callbacks que o main registra para reagir a eventos vindos do Hub.
struct HubCallbacks {
  void (*onApproval)(const Approval& ap) = nullptr;
  void (*onApprovalCancelled)(const String& requestId) = nullptr;
  void (*onFinished)(const Session& s) = nullptr;
};

void hubBegin(AppState& st, HubCallbacks cb);
// Chamar em loop. Conecta/reconecta o WS e alimenta o modo mock.
void hubLoop(AppState& st);
// Envia resposta de aprovação ao Hub (só tem efeito com Hub conectado).
// choice: "once" | "session" | "always" | "deny"
bool hubSendApproval(AppState& st, const String& requestId,
                     const String& choice);
// Envia texto a uma sessão (prompt.submit do Hermes via Hub).
bool hubSendMessage(AppState& st, const String& sessionId, const String& text);
// Processa um frame JSON recebido do Hub (também usado por testes locais).
void hubHandleJson(AppState& st, const String& payload, HubCallbacks& cb);
