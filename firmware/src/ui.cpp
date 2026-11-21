// AI Monitor v0.2 — telas 320x240 com o tema "espaço" e a Nous-Girl.
// Tudo é desenhado no quadro em memória (gfx.h) e enviado de uma vez.
#include "ui.h"

#include <M5Unified.h>
#include <WiFi.h>
#include <time.h>

#include "config.h"
#include "face.h"
#include "gfx.h"

#define D G()

// ---------------------------------------------------------------------------
// Utilitários
// ---------------------------------------------------------------------------
static bool inside(int tx, int ty, int x, int y, int w, int h) {
  return tx >= x && tx < x + w && ty >= y && ty < y + h;
}

static void font(const lgfx::IFont* f) {
  D.setFont(f);
  D.setTextSize(1);
  D.setTextDatum(textdatum_t::top_left);
}

// Mistura a -> b (alpha 0..255 = quanto de b).
static uint16_t blend(uint16_t a, uint16_t b, uint8_t alpha) {
  int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  int r = ar + ((br - ar) * alpha) / 255;
  int g = ag + ((bg - ag) * alpha) / 255;
  int bl = ab + ((bb - ab) * alpha) / 255;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

// Corta o texto para caber em maxW pixels com a fonte atual.
static String fit(const String& s, int maxW) {
  if (D.textWidth(s.c_str()) <= maxW) return s;
  String t = s;
  while (t.length() > 1 && D.textWidth((t + "..").c_str()) > maxW)
    t.remove(t.length() - 1);
  return t + "..";
}

// Quebra em até maxLines linhas de largura maxW (fonte atual).
static int wrapLines(const String& s, int maxW, String* out, int maxLines) {
  int n = 0;
  String line, word;
  String src = s + " ";
  for (size_t i = 0; i < src.length() && n < maxLines; i++) {
    char c = src[i];
    if (c != ' ') {
      word += c;
      continue;
    }
    String cand = line.length() ? line + " " + word : word;
    if (D.textWidth(cand.c_str()) <= maxW) {
      line = cand;
    } else {
      if (line.length()) out[n++] = line;
      line = word;
      if (n >= maxLines) break;
      while (D.textWidth(line.c_str()) > maxW && line.length() > 1) {
        // palavra maior que a linha (ex.: comando sem espaços)
        int cut = line.length();
        while (cut > 1 && D.textWidth(line.substring(0, cut).c_str()) > maxW) cut--;
        if (n < maxLines) out[n++] = line.substring(0, cut);
        line = line.substring(cut);
      }
    }
    word = "";
  }
  if (n < maxLines && line.length()) out[n++] = line;
  if (n == maxLines) {
    // reticências se cortou
    String rest = s;
    String joined;
    for (int i = 0; i < n; i++) joined += out[i];
    if (joined.length() + n < s.length()) out[n - 1] = fit(out[n - 1] + " ...", maxW);
  }
  return n;
}

static void pill(int x, int y, int w, int h, uint16_t bg, uint16_t fg,
                 const char* txt, const lgfx::IFont* f = &fonts::Font0) {
  D.fillRoundRect(x, y, w, h, h / 2, bg);
  font(f);
  D.setTextColor(fg);
  D.setTextDatum(textdatum_t::middle_center);
  D.drawString(txt, x + w / 2, y + h / 2 + 1);
  D.setTextDatum(textdatum_t::top_left);
}

static void button(int x, int y, int w, int h, uint16_t bg, uint16_t fg,
                   const char* label, bool outline = false) {
  if (outline) {
    D.fillRoundRect(x, y, w, h, 10, COL_BG);
    D.drawRoundRect(x, y, w, h, 10, bg);
    D.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 9, bg);
  } else {
    D.fillRoundRect(x, y, w, h, 10, bg);
    D.drawFastHLine(x + 8, y + 1, w - 16, blend(bg, 0xFFFF, 70));
  }
  font(&fonts::FreeSansBold9pt7b);
  D.setTextColor(outline ? bg : fg);
  D.setTextDatum(textdatum_t::middle_center);
  D.drawString(label, x + w / 2, y + h / 2 + 1);
  D.setTextDatum(textdatum_t::top_left);
}

static uint16_t sourceColor(const String& src) {
  if (src == "hermes") return COL_ACCENT;
  if (src == "claude") return RGB565(255, 146, 96);
  if (src == "codex") return RGB565(120, 210, 255);
  if (src == "gemini") return RGB565(110, 150, 255);
  if (src == "timon") return RGB565(244, 196, 92);   // dourado
  return COL_MUTED;
}

static char sourceLetter(const String& src) {
  if (src == "codex") return 'X';
  if (src.length() == 0) return '?';
  char c = src[0];
  return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

static void wifiBars(int x, int y, uint16_t onCol) {
  int rssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -127;
  int bars = rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : rssi > -90 ? 1 : 0;
  for (int i = 0; i < 4; i++) {
    int h = 3 + i * 2;
    D.fillRect(x + i * 4, y + 9 - h, 3, h, i < bars ? onCol : COL_BORDER);
  }
}

String uiClockStr() {
  time_t now = time(nullptr);
  if (now < 1000000000L) return "--:--";
  struct tm tmv;
  localtime_r(&now, &tmv);
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
  return String(buf);
}

bool uiReadTouch(int& x, int& y) {
  auto t = M5.Touch.getDetail();
  if (t.wasPressed()) {
    x = t.x;
    y = t.y;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Humor (compartilhado pelo rosto e pelo avatar)
// ---------------------------------------------------------------------------
FaceMood uiMoodFor(const AppState& st, uint32_t now, uint16_t& ink) {
  if (st.approval.active || countStatus(st, ST_NEEDYOU) > 0) {
    ink = INK_NEEDYOU;
    return MOOD_LISTENING;
  }
  if (!st.mockMode && !st.hubConnected) {
    ink = INK_SAD;
    return MOOD_SAD;
  }
  if (st.lastFinishedMs && now - st.lastFinishedMs < FACE_HAPPY_MS) {
    ink = INK_FINISHED;
    return MOOD_HAPPY;
  }
  if (countStatus(st, ST_WORKING) > 0) {
    ink = INK_WORKING;
    return MOOD_THINKING;
  }
  if (now - st.lastActivityMs > FACE_SLEEP_AFTER_MS) {
    ink = INK_SLEEPING;
    return MOOD_SLEEPING;
  }
  ink = INK_NEUTRAL;
  return MOOD_NEUTRAL;
}

// ---------------------------------------------------------------------------
// Fundo, cabeçalho e rodapé
// ---------------------------------------------------------------------------
static const int16_t BG_STARS[][2] = {
    {14, 70},  {296, 64}, {270, 112}, {40, 150}, {300, 160}, {160, 200},
    {120, 64}, {222, 150}, {84, 186},  {248, 196}, {190, 92}, {58, 104},
};

void uiDrawBackground() {
  for (int y = 0; y < 240; y += 4)
    D.fillRect(0, y, 320, 4, blend(COL_BG, COL_BG2, (uint8_t)(y * 255 / 240)));
  uint32_t t = millis() / 400;
  for (size_t i = 0; i < sizeof(BG_STARS) / sizeof(BG_STARS[0]); i++) {
    bool hi = ((t + i * 7) % 9) == 0;
    uint16_t c = hi ? 0xFFFF : blend(COL_BG2, COL_FINISHED, 110);
    D.drawPixel(BG_STARS[i][0], BG_STARS[i][1], c);
    if (hi) {
      D.drawPixel(BG_STARS[i][0] - 1, BG_STARS[i][1], blend(COL_BG, COL_FINISHED, 160));
      D.drawPixel(BG_STARS[i][0] + 1, BG_STARS[i][1], blend(COL_BG, COL_FINISHED, 160));
      D.drawPixel(BG_STARS[i][0], BG_STARS[i][1] - 1, blend(COL_BG, COL_FINISHED, 160));
      D.drawPixel(BG_STARS[i][0], BG_STARS[i][1] + 1, blend(COL_BG, COL_FINISHED, 160));
    }
  }
}

static uint16_t hubDotColor(const AppState& st) {
  if (st.mockMode) return COL_NEEDYOU;
  return st.hubConnected ? COL_WORKING : COL_DANGER;
}

static void drawLimitBar(int x, int y, const char* label, int pct) {
  font(&fonts::Font0);
  D.setTextColor(COL_MUTED);
  D.drawString(label, x, y + 1);
  int bx = x + D.textWidth(label) + 4, bw = 56;
  D.fillRoundRect(bx, y + 1, bw, 7, 3, COL_PANEL2);
  int fw = bw * constrain(pct, 0, 100) / 100;
  uint16_t c = pct >= 85 ? COL_DANGER : pct >= 60 ? COL_NEEDYOU : COL_WORKING;
  if (fw > 2) D.fillRoundRect(bx, y + 1, fw, 7, 3, c);
  D.setTextColor(COL_TEXT);
  D.drawString((String(pct) + "%").c_str(), bx + bw + 4, y + 1);
}

void uiDrawHeader(const AppState& st, bool blink) {
  (void)blink;
  uint32_t now = millis();
  uint16_t ink;
  uiMoodFor(st, now, ink);
  faceDrawAvatar(D, 5, 3, 28, ink);

  font(&fonts::FreeSansBold9pt7b);
  D.setTextColor(COL_TEXT);
  D.drawString(DELLMONITOR_NAME, 40, 2);

  font(&fonts::Font0);
  String sub;
  uint16_t subCol = COL_MUTED;
  if (st.mockMode) {
    sub = "modo demonstracao";
  } else if (!st.hubConnected) {
    sub = st.hubEverConnected ? "Hub desconectado" : "procurando o Hub...";
    subCol = COL_NEEDYOU;
  } else {
    int nw = countStatus(st, ST_WORKING), nn = countStatus(st, ST_NEEDYOU);
    sub = String(nw) + " trabalhando";
    if (nn) {
      sub += " - " + String(nn) + " precisa de voce";
      subCol = COL_NEEDYOU;
    }
  }
  D.setTextColor(subCol);
  D.drawString(fit(sub, 150).c_str(), 41, 22);

  font(&fonts::FreeSansBold12pt7b);
  String clk = uiClockStr();
  int cw = D.textWidth(clk.c_str());
  D.setTextColor(COL_TEXT);
  D.drawString(clk.c_str(), 314 - cw, 6);
  D.fillCircle(314 - cw - 9, 17, 3, hubDotColor(st));

  D.drawFastHLine(0, HDR_H, 320, COL_BORDER);

  // Limites de uso (5h / semana / plano).
  if (st.limits.valid) {
    drawLimitBar(8, LIM_Y + 3, "5H", st.limits.fiveHourUsedPct);
    drawLimitBar(118, LIM_Y + 3, "SEM", st.limits.weeklyUsedPct);
    if (st.limits.planRemaining.length()) {
      font(&fonts::Font0);
      D.setTextColor(COL_MUTED);
      String pr = fit(st.limits.planRemaining, 80);
      D.drawString(pr.c_str(), 312 - D.textWidth(pr.c_str()), LIM_Y + 4);
    }
  } else {
    font(&fonts::Font0);
    D.setTextColor(COL_IDLE);
    D.drawString("limites de uso: aguardando dados", 8, LIM_Y + 4);
  }
}

void uiDrawFooter(const AppState& st) {
  D.fillRect(0, FOOTER_Y, 320, 240 - FOOTER_Y, COL_PANEL);
  D.drawFastHLine(0, FOOTER_Y, 320, COL_BORDER);
  wifiBars(8, FOOTER_Y + 5, COL_FINISHED);
  font(&fonts::Font0);
  D.setTextColor(COL_MUTED);
  const char* hub = st.mockMode ? "demo" : (st.hubConnected ? "Hub ok" : "sem Hub");
  D.fillCircle(32, FOOTER_Y + 10, 3, hubDotColor(st));
  D.drawString(hub, 40, FOOTER_Y + 6);
  String right = st.host.valid
                     ? "CPU " + String(st.host.cpuPct) + "%  RAM " + String(st.host.memPct) + "%  >"
                     : String("computador  >");
  D.drawString(right.c_str(), 312 - D.textWidth(right.c_str()), FOOTER_Y + 6);
}

// ---------------------------------------------------------------------------
// Lista
// ---------------------------------------------------------------------------
static void drawEmptyState(const AppState& st) {
  uint16_t ink;
  uiMoodFor(st, millis(), ink);
  faceDrawAvatar(D, 128, 66, 64, ink);
  font(&fonts::FreeSansBold9pt7b);
  D.setTextColor(COL_TEXT);
  D.setTextDatum(textdatum_t::top_center);
  D.drawString(st.mockMode || st.hubConnected ? "Tudo tranquilo" : "Sem conexao com o Hub", 160, 140);
  font(&fonts::Font0);
  D.setTextColor(COL_MUTED);
  D.setTextDatum(textdatum_t::top_center);
  D.drawString(st.mockMode || st.hubConnected ? "nenhuma sessao de IA rodando agora"
                                              : "confira se o Hub esta rodando no PC",
               160, 162);
  D.setTextDatum(textdatum_t::top_left);
}

static void drawCard(const Session& s, int y, bool blinkOn) {
  uint16_t col = statusColor(s.status);
  bool alert = s.status == ST_NEEDYOU;
  D.fillRoundRect(6, y, 308, CARD_H, 10, COL_PANEL);
  if (alert) {
    uint16_t bc = blinkOn ? COL_NEEDYOU : blend(COL_PANEL, COL_NEEDYOU, 90);
    D.drawRoundRect(6, y, 308, CARD_H, 10, bc);
    D.drawRoundRect(7, y + 1, 306, CARD_H - 2, 9, bc);
  }
  D.fillRoundRect(9, y + 8, 4, CARD_H - 16, 2, col);

  uint16_t sc = sourceColor(s.source);
  D.fillCircle(30, y + CARD_H / 2, 12, blend(COL_PANEL, sc, 70));
  D.drawCircle(30, y + CARD_H / 2, 12, sc);
  font(&fonts::FreeSansBold9pt7b);
  D.setTextColor(sc);
  D.setTextDatum(textdatum_t::middle_center);
  char letter[2] = {sourceLetter(s.source), 0};
  D.drawString(letter, 30, y + CARD_H / 2 + 1);
  D.setTextDatum(textdatum_t::top_left);

  // Selo de status.
  font(&fonts::Font0);
  const char* lb = statusLabel(s.status);
  int cw = D.textWidth(lb) + 14;
  uint16_t chipFg = (alert && !blinkOn) ? blend(COL_PANEL, col, 150) : col;
  pill(306 - cw, y + 7, cw, 14, blend(COL_PANEL, col, 50), chipFg, lb);

  font(&fonts::FreeSansBold9pt7b);
  D.setTextColor(COL_TEXT);
  D.drawString(fit(s.title, 300 - cw - 52).c_str(), 48, y + 5);

  font(&fonts::Font0);
  D.setTextColor(COL_MUTED);
  String d = s.source;
  if (s.profile.length()) d += "/" + s.profile;
  if (s.detail.length()) d += "  " + s.detail;
  D.drawString(fit(d, 256).c_str(), 48, y + 30);
}

int uiDrawList(AppState& st, bool touched, int tx, int ty) {
  int pages = (st.sessionCount + PER_PAGE - 1) / PER_PAGE;
  if (pages < 1) pages = 1;
  if (st.listPage >= pages) st.listPage = 0;
  if (st.sessionCount == 0) {
    drawEmptyState(st);
    return -1;
  }
  bool blinkOn = (millis() / 500) % 2 == 0;
  int hit = -1;
  for (int k = 0; k < PER_PAGE; k++) {
    int i = st.listPage * PER_PAGE + k;
    if (i >= st.sessionCount) break;
    int y = LIST_TOP + k * CARD_STEP;
    drawCard(st.sessions[i], y, blinkOn);
    if (touched && inside(tx, ty, 6, y, 308, CARD_H)) hit = i;
  }
  if (pages > 1) {
    String p = "<   " + String(st.listPage + 1) + " / " + String(pages) + "   >";
    font(&fonts::Font0);
    int w = D.textWidth(p.c_str()) + 20;
    pill(160 - w / 2, PAGER_Y + 1, w, 13, COL_PANEL2, COL_TEXT, p.c_str());
    if (touched && ty >= PAGER_Y - 2 && ty < FOOTER_Y) return -2;
  }
  return hit;
}

// ---------------------------------------------------------------------------
// Detalhe
// ---------------------------------------------------------------------------
int uiDrawDetail(AppState& st, bool touched, int tx, int ty) {
  if (st.selectedSession < 0 || st.selectedSession >= st.sessionCount) return 1;
  Session& s = st.sessions[st.selectedSession];
  uint16_t col = statusColor(s.status);
  D.fillRoundRect(6, LIST_TOP - 2, 308, 128, 12, COL_PANEL);
  D.fillRoundRect(6, LIST_TOP - 2, 6, 128, 3, col);

  font(&fonts::FreeSansBold12pt7b);
  D.setTextColor(COL_TEXT);
  D.drawString(fit(s.title, 288).c_str(), 18, LIST_TOP + 4);

  font(&fonts::Font0);
  const char* lb = statusLabel(s.status);
  int cw = D.textWidth(lb) + 14;
  pill(18, LIST_TOP + 32, cw, 14, blend(COL_PANEL, col, 60), col, lb);
  D.setTextColor(sourceColor(s.source));
  String src = s.source + (s.profile.length() ? " / " + s.profile : String(""));
  D.drawString(fit(src, 270 - cw).c_str(), 26 + cw, LIST_TOP + 35);

  D.setTextColor(COL_ACCENT);
  D.drawString("FAZENDO AGORA", 18, LIST_TOP + 54);
  font(&fonts::Font2);
  D.setTextColor(COL_TEXT);
  String lines[3];
  int n = wrapLines(s.detail.length() ? s.detail : String("(sem detalhes)"), 284, lines, 3);
  for (int i = 0; i < n; i++) D.drawString(lines[i].c_str(), 18, LIST_TOP + 66 + i * 16);

  if (s.hasUsage) {
    font(&fonts::Font0);
    D.setTextColor(COL_MUTED);
    String u = "tokens " + String(s.inputTokens) + " in / " + String(s.outputTokens) +
               " out   custo $" + String(s.costUsd, 2);
    D.drawString(fit(u, 284).c_str(), 18, LIST_TOP + 114);
  }

  const int by = 184, bh = 32;
  bool hasAp = st.approval.active && st.approval.sessionId == s.id;
  bool canAct = hasAp && st.hubConnected && st.hubActions;
  button(8, by, canAct ? 92 : 304, bh, COL_MUTED, COL_TEXT, "Voltar", true);
  if (canAct) {
    button(108, by, 96, bh, COL_DANGER, COL_BG, "Negar");
    button(212, by, 100, bh, COL_WORKING, COL_BG, "Aprovar 1x");
  } else if (hasAp) {
    font(&fonts::Font0);
    D.setTextColor(COL_NEEDYOU);
    D.drawString("aprovacao pendente: responda no computador", 18, by - 12);
  }
  if (!touched) return 0;
  if (inside(tx, ty, 8, by, canAct ? 92 : 304, bh)) return 1;
  if (canAct && inside(tx, ty, 108, by, 96, bh)) return 4;
  if (canAct && inside(tx, ty, 212, by, 100, bh)) return 2;
  return 0;
}

// ---------------------------------------------------------------------------
// Aprovação
// ---------------------------------------------------------------------------
int uiDrawApproval(AppState& st, bool touched, int tx, int ty) {
  bool blinkOn = (millis() / 600) % 2 == 0;
  uint16_t amber = blinkOn ? COL_NEEDYOU : blend(COL_NEEDYOU, RGB565(200, 120, 20), 140);
  uiDrawBackground();
  D.fillRect(0, 0, 320, 64, amber);
  D.fillRect(0, 64, 320, 3, blend(amber, COL_BG, 120));
  faceDrawAvatar(D, 10, 8, 48, RGB565(255, 236, 200), RGB565(40, 20, 10));

  font(&fonts::FreeSansBold12pt7b);
  D.setTextColor(RGB565(30, 16, 8));
  D.drawString("Precisa de voce", 68, 10);
  font(&fonts::Font0);
  Session* s = findSession(st, st.approval.sessionId);
  String who = s ? (s->source + ": " + s->title) : String("sessao de IA");
  D.drawString(fit(who, 240).c_str(), 69, 40);

  D.fillRoundRect(8, 76, 304, 100, 12, COL_PANEL);
  font(&fonts::Font0);
  D.setTextColor(COL_NEEDYOU);
  D.drawString("COMANDO", 18, 84);
  font(&fonts::FreeMono9pt7b);
  D.setTextColor(COL_TEXT);
  String lines[3];
  int n = wrapLines(st.approval.command, 286, lines, 3);
  for (int i = 0; i < n; i++) D.drawString(lines[i].c_str(), 18, 96 + i * 18);
  if (st.approval.description.length()) {
    font(&fonts::Font0);
    D.setTextColor(COL_MUTED);
    D.drawString(fit(st.approval.description, 286).c_str(), 18, 160);
  }

  const int by = 188, bh = 40;
  bool canAct = st.hubConnected && st.hubActions;
  if (canAct) {
    button(8, by, 96, bh, COL_DANGER, COL_BG, "Negar");
    button(112, by, 88, bh, COL_MUTED, COL_TEXT, "Depois", true);
    button(208, by, 104, bh, COL_WORKING, COL_BG, "Aprovar 1x");
    if (!touched) return 0;
    if (inside(tx, ty, 8, by, 96, bh)) return 4;
    if (inside(tx, ty, 112, by, 88, bh)) return 5;
    if (inside(tx, ty, 208, by, 104, bh)) return 2;
    return 0;
  }
  font(&fonts::Font0);
  D.setTextColor(COL_MUTED);
  D.setTextDatum(textdatum_t::top_center);
  D.drawString("Modo so leitura: responda no computador ou no Telegram.", 160, by - 8);
  D.setTextDatum(textdatum_t::top_left);
  button(8, by + 6, 304, bh - 6, COL_NEEDYOU, COL_BG, "Entendi");
  if (touched && inside(tx, ty, 8, by, 304, bh)) return 5;
  return 0;
}

// ---------------------------------------------------------------------------
// Rosto em tela cheia: sobreposições
// ---------------------------------------------------------------------------
void uiDrawFaceOverlay(const AppState& st, uint32_t now) {
  uint16_t ink;
  FaceMood m = uiMoodFor(st, now, ink);
  // Relógio no canto superior esquerdo (área vazia do desenho), escuro
  // sobre o papel colorido.
  D.fillRect(4, 4, 74, 26, ink);
  font(&fonts::FreeSansBold12pt7b);
  D.setTextColor(FACE_LINE);
  D.drawString(uiClockStr().c_str(), 8, 8);
  // Conexão + limite (canto superior direito).
  D.fillRect(250, 4, 68, 18, ink);
  D.fillCircle(308, 13, 4, hubDotColor(st));
  D.drawCircle(308, 13, 4, FACE_LINE);
  if (st.limits.valid) {
    font(&fonts::Font0);
    D.setTextColor(FACE_LINE);
    String l = "5h " + String(st.limits.fiveHourUsedPct) + "%";
    D.drawString(l.c_str(), 300 - D.textWidth(l.c_str()), 9);
  }
  // Faixa de estado.
  String msg;
  switch (m) {
    case MOOD_LISTENING: {
      const Session* ns = nullptr;
      for (int i = 0; i < st.sessionCount; i++)
        if (st.sessions[i].status == ST_NEEDYOU) ns = &st.sessions[i];
      msg = "Precisa de voce" + (ns ? ": " + ns->title : String(""));
      break;
    }
    case MOOD_SAD: msg = "Sem conexao com o Hub"; break;
    case MOOD_HAPPY: msg = "Terminou: " + st.lastFinishedTitle; break;
    case MOOD_THINKING: {
      int n = countStatus(st, ST_WORKING);
      msg = "Trabalhando em " + String(n) + (n == 1 ? " sessao" : " sessoes");
      break;
    }
    case MOOD_SLEEPING: msg = "Zzz... toque para acordar"; break;
    default: msg = "Tudo tranquilo - toque para ver"; break;
  }
  D.fillRoundRect(10, 206, 300, 28, 14, COL_PANEL);
  D.drawRoundRect(10, 206, 300, 28, 14, FACE_LINE);
  D.fillCircle(26, 220, 4, ink);
  font(&fonts::FreeSans9pt7b);
  D.setTextColor(COL_TEXT);
  D.drawString(fit(msg, 260).c_str(), 38, 212);
}

// ---------------------------------------------------------------------------
// Configurações
// ---------------------------------------------------------------------------
static uint32_t s_forgetArmedAt = 0;

static void infoRow(int y, const char* label, const String& value, uint16_t vc = COL_TEXT) {
  font(&fonts::Font0);
  D.setTextColor(COL_MUTED);
  D.drawString(label, 18, y);
  D.setTextColor(vc);
  D.drawString(fit(value, 200).c_str(), 96, y);
}

int uiDrawSettings(AppState& st, bool touched, int tx, int ty) {
  font(&fonts::FreeSansBold9pt7b);
  D.setTextColor(COL_TEXT);
  D.drawString("Configuracoes", 10, 40);
  D.fillRoundRect(6, 62, 308, 98, 12, COL_PANEL);
  int y = 72;
  if (WiFi.status() == WL_CONNECTED && st.wifiSsid.length()) {
    int pct = constrain(2 * (WiFi.RSSI() + 100), 0, 100);
    infoRow(y, "Wi-Fi", st.wifiSsid + "  (" + String(pct) + "%)");
  } else {
    infoRow(y, "Wi-Fi", "sem conexao", COL_DANGER);
  }
  y += 18;
  infoRow(y, "Hub", st.hubHost.length() ? st.hubHost + ":" + String(st.hubPort)
                                        : String("nao configurado (demo)"));
  y += 18;
  infoRow(y, "Estado",
          st.mockMode ? String("demonstracao") : (st.hubConnected ? String("conectado") : String("sem conexao")),
          st.mockMode ? COL_NEEDYOU : (st.hubConnected ? COL_WORKING : COL_DANGER));
  y += 18;
  infoRow(y, "Aprovar", st.hubActions ? String("pelo toque (ligado no Hub)") : String("so leitura"),
          st.hubActions ? COL_NEEDYOU : COL_WORKING);
  y += 18;
  infoRow(y, "Aparelho", (st.ipAddress.length() ? st.ipAddress : String("-")) + "   v" DELLMONITOR_VERSION);

  uint32_t now = millis();
  bool armed = s_forgetArmedAt && now - s_forgetArmedAt < 4000;
  if (!armed) s_forgetArmedAt = 0;
  button(8, 168, 148, 34, COL_PANEL2, COL_TEXT, "Trocar rede");
  button(164, 168, 148, 34, COL_DANGER, COL_BG, armed ? "Confirmar?" : "Esquecer Wi-Fi", !armed);
  font(&fonts::Font0);
  D.setTextColor(COL_MUTED);
  D.drawString("< voltar", 10, 208);
  if (!touched) return 0;
  if (inside(tx, ty, 8, 168, 148, 34)) return 2;
  if (inside(tx, ty, 164, 168, 148, 34)) {
    if (armed) {
      s_forgetArmedAt = 0;
      return 3;
    }
    s_forgetArmedAt = now;
    return 0;
  }
  if (ty >= 204) return 1;
  return 0;
}

// ---------------------------------------------------------------------------
// Boot e aviso
// ---------------------------------------------------------------------------
void uiBootScreen(const char* line) {
  uiDrawBackground();
  faceDrawAvatar(D, 112, 22, 96, INK_NEUTRAL);
  font(&fonts::FreeSansBold12pt7b);
  D.setTextColor(COL_TEXT);
  D.setTextDatum(textdatum_t::top_center);
  D.drawString(DELLMONITOR_NAME, 160, 132);
  font(&fonts::Font0);
  D.setTextColor(COL_MUTED);
  D.setTextDatum(textdatum_t::top_center);
  D.drawString("monitor de sessoes de IA  v" DELLMONITOR_VERSION, 160, 160);
  D.setTextColor(COL_ACCENT);
  D.setTextDatum(textdatum_t::top_center);
  D.drawString(line, 160, 182);
  D.setTextDatum(textdatum_t::top_left);
  gfxPresent();
}

void uiDrawToast(const String& msg) {
  font(&fonts::Font2);
  String t = fit(msg, 260);
  int w = D.textWidth(t.c_str()) + 28;
  int x = 160 - w / 2;
  D.fillRoundRect(x, 170, w, 28, 14, COL_PANEL2);
  D.drawRoundRect(x, 170, w, 28, 14, COL_ACCENT);
  D.setTextColor(COL_TEXT);
  D.setTextDatum(textdatum_t::middle_center);
  D.drawString(t.c_str(), 160, 185);
  D.setTextDatum(textdatum_t::top_left);
}

// ---------------------------------------------------------------------------
// Tela COMPUTADOR (estatísticas do PC) — cena de mesa da referência
// (parede roxo-azulada com estrelas, quadro, janela, luminária, laptop,
// aparelhinho amarelo, planta) na parte de cima (y 0–146) e cartão marrom
// com os números na parte de baixo (y 150–232). Tela cheia: o main.cpp não
// desenha cabeçalho nem rodapé aqui — título e selo vivem dentro da cena,
// como na referência. A ilustração é uma versão pixel estilizada da
// referência, desenhada só com primitivas do M5GFX; não é a imagem exata.
// ---------------------------------------------------------------------------

// Cores da cena (RGB565).
#define SC_WALL        0x296D  // parede roxo-azulada
#define SC_WALL_LIGHT  0x4251  // cone de luz da luminária
#define SC_SKY         0x8E9E  // céu da janela
#define SC_SKY_LIGHT   0xBF1E  // céu claro do quadro
#define SC_SUN         0xFEC7  // sol amarelo
#define SC_CREAM       0xF75A  // moldura da janela / quadro / peitoril
#define SC_MOUNT1      0x7AF4  // montanhas roxas
#define SC_MOUNT2      0x9BF8
#define SC_LAMP        0xF3E4  // luminária laranja
#define SC_WOOD        0xABA7  // mesa de madeira
#define SC_WOOD_DARK   0x7A86
#define SC_WOOD_LIGHT  0xC449
#define SC_SCREEN      0x1947  // tela do laptop / do aparelhinho
#define SC_SILVER      0xCE7A  // base do laptop
#define SC_GADGET      0xF623  // aparelhinho amarelo
#define SC_LEAF        0x3D09  // planta
#define SC_LEAF2       0x558B
#define SC_POT         0xEB45  // vaso laranja
#define SC_CARD        0x4963  // cartão marrom das estatísticas
#define SC_TRACK       0x3101  // trilho das barras
#define SC_NET         0x2E6E  // verde da linha de rede
#define SC_BADGE_BG    0x18E9  // pílula do selo
#define SC_CODE_PURP   0x9AD6
#define SC_CODE_BLUE   0x34DB
#define SC_CODE_RED    0xE267
#define SC_CODE_ORANGE 0xF4E2

// 0 = sem dados, 1 = TRANQUILO, 2 = PESADO, 3 = BAIXA (bateria < 20%).
static int hostStateCode(const AppState& st) {
  if (!st.host.valid) return 0;
  if (st.host.hasBattery && st.host.battPct < 20) return 3;
  if (st.host.cpuPct >= 85) return 2;
  return 1;
}

// MB/s com vírgula decimal, como na referência ("1,2 MB/s").
static String fmtMbps(float v) {
  String s = String(v, 1);
  s.replace(".", ",");
  return s;
}

// Setinha (triângulo 6x5 px) para a linha de rede.
static void drawArrow(int x, int y, bool down, uint16_t col) {
  if (down) {
    G().fillTriangle(x, y, x + 6, y, x + 3, y + 5, col);
  } else {
    G().fillTriangle(x, y + 5, x + 6, y + 5, x + 3, y, col);
  }
}

// Estrelas fixas (tabela determinística — a cena não muda a cada redraw).
static const int STARS[][3] = {
    // x, y, tamanho (1 ou 2 px)
    {26, 26, 2}, {52, 14, 1}, {64, 44, 2}, {140, 16, 2}, {158, 36, 1},
    {172, 60, 2}, {30, 66, 1}, {48, 92, 2}, {66, 78, 1}, {128, 84, 2},
    {146, 60, 1}, {160, 96, 2}, {18, 40, 2}, {110, 16, 1}, {176, 88, 1},
    {310, 18, 2}, {100, 66, 1}, {186, 14, 1},
};

// Cena da mesa (y 0–146): parede, estrelas, quadro, janela, luminária,
// laptop, aparelhinho amarelo, planta e mesa de madeira.
static void drawDeskScene() {
  // Parede.
  G().fillRect(0, 0, 320, 118, SC_WALL);
  // Estrelas (algumas amareladas).
  for (size_t i = 0; i < sizeof(STARS) / sizeof(STARS[0]); i++) {
    uint16_t c = (i % 4 == 3) ? 0xFFDD : 0xFFFF;
    G().fillRect(STARS[i][0], STARS[i][1], STARS[i][2], STARS[i][2],
                        c);
  }

  // Quadro pequeno (sol laranja sobre montanhas roxas): x 80–118, y 28–56.
  G().fillRoundRect(80, 28, 38, 28, 3, SC_CREAM);
  G().fillRect(83, 31, 32, 22, SC_SKY_LIGHT);
  G().fillCircle(96, 38, 4, SC_CODE_ORANGE);
  G().fillTriangle(83, 53, 93, 42, 103, 53, SC_MOUNT1);
  G().fillTriangle(96, 53, 107, 43, 115, 53, SC_MOUNT2);

  // Janela (moldura creme, 4 vidros, sol e nuvens): x 192–302, y 24–108.
  G().fillRect(192, 24, 110, 84, SC_CREAM);
  G().fillRect(196, 28, 49, 34, SC_SKY);
  G().fillRect(249, 28, 49, 34, SC_SKY);
  G().fillRect(196, 66, 49, 38, SC_SKY);
  G().fillRect(249, 66, 49, 38, SC_SKY);
  G().fillCircle(282, 42, 9, SC_SUN);
  G().fillRoundRect(204, 44, 20, 6, 3, 0xFFFF);
  G().fillCircle(210, 43, 4, 0xFFFF);
  G().fillRoundRect(206, 80, 18, 6, 3, 0xFFFF);
  G().fillCircle(212, 79, 4, 0xFFFF);
  G().fillRect(188, 108, 118, 6, SC_CREAM);  // peitoril

  // Luminária: cone de luz (triângulo num tom mais claro da parede),
  // base, braço em 2 segmentos e abajur com a lâmpada.
  G().fillTriangle(56, 80, 72, 118, 102, 118, SC_WALL_LIGHT);
  G().fillRect(12, 112, 20, 6, SC_LAMP);
  G().drawWideLine(22, 112, 38, 74, 2.5f, SC_LAMP);
  G().drawWideLine(38, 74, 52, 68, 2.5f, SC_LAMP);
  G().fillTriangle(48, 64, 66, 72, 54, 82, SC_LAMP);
  G().fillCircle(58, 76, 3, 0xFFFF);

  // Mesa de madeira (y 118–146).
  G().fillRect(0, 118, 320, 28, SC_WOOD);
  G().fillRect(0, 118, 320, 2, SC_WOOD_LIGHT);
  G().fillRect(0, 144, 320, 2, SC_WOOD_DARK);

  // Laptop: moldura, tela escura com linhas de código, base prateada.
  G().fillRect(98, 82, 80, 34, 0x0000);
  G().fillRect(100, 84, 76, 30, SC_SCREEN);
  G().fillRect(104, 88, 20, 3, SC_NET);
  G().fillRect(126, 88, 14, 3, SC_CODE_PURP);
  G().fillRect(104, 94, 30, 3, SC_CODE_BLUE);
  G().fillRect(136, 94, 10, 3, SC_CODE_RED);
  G().fillRect(104, 100, 16, 3, SC_NET);
  G().fillRect(122, 100, 22, 3, SC_CODE_ORANGE);
  G().fillRect(104, 106, 34, 3, SC_CODE_PURP);
  G().fillRoundRect(94, 114, 88, 6, 3, SC_SILVER);

  // Aparelhinho amarelo ao lado do laptop.
  G().fillRoundRect(190, 98, 18, 20, 3, SC_GADGET);
  G().fillRect(193, 101, 12, 10, SC_SCREEN);
  G().fillRect(194, 113, 10, 2, SC_CODE_RED);

  // Planta: folhas em leque + vaso laranja.
  G().fillTriangle(282, 102, 262, 72, 288, 102, SC_LEAF);
  G().fillTriangle(284, 102, 274, 62, 290, 102, SC_LEAF2);
  G().fillTriangle(286, 102, 284, 58, 292, 102, SC_LEAF);
  G().fillTriangle(288, 102, 296, 64, 294, 102, SC_LEAF2);
  G().fillTriangle(290, 102, 304, 74, 296, 102, SC_LEAF);
  G().fillRect(264, 100, 40, 5, SC_POT);
  G().fillRect(268, 105, 32, 15, SC_POT);
}

// Número grande (tamanho 3) + rótulo pequeno ao lado ("31 % CPU").
static void drawBigStat(int x, int y, int value, const char* unit,
                        uint16_t bg) {
  String num = String(value);
  G().setTextColor(COL_TEXT, bg);
  G().setTextSize(3);
  G().setCursor(x, y);
  G().print(num);
  G().setTextSize(1);
  G().setCursor(x + (int)num.length() * 18 + 4, y + 14);
  G().print(unit);
}

// Barra do cartão: trilho marrom escuro + preenchimento colorido.
static void drawCardBar(int x, int y, int w, int h, int pct, uint16_t col) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  G().fillRect(x, y, w, h, SC_TRACK);
  int fw = w * pct / 100;
  if (fw > 0) G().fillRect(x, y, fw, h, col);
}

int uiDrawStats(AppState& st, bool touched, int tx, int ty) {
  (void)tx;
  (void)ty;
  G().setFont(&fonts::Font0);
  G().setTextDatum(textdatum_t::top_left);
  int code = hostStateCode(st);
  const char* badge = (code == 1)   ? "TRANQUILO"
                      : (code == 2) ? "PESADO"
                      : (code == 3) ? "BAIXA"
                                    : "SEM DADOS";
  uint16_t badgeFg = (code == 1)   ? COL_WORKING
                     : (code == 2) ? COL_NEEDYOU
                     : (code == 3) ? 0xF800  // vermelho
                                   : COL_IDLE;

  // Cena da mesa (y 0–146) + título e selo dentro dela.
  drawDeskScene();
  G().setTextColor(COL_TEXT, SC_WALL);
  G().setTextSize(2);
  G().setCursor(10, 10);
  G().print("COMPUTADOR");
  int bw = (int)strlen(badge) * 6 + 14;
  int bx = 320 - 10 - bw;
  G().fillRoundRect(bx, 8, bw, 18, 9, SC_BADGE_BG);
  G().setTextColor(badgeFg, SC_BADGE_BG);
  G().setTextSize(1);
  G().setCursor(bx + (bw - (int)strlen(badge) * 6) / 2, 8 + 5);
  G().print(badge);

  // Cartão marrom (y 150–232).
  G().fillRoundRect(10, 150, 300, 82, 10, SC_CARD);

  if (!st.host.valid) {
    G().setTextColor(COL_MUTED, SC_CARD);
    G().setTextSize(1);
    G().setCursor(22, 186);
    G().print("Sem dados do Hub.");
    return touched ? 1 : 0;
  }

  // CPU e RAM grandes + barras.
  drawBigStat(22, 158, st.host.cpuPct, "% CPU", SC_CARD);
  drawBigStat(172, 158, st.host.memPct, "% RAM", SC_CARD);
  uint16_t cpuCol = (st.host.cpuPct >= 85)   ? 0xF800
                    : (st.host.cpuPct >= 60) ? COL_NEEDYOU
                                             : COL_WORKING;
  drawCardBar(22, 190, 128, 6, st.host.cpuPct, cpuCol);
  drawCardBar(172, 190, 126, 6, st.host.memPct, COL_FINISHED);

  // Rede (download/upload) em verde, com setinhas; GPU à direita.
  G().setTextColor(SC_NET, SC_CARD);
  G().setTextSize(1);
  int ny = 202;
  drawArrow(22, ny + 1, true, SC_NET);
  G().setCursor(31, ny);
  if (st.host.hasNet) {
    G().print(fmtMbps(st.host.netDownMbps));
    G().print(" MB/s");
  } else {
    G().print("-- MB/s");
  }
  int upX = G().getCursorX() + 6;
  drawArrow(upX, ny + 1, false, SC_NET);
  G().setCursor(upX + 9, ny);
  if (st.host.hasNet) {
    G().print(fmtMbps(st.host.netUpMbps));
    G().print(" MB/s");
  } else {
    G().print("-- MB/s");
  }
  String gpu = st.host.hasGpu ? ("GPU " + String(st.host.gpuPct) + "%")
                              : "GPU --";
  G().setTextColor(COL_TEXT, SC_CARD);
  G().setCursor(298 - (int)gpu.length() * 6, ny);
  G().print(gpu);

  // 4ª linha: bateria (quando houver) ou processo top; IP à direita.
  G().setTextColor(COL_MUTED, SC_CARD);
  G().setCursor(22, 216);
  if (st.host.hasBattery) {
    G().print("Bateria ");
    G().print(st.host.battPct);
    G().print("%");
    if (st.host.battMins >= 0) {
      G().print("  faltam ");
      G().print(st.host.battMins);
      G().print(" min");
    }
  } else if (st.host.topPct >= 0 && st.host.topName.length()) {
    String top = st.host.topName;
    if (top.length() > 20) top = top.substring(0, 19) + "~";
    G().print("Top: ");
    G().print(top);
    G().print(" ");
    G().print(st.host.topPct);
    G().print("%");
  } else {
    G().print("Top: --");
  }
  if (st.ipAddress.length()) {
    int iw = (int)st.ipAddress.length() * 6;
    G().setCursor(298 - iw, 216);
    G().print(st.ipAddress);
  }

  // Qualquer toque volta à lista (não há botão Voltar nesta tela).
  return touched ? 1 : 0;
}


void uiDrawPinBadge() {
  // Entre o título "COMPUTADOR" (termina em x~130) e o selo de estado.
  G().fillRoundRect(146, 8, 84, 18, 9, COL_ACCENT);
  G().fillCircle(158, 15, 3, COL_TEXT);        // cabeça do alfinete
  G().drawLine(158, 18, 158, 22, COL_TEXT);    // haste
  G().setFont(&fonts::Font0);
  G().setTextSize(1);
  G().setTextColor(COL_TEXT, COL_ACCENT);
  G().setCursor(168, 13);
  G().print("FIXADA");
}
