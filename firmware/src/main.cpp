// AI Monitor v0.2 — main: boot, máquina de estados e loop principal.
// Telas: lista -> detalhe -> aprovação (âmbar) -> rosto da Nous-Girl
//        (após 45 s sem toque) -> COMPUTADOR (toque no rodapé) ->
//        CONFIGURACOES (pressão longa de 2 s no cabeçalho).
// Hardware: M5Stack CoreS3 (ESP32-S3, 320x240 touch, 8 MB PSRAM).
#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>

#include "config.h"
#include "face.h"
#include "gfx.h"
#include "hub_client.h"
#include "model.h"
#include "servo_head.h"
#include "ui.h"
#include "voice.h"
#include "wifi_setup.h"

AppState g_app;
AppState* g_state = &g_app;  // usado pelo hub_client.cpp

static Screen g_screen = SCR_LIST;
static bool g_alertPending = false;  // beep + aceno uma vez
static bool g_finishedPending = false;  // "plim" de tarefa terminada
// Tela Computador: toque curto volta; segurar 2 s fixa/solta (fixada, o
// rosto não entra e toque curto não sai; avisos continuam tomando a tela).
static bool g_statsPinned = false;
static uint32_t g_statsPressStart = 0;
static bool g_statsHoldDone = false;
static uint32_t g_toastUntil = 0;
static String g_toast;
static uint32_t g_headerPressStart = 0;
static uint32_t g_lastGlance = 0;
static uint32_t g_glanceUntil = 0;
static FaceMood g_glanceMood = MOOD_LOOK_LEFT;

// ---------------------------------------------------------------------------
// Callbacks do Hub
// ---------------------------------------------------------------------------
static void onApprovalCb(const Approval& ap) {
  (void)ap;
  g_alertPending = true;  // beep + aceno no loop (não bloquear o WS)
  g_app.lastTouchMs = millis();
  g_app.lastActivityMs = millis();
  g_app.approvalDismissedId = "";
}

static void onApprovalCancelledCb(const String& requestId) {
  (void)requestId;
  if (g_screen == SCR_APPROVAL) g_screen = SCR_LIST;
}

static void onFinishedCb(const Session& s) {
  g_app.lastFinishedTitle = s.title;
  g_app.lastFinishedMs = millis();
  g_app.lastActivityMs = millis();
  g_finishedPending = true;  // "plim" no loop
}

// ---------------------------------------------------------------------------
// Alertas
// ---------------------------------------------------------------------------
// Sons (não bloqueiam: o M5.Speaker toca em segundo plano; a fila só
// avança quando o som anterior terminou).
//   precisa de você : dois toques subindo + aceno (repete a cada 60 s
//                     enquanto houver pendência, até 5 vezes)
//   terminou        : "plim" agudo
//   sessão nova     : toque curto e baixo
// Todos os sons passam pela tarefa de voz (microfone e alto-falante dividem o I2S).
static void alertNow() {
  voiceTone(880, 140, 10);
  voiceTone(1318, 200);
  servoNod();
}

static void chimeFinished() {
  voiceTone(1568, 90, 10);
  voiceTone(2093, 140);
}

static void tickNew() { voiceTone(660, 60); }

static uint32_t g_lastNagMs = 0;
static uint8_t g_nags = 0;

static void playPendingSounds(uint32_t now) {
  if (voiceSoundBusy()) return;
  if (g_app.sndNeedYou || g_alertPending) {
    g_app.sndNeedYou = 0;
    g_alertPending = false;
    g_lastNagMs = now;
    g_nags = 0;
    alertNow();
    return;
  }
  if (g_finishedPending) {
    g_finishedPending = false;
    chimeFinished();
    return;
  }
  if (g_app.sndNew) {
    g_app.sndNew = 0;
    tickNew();
    return;
  }
  // Lembrete enquanto algo espera você.
  bool waiting = g_app.approval.active || countStatus(g_app, ST_NEEDYOU) > 0;
  if (!waiting) {
    g_nags = 0;
    return;
  }
  if (g_nags < 5 && now - g_lastNagMs > 60000UL) {
    g_lastNagMs = now;
    g_nags++;
    alertNow();
  }
}

static void showToast(const String& msg, uint32_t ms = 2200) {
  g_toast = msg;
  g_toastUntil = millis() + ms;
}

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------
static void runPortalForever() {
  // O portal desenha direto no painel (não usa o quadro em memória).
  wifiStartSetupPortal();
  while (true) wifiPortalLoop();  // só sai com ESP.restart() após salvar
}

void setup() {
  Serial.begin(115200);
  auto cfg = M5.config();
  M5.begin(cfg);
  // Paisagem 320x240 no StackChan: escolhe a rotação que dá largura > altura.
  M5.Display.setRotation(DISPLAY_ROTATION);
  if (M5.Display.width() < M5.Display.height()) M5.Display.setRotation((DISPLAY_ROTATION + 1) % 4);
  M5.Display.setBrightness(160);
  M5.Speaker.setVolume(SPEAKER_VOLUME);
  Serial.printf("[tela] rotacao %d -> %dx%d\n", M5.Display.getRotation(),
                M5.Display.width(), M5.Display.height());
  gfxBegin();
  faceBegin();
  uiBootScreen("iniciando...");
  servoBegin();

  if (!wifiLoadSaved(g_app)) {
    uiBootScreen("sem Wi-Fi: abrindo portal...");
    delay(800);
    runPortalForever();
  }
  uiBootScreen("conectando ao Wi-Fi...");
  if (!wifiConnect(g_app)) {
    // Falhou (o wifiConnect já mostrou o erro): volta ao portal para
    // corrigir a senha. As credenciais erradas são apagadas.
    wifiForget();
    runPortalForever();
  }
  wifiStartMDNS();

  HubCallbacks cb;
  cb.onApproval = onApprovalCb;
  cb.onApprovalCancelled = onApprovalCancelledCb;
  cb.onFinished = onFinishedCb;
  hubBegin(g_app, cb);
  // Voz: o mesmo Hub (host e token), porta do /voice.
  voiceBegin(g_app.mockMode ? String() : g_app.hubHost, VOICE_DEFAULT_PORT, g_app.hubToken);

  g_app.lastTouchMs = millis();
  g_app.lastActivityMs = millis();
  Serial.println("[main] AI Monitor v" DELLMONITOR_VERSION " pronto");
}

// Resolve uma aprovação pelo toque (só se o Hub permitir).
static void resolveApproval(const String& choice) {
  String rid = g_app.approval.requestId;
  if (g_app.mockMode) {
    showToast("demonstracao: nada foi enviado");
  } else if (hubSendApproval(g_app, rid, choice)) {
    showToast(choice == "deny" ? "negado - enviado ao Hub" : "aprovado 1x - enviado ao Hub");
  } else {
    showToast("Hub so leitura: responda no computador");
    g_app.approvalDismissedId = rid;
    g_screen = SCR_LIST;
    return;
  }
  g_app.approval.active = false;
  Session* s = findSession(g_app, g_app.approval.sessionId);
  if (s) {
    s->status = ST_WORKING;
    s->detail = choice == "deny" ? "negado pelo operador" : "aprovado, continuando...";
  }
  g_screen = SCR_LIST;
  g_app.lastTouchMs = millis();
}

static void updateFace(uint32_t now) {
  uint16_t ink;
  FaceMood m = uiMoodFor(g_app, now, ink);
  // Conversa por voz: o rosto mostra ouvindo / pensando / falando.
  switch (voiceState()) {
    case VOICE_LISTENING: m = MOOD_LISTENING; ink = INK_WORKING; break;
    case VOICE_THINKING: m = MOOD_THINKING; ink = INK_WORKING; break;
    case VOICE_SPEAKING: m = MOOD_HAPPY; ink = INK_FINISHED; break;
    default: break;
  }
  // Ociosa: de vez em quando dá uma olhadinha para o lado.
  if (m == MOOD_NEUTRAL) {
    if (g_glanceUntil && now < g_glanceUntil) {
      m = g_glanceMood;
    } else if (now - g_lastGlance > FACE_GLANCE_EVERY_MS) {
      g_lastGlance = now;
      g_glanceUntil = now + 3500;
      g_glanceMood = (g_glanceMood == MOOD_LOOK_LEFT) ? MOOD_LOOK_RIGHT : MOOD_LOOK_LEFT;
      m = g_glanceMood;
    } else {
      g_glanceUntil = 0;
    }
  }
  faceSetMood(m, ink);
  faceUpdate(now);
  uiDrawFaceOverlay(g_app, now);
}

static uint32_t g_loopMax = 0, g_loopLog = 0, g_loopPrev = 0;

void loop() {
  M5.update();
  uint32_t now = millis();
  if (g_loopPrev) {
    uint32_t dt = now - g_loopPrev;
    if (dt > g_loopMax) g_loopMax = dt;
  }
  g_loopPrev = now;
  if (now - g_loopLog > 10000UL) {
    Serial.printf("[diag] tela %d, loop max %lums, wifi %d dBm, hub %d, voz %d/%d\n", (int)g_screen,
                  (unsigned long)g_loopMax, WiFi.RSSI(), g_app.hubConnected ? 1 : 0,
                  voiceConnected() ? 1 : 0, (int)voiceState());
    g_loopMax = 0;
    g_loopLog = now;
  }

  int tx = 0, ty = 0;
  bool touched = uiReadTouch(tx, ty);
  if (touched) {
    g_app.lastTouchMs = now;
    g_app.lastActivityMs = now;
  }

  // Pressão longa (2 s) no cabeçalho: Configurações.
  auto td = M5.Touch.getDetail();
  if (td.isPressed() && td.y < HDR_H && g_screen != SCR_FACE && g_screen != SCR_STATS) {
    if (g_headerPressStart == 0) g_headerPressStart = now;
    if (now - g_headerPressStart > 2000UL) {
      g_headerPressStart = 0;
      g_screen = SCR_SETTINGS;
      touched = false;
    }
  } else {
    g_headerPressStart = 0;
  }

  playPendingSounds(now);

  // "Hey Hermes": vai para o rosto (ouvindo), exceto se um pedido de aprovação está na tela.
  static uint32_t s_wakes = 0;
  if (voiceWakeCount() != s_wakes) {
    s_wakes = voiceWakeCount();
    if (g_screen != SCR_APPROVAL) {
      g_screen = SCR_FACE;
      faceInvalidate();
    }
  }

  hubLoop(g_app);

  // Aprovação nova assume a tela (exceto se o usuário mandou "ver depois").
  if (g_app.approval.active && g_screen != SCR_APPROVAL &&
      g_app.approvalDismissedId != g_app.approval.requestId) {
    g_screen = SCR_APPROVAL;
  }
  if (!g_app.approval.active && g_screen == SCR_APPROVAL) g_screen = SCR_LIST;

  // Rosto da Nous-Girl em tela cheia após um tempo sem toque.
  if (g_screen != SCR_FACE && g_screen != SCR_APPROVAL &&
      !(g_screen == SCR_STATS && g_statsPinned) &&
      now - g_app.lastTouchMs > FACE_IDLE_TIMEOUT_MS) {
    g_screen = SCR_FACE;
    faceInvalidate();
  }

  switch (g_screen) {
    case SCR_FACE: {
      if (touched && voiceState() == VOICE_SPEAKING) {
        voiceStopSpeaking();  // toque enquanto ela fala: para de falar
        touched = false;
        updateFace(now);
        break;
      }
      if (touched) {
        g_screen = SCR_LIST;
        touched = false;  // o toque que acorda não clica em nada
        break;
      }
      updateFace(now);
      break;
    }
    case SCR_LIST: {
      uiDrawBackground();
      uiDrawHeader(g_app, false);
      int hit = uiDrawList(g_app, touched, tx, ty);
      uiDrawFooter(g_app);
      if (touched && ty >= FOOTER_Y) {
        g_screen = SCR_STATS;
        g_statsPressStart = now;   // o toque que abriu não conta ao soltar
        g_statsHoldDone = true;
      } else if (hit == -2) {
        g_app.listPage++;
      } else if (hit >= 0) {
        g_app.selectedSession = hit;
        g_screen = SCR_DETAIL;
      }
      break;
    }
    case SCR_DETAIL: {
      uiDrawBackground();
      uiDrawHeader(g_app, false);
      int act = uiDrawDetail(g_app, touched, tx, ty);
      if (act == 1) g_screen = SCR_LIST;
      else if (act == 2) resolveApproval("once");
      else if (act == 4) resolveApproval("deny");
      break;
    }
    case SCR_APPROVAL: {
      int act = uiDrawApproval(g_app, touched, tx, ty);
      if (act == 2) resolveApproval("once");
      else if (act == 4) resolveApproval("deny");
      else if (act == 5) {
        g_app.approvalDismissedId = g_app.approval.requestId;
        g_screen = SCR_LIST;
      }
      break;
    }
    case SCR_STATS: {
      uiDrawStats(g_app, false, tx, ty);
      if (td.isPressed()) {
        if (g_statsPressStart == 0) {
          g_statsPressStart = now;
          g_statsHoldDone = false;
        }
        if (!g_statsHoldDone && now - g_statsPressStart > 2000UL) {
          g_statsHoldDone = true;
          g_statsPinned = !g_statsPinned;
          voiceTone(g_statsPinned ? 1318 : 880, 80);
          g_app.lastTouchMs = now;
        }
      } else if (g_statsPressStart != 0) {
        if (!g_statsHoldDone && !g_statsPinned) g_screen = SCR_LIST;
        g_statsPressStart = 0;
        g_statsHoldDone = false;
      }
      if (g_statsPinned) uiDrawPinBadge();
      break;
    }
    case SCR_SETTINGS: {
      uiDrawBackground();
      uiDrawHeader(g_app, false);
      int act = uiDrawSettings(g_app, touched, tx, ty);
      if (act == 1) {
        g_screen = SCR_LIST;
      } else if (act == 2) {
        uiBootScreen("abrindo portal de Wi-Fi...");
        delay(500);
        runPortalForever();
      } else if (act == 3) {
        uiBootScreen("esquecendo Wi-Fi...");
        wifiForget();
        delay(500);
        ESP.restart();
      }
      break;
    }
  }

  if (g_toastUntil && now < g_toastUntil) uiDrawToast(g_toast);
  gfxPresent();
  delay(g_screen == SCR_FACE ? 10 : 33);  // ~30 fps nas telas
}
