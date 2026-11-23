// AI Monitor v0.2 — main: boot, máquina de estados e loop principal.
// Telas: lista -> detalhe -> aprovação (âmbar) -> rosto da Nous-Girl
//        (após 45 s sem toque) -> COMPUTADOR (toque no rodapé) ->
//        CONFIGURACOES (pressão longa de 2 s no cabeçalho).
// Hardware: M5Stack CoreS3 (ESP32-S3, 320x240 touch, 8 MB PSRAM).
#include <Arduino.h>
#include "hw.h"
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
static String g_alertSrc;  // ferramenta do último "precisa de você" (lembrete)

static void onApprovalCb(const Approval& ap) {
  g_alertSrc = ap.source;
  sndPush(g_app, SND_NEEDYOU, ap.source);  // beep + aceno no loop (não bloquear o WS)
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
  sndPush(g_app, SND_FINISHED, s.source);  // "plim" no loop
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
// Primeiro tenta o som escolhido no notebook (Hub); sem ele, os toques de sempre.
// Ferramenta que espera você agora (para o lembrete repetido).
static String needYouSource() {
  if (g_app.approval.active && g_app.approval.source.length()) return g_app.approval.source;
  for (int i = 0; i < g_app.sessionCount; i++)
    if (g_app.sessions[i].status == ST_NEEDYOU) return g_app.sessions[i].source;
  return g_alertSrc;
}

static void alertNow() {
  String src = needYouSource();
  bool hub = voiceSound("needyou", src.c_str());
  Serial.printf("[som] precisa de voce: %s\n", hub ? "arquivo do Hub" : "toque embutido");
  if (!hub) {
    voiceTone(880, 140, 10);
    voiceTone(1318, 200);
  }
  servoNod();
}

static void chimeFinished(const String& src) {
  bool hub = voiceSound("finished", src.c_str());
  Serial.printf("[som] terminou: %s\n", hub ? "arquivo do Hub" : "toque embutido");
  if (hub) return;
  voiceTone(1568, 90, 10);
  voiceTone(2093, 140);
}

static void tickNew(const String& src) {
  if (!voiceSound("new", src.c_str())) voiceTone(660, 60);
}

static uint32_t g_lastNagMs = 0;
static uint8_t g_nags = 0;

static void playPendingSounds(uint32_t now) {
  if (voiceSoundBusy()) return;
  uint8_t kind;
  String src;
  if (sndPop(g_app, kind, src)) {
    if (kind == SND_NEEDYOU) {
      g_alertSrc = src;
      g_lastNagMs = now;
      g_nags = 0;
      alertNow();
    } else if (kind == SND_FINISHED) {
      chimeFinished(src);
    } else {
      tickNew(src);
    }
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
  hwBegin();
  Serial.println("[placa] " BOARD_NAME);
  // Paisagem 320x240 no StackChan: escolhe a rotação que dá largura > altura.
  DISP.setRotation(DISPLAY_ROTATION);
  if (DISP.width() < DISP.height()) DISP.setRotation((DISPLAY_ROTATION + 1) % 4);
  DISP.setBrightness(160);
  g_app.volumePct = settingsLoadVolume();
  voiceSetVolume(g_app.volumePct);
  Serial.printf("[tela] rotacao %d -> %dx%d\n", DISP.getRotation(),
                DISP.width(), DISP.height());
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
  if (g_app.demoMood >= 0 && g_app.demoMood < MOOD_COUNT) {
    m = (FaceMood)g_app.demoMood;
    static const uint16_t inks[MOOD_COUNT] = {INK_NEUTRAL, INK_WORKING, INK_FINISHED, INK_WORKING,
                                              INK_SAD, INK_SLEEPING, INK_NEUTRAL, INK_NEUTRAL};
    ink = inks[m];
  }
  faceSetMood(m, ink);
  faceUpdate(now);
  uiDrawFaceOverlay(g_app, now);
}

static uint32_t g_touchCount = 0;
static uint32_t g_loopMax = 0, g_loopLog = 0, g_loopPrev = 0;

void loop() {
  hwUpdate();
  uint32_t now = millis();
  if (g_loopPrev) {
    uint32_t dt = now - g_loopPrev;
    if (dt > g_loopMax) g_loopMax = dt;
  }
  g_loopPrev = now;
  if (now - g_loopLog > 10000UL) {
    if (now < 70000UL) Serial.printf("[placa] %s\n", hwInfo());
    extern uint32_t g_presentMaxUs, g_frames;
    Serial.printf("[diag] quadros %lu em 10 s, envio max %lums, toques %lu\n", (unsigned long)g_frames,
                  (unsigned long)(g_presentMaxUs / 1000), (unsigned long)g_touchCount);
    g_presentMaxUs = 0; g_frames = 0; g_touchCount = 0;
    Serial.printf("[diag] tela %d, loop max %lums, wifi %d dBm, hub %d, voz %d/%d\n", (int)g_screen,
                  (unsigned long)g_loopMax, WiFi.RSSI(), g_app.hubConnected ? 1 : 0,
                  voiceConnected() ? 1 : 0, (int)voiceState());
    g_loopMax = 0;
    g_loopLog = now;
  }

  int tx = 0, ty = 0;
  bool touched = uiReadTouch(tx, ty);
  // Bancada pela USB: "S" = captura do quadro; "T x,y" = toque simulado.
  static String s_cmd;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (s_cmd == "S") gfxDumpSerial();
      else if (s_cmd.startsWith("T ")) {
        int comma = s_cmd.indexOf(',');
        if (comma > 2) {
          tx = s_cmd.substring(2, comma).toInt();
          ty = s_cmd.substring(comma + 1).toInt();
          touched = true;
        }
      }
      s_cmd = "";
    } else if (s_cmd.length() < 24) {
      s_cmd += c;
    }
  }
  if (touched) {
    g_touchCount++;
    Serial.printf("[toque] %d,%d tela %d\n", tx, ty, (int)g_screen);
    g_app.lastTouchMs = now;
    g_app.lastActivityMs = now;
  }

  // Pressão longa (2 s) no cabeçalho: Configurações.
  int hx = 0, hy = 0;
  bool held = hwTouchHeld(hx, hy);
  if (held && hy < HDR_H && g_screen != SCR_FACE && g_screen != SCR_STATS) {
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

  // Demonstração pedida pelo Hub (hub/tools/demo.py).
  if (g_app.demoScreen >= 0) {
    static const Screen map[3] = {SCR_LIST, SCR_STATS, SCR_FACE};
    g_screen = map[g_app.demoScreen];
    g_app.demoScreen = -1;
    g_app.lastTouchMs = now;  // não cai no rosto sozinho logo em seguida
    if (g_screen == SCR_FACE) faceInvalidate();
  }

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
      int fh = touched ? uiFooterHit(tx, ty) : 0;
      int pages = max(1, (g_app.sessionCount + PER_PAGE - 1) / PER_PAGE);
      if (fh == 2) {
        g_screen = SCR_STATS;
        g_statsPressStart = now;   // o toque que abriu não conta ao soltar
        g_statsHoldDone = true;
      } else if (fh == 1) {
        // pílula do Hub: só informativa
      } else if (hit == -2) {
        g_app.listPage = (g_app.listPage + 1) % pages;
      } else if (hit == -3) {
        g_app.listPage = (g_app.listPage + pages - 1) % pages;
      } else if (hit == -4) {
        g_screen = SCR_SETTINGS;
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
      if (held) {
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
      uiDrawHeader(g_app, false, false);
      int act = uiDrawSettings(g_app, touched, tx, ty);
      if (act == 4 || act == 5) {
        int v = (int)g_app.volumePct + (act == 5 ? 10 : -10);
        g_app.volumePct = (uint8_t)constrain(v, 0, 100);
        voiceSetVolume(g_app.volumePct);
        settingsSaveVolume(g_app.volumePct);
        if (g_app.volumePct) voiceTone(1046, 90);  // amostra no volume novo
        Serial.printf("[som] volume %u%%\n", g_app.volumePct);
      } else if (act == 1) {
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
