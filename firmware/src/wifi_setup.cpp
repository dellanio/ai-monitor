// AI Monitor v0.1 — Wi-Fi: portal de configuração, conexão e mDNS.
//
// Primeira inicialização (sem credenciais salvas): sobe um Access Point
// "AIMonitor-Setup" com portal captivo. A tela do aparelho mostra os
// passos numerados, o nome do AP, a URL do portal e um QR code — e a
// linha de status ao vivo (aguardando / conectando / conectado / erro).
// O portal lista as redes encontradas por scan, com barras de sinal.
#include "wifi_setup.h"
#include "hw.h"
#include "config.h"

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <M5Unified.h>

static Preferences prefs;
static WebServer portalServer(80);
static DNSServer dnsServer;
static bool portalRunning = false;

// Máquina de estados do portal (pós-envio do formulário).
enum PortalState {
  PORTAL_IDLE = 0,     // aguardando o usuário preencher o formulário
  PORTAL_CONNECTING,   // tentando conectar com as credenciais enviadas
  PORTAL_OK,           // conectado — mostra o IP e reinicia em seguida
  PORTAL_FAIL          // falhou — mostra o erro e o link para tentar de novo
};
static PortalState portalState = PORTAL_IDLE;
static String portalFailReason;
static String portalOkIp;
static uint32_t portalRestartAt = 0;

// ---------------------------------------------------------------------------
// Tela de setup no aparelho (320x240)
// Layout:
//   y   6  título "AI Monitor" (tamanho 3)
//   y  34  subtítulo "Configurar Wi-Fi"
//   y  44  passo 1: conecte no Wi-Fi "AIMonitor-Setup" (tamanho 2, âmbar)
//   y  86  passo 2: escaneie o QR ou abra 192.168.4.1 (tamanho 2)
//   y 128  passo 3: escolha sua rede e salve
//   x 216  QR code da URL do portal (96x96)
//   y 168  faixa de status ao vivo (redesenhada por wifiSetupSetStatus)
//   y 216  dica: "Depois: dellmonitor.local"
// ---------------------------------------------------------------------------

// Círculo numerado do passo (fundo âmbar, número preto).
static void drawStepBadge(int cx, int cy, const char* n) {
  DISP.fillCircle(cx, cy, 9, COL_ACCENT);
  DISP.setTextColor(0x0000, COL_ACCENT);
  DISP.setTextSize(1);
  DISP.setCursor(cx - 3, cy - 4);
  DISP.print(n);
}

void wifiSetupSetStatus(const String& msg, uint16_t color) {
  // Redesenha só a faixa de status (evita piscar a tela inteira).
  DISP.fillRoundRect(10, 168, 300, 34, 6, COL_PANEL);
  DISP.setTextColor(color, COL_PANEL);
  String m = msg;
  if (m.length() > 26) {
    DISP.setTextSize(1);
    DISP.setCursor(10 + (300 - (int)m.length() * 6) / 2, 181);
  } else {
    DISP.setTextSize(2);
    DISP.setCursor(10 + (300 - (int)m.length() * 12) / 2, 177);
  }
  DISP.print(m);
}

static void wifiSetupDrawScreen() {
  DISP.clear(COL_BG);
  DISP.setTextColor(COL_TEXT, COL_BG);
  DISP.setTextSize(3);
  DISP.setCursor(10, 6);
  DISP.println(DELLMONITOR_NAME);
  DISP.setTextColor(COL_MUTED, COL_BG);
  DISP.setTextSize(1);
  DISP.setCursor(10, 34);
  DISP.println("Configurar Wi-Fi");

  // Passo 1: o AP do aparelho.
  drawStepBadge(19, 57, "1");
  DISP.setTextColor(COL_TEXT, COL_BG);
  DISP.setTextSize(1);
  DISP.setCursor(32, 53);
  DISP.println("Conecte no Wi-Fi:");
  DISP.setTextColor(COL_ACCENT, COL_BG);
  DISP.setTextSize(2);
  DISP.setCursor(10, 66);
  DISP.println(SETUP_AP_SSID);

  // Passo 2: QR code + URL.
  drawStepBadge(19, 99, "2");
  DISP.setTextColor(COL_TEXT, COL_BG);
  DISP.setTextSize(1);
  DISP.setCursor(32, 95);
  DISP.println("Escaneie o QR ou abra:");
  DISP.setTextSize(2);
  DISP.setCursor(10, 108);
  DISP.println("192.168.4.1");
  DISP.qrcode("http://192.168.4.1", 216, 44, 96, 3, true);
  DISP.setTextColor(COL_MUTED, COL_BG);
  DISP.setTextSize(1);
  DISP.setCursor(216 + (96 - 8 * 6) / 2, 144);
  DISP.println("escaneie");

  // Passo 3: escolher a rede no portal.
  drawStepBadge(19, 141, "3");
  DISP.setTextColor(COL_TEXT, COL_BG);
  DISP.setTextSize(1);
  DISP.setCursor(32, 137);
  DISP.println("Escolha sua rede,");
  DISP.setCursor(32, 149);
  DISP.println("digite a senha e salve.");

  wifiSetupSetStatus("Aguardando conexao...", COL_TEXT);
  DISP.setTextColor(COL_MUTED, COL_BG);
  DISP.setTextSize(1);
  DISP.setCursor(10, 216);
  DISP.println("Depois de conectar: dellmonitor.local");
}

// ---------------------------------------------------------------------------
// Portal captivo (página servida pelo aparelho)
// ---------------------------------------------------------------------------

// Escapa texto para HTML / atributo.
static String htmlEsc(const String& s) {
  String o = s;
  o.replace("&", "&amp;");
  o.replace("<", "&lt;");
  o.replace(">", "&gt;");
  o.replace("\"", "&quot;");
  return o;
}

// Barras de sinal por RSSI: 4 níveis (▂▄▆█ cheios conforme a força).
static String signalBars(int rssi) {
  int n = (rssi >= -55) ? 4 : (rssi >= -65) ? 3 : (rssi >= -75) ? 2 : 1;
  String b;
  for (int i = 0; i < 4; i++) b += (i < n) ? "▂" : "▁";
  return b;
}

static const char* PORTAL_STYLE =
    "<style>body{font-family:sans-serif;max-width:480px;margin:0 auto;"
    "padding:16px;color:#111}h1{font-size:22px;margin:8px 0}"
    "p{color:#555}.net{display:block;width:100%;text-align:left;"
    "padding:12px;margin:6px 0;font-size:16px;border:1px solid #ccc;"
    "border-radius:8px;background:#fff;cursor:pointer}"
    ".net b{float:right;color:#888}label{font-weight:bold}"
    "input{width:100%;padding:10px;font-size:16px;margin:6px 0 12px;"
    "border:1px solid #ccc;border-radius:8px;box-sizing:border-box}"
    "button.save{width:100%;padding:14px;font-size:17px;background:#f5a623;"
    "border:0;border-radius:8px;cursor:pointer}small{color:#888}"
    ".ok{color:#0a0}.err{color:#c00}</style>";

static String portalFormHtml() {
  // Scan de redes (bloqueante, ~2 s). Deduplica SSID, fica o mais forte.
  int n = WiFi.scanNetworks(false);
  struct Net {
    String ssid;
    int rssi;
    bool open;
  };
  Net nets[12];
  int count = 0;
  for (int i = 0; i < n && count < 12; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    int rssi = WiFi.RSSI(i);
    bool open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
    bool dup = false;
    for (int j = 0; j < count; j++) {
      if (nets[j].ssid == ssid) {
        dup = true;
        if (rssi > nets[j].rssi) {
          nets[j].rssi = rssi;
          nets[j].open = open;
        }
        break;
      }
    }
    if (!dup) {
      nets[count].ssid = ssid;
      nets[count].rssi = rssi;
      nets[count].open = open;
      count++;
    }
  }
  // Ordena por sinal (mais forte primeiro).
  for (int i = 0; i < count; i++)
    for (int j = i + 1; j < count; j++)
      if (nets[j].rssi > nets[i].rssi) {
        Net t = nets[i];
        nets[i] = nets[j];
        nets[j] = t;
      }

  String h = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
             "<meta name='viewport' content='width=device-width,initial-scale=1'>"
             "<title>AI Monitor — Wi-Fi</title>" +
             String(PORTAL_STYLE) +
             "<script>function pick(s){document.getElementById('ssid').value=s;"
             "document.getElementById('pass').focus();}"
             "function tog(cb){document.getElementById('pass').type="
             "cb.checked?'text':'password';}</script>"
             "</head><body>"
             "<h1>AI Monitor</h1>"
             "<p>Escolha a rede Wi-Fi do aparelho e informe a senha. "
             "Depois, o IP do Hub (o computador onde roda o Hub).</p>"
             "<form action='/save' method='POST'>";
  if (count == 0) {
    h += "<p><b>Nenhuma rede encontrada.</b> "
         "Digite o nome da rede manualmente abaixo.</p>";
  } else {
    h += "<p><b>Redes encontradas — toque para escolher:</b></p>";
    for (int i = 0; i < count; i++) {
      h += "<button type='button' class='net' onclick=\"pick(this.dataset.ssid)\" "
           "data-ssid='" +
           htmlEsc(nets[i].ssid) + "'>" + htmlEsc(nets[i].ssid) +
           "<b>" + signalBars(nets[i].rssi) + " " +
           (nets[i].open ? "aberta" : "🔒") + "</b></button>";
    }
  }
  h += "Rede Wi-Fi (SSID):<br><input id='ssid' name='ssid' required>"
       "Senha:<br><input id='pass' name='pass' type='password'>"
       "<label><input type='checkbox' style='width:auto' "
       "onchange='tog(this)'> mostrar senha</label><br><br>"
       "Hub (IP do PC):<br><input name='hub' placeholder='192.168.0.10'>"
       "<small>Vazio = modo de demonstracao (sessoes falsas).</small><br><br>"
       "Porta do Hub:<br><input name='port' value='8790'><br><br>"
       "Token do Hub:<br><input name='tok' type='password' autocomplete='off' "
       "placeholder='aparece no console do Hub'>"
       "<small>Obrigatorio com Hub. Fica salvo so no aparelho.</small><br><br>"
       "<button class='save' type='submit'>Salvar e conectar</button>"
       "</form></body></html>";
  return h;
}

static String portalProgressHtml() {
  return String("<!DOCTYPE html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<meta http-equiv='refresh' content='2;url=/result'>"
                "<title>AI Monitor — conectando</title>") +
         PORTAL_STYLE +
         "</head><body><h1>Conectando...</h1>"
         "<p>O AI Monitor está tentando entrar na sua rede. "
         "Esta página atualiza sozinha.</p></body></html>";
}

static String portalResultHtml() {
  String h = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
             "<meta name='viewport' content='width=device-width,initial-scale=1'>"
             "<title>AI Monitor — resultado</title>" +
             String(PORTAL_STYLE) + "</head><body>";
  if (portalState == PORTAL_OK) {
    h += "<h1 class='ok'>Conectado!</h1><p>O AI Monitor entrou na sua rede."
         "</p><p>IP do aparelho: <b>" +
         portalOkIp + "</b></p><p>Ele vai reiniciar e começar a funcionar.</p>";
  } else if (portalState == PORTAL_FAIL) {
    h += "<h1 class='err'>Não conectou</h1><p><b>" +
         htmlEsc(portalFailReason) +
         "</b></p><p>Confira a senha e o nome da rede.</p>"
         "<p><a href='/'>← Voltar e tentar de novo</a></p>";
  } else {
    h += "<h1>Conectando...</h1><p>Aguarde, atualizando.</p>"
         "<meta http-equiv='refresh' content='2;url=/result'>";
  }
  return h + "</body></html>";
}

// Tenta conectar com as credenciais salvas (modo AP+STA para o portal
// continuar no ar). Atualiza a tela do aparelho com o progresso.
static bool portalTryConnect() {
  prefs.begin("dellmonitor", true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  prefs.end();
  if (ssid.length() == 0) {
    portalFailReason = "Nenhuma rede configurada.";
    return false;
  }
  String shortSsid = ssid;
  if (shortSsid.length() > 12) shortSsid = shortSsid.substring(0, 11) + "~";
  wifiSetupSetStatus("Conectando a " + shortSsid + "...", COL_ACCENT);
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000UL) {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    portalOkIp = WiFi.localIP().toString();
    wifiSetupSetStatus("Conectado! IP " + portalOkIp, COL_WORKING);
    return true;
  }
  wl_status_t st = WiFi.status();
  if (st == WL_NO_SSID_AVAIL) {
    portalFailReason = "Rede nao encontrada.";
  } else if (st == WL_CONNECT_FAILED) {
    portalFailReason = "Senha errada — tente de novo.";
  } else {
    portalFailReason = "Nao foi possivel conectar.";
  }
  wifiSetupSetStatus(portalFailReason, 0xF800);
  // Credenciais erradas: apaga para o aparelho voltar ao portal no boot.
  wifiForget();
  return false;
}

// ---------------------------------------------------------------------------
// API pública (wifi_setup.h)
// ---------------------------------------------------------------------------

bool wifiLoadSaved(AppState& st) {
  prefs.begin("dellmonitor", true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  st.hubHost = prefs.getString("hub", "");
  st.hubPort = prefs.getUInt("hubport", HUB_DEFAULT_PORT);
  st.hubToken = prefs.getString("hubtok", "");
  prefs.end();
  // 8787 passou a ser do headroom_proxy do Hermes: o hub-stack-chan mudou para 8790.
  if (st.hubPort == 8787) {
    st.hubPort = HUB_DEFAULT_PORT;
    prefs.begin("dellmonitor", false);
    prefs.putUInt("hubport", st.hubPort);
    prefs.end();
    Serial.printf("[wifi] porta do Hub migrada 8787 -> %u (hub-stack-chan)\n", (unsigned)st.hubPort);
  }
  if (ssid.length() == 0) return false;
  st.wifiSsid = ssid;
  // A senha fica só nas Preferences; não é exibida nem registrada.
  (void)pass;
  return true;
}

void wifiSave(const String& ssid, const String& pass, const String& hubHost,
              uint16_t hubPort, const String& hubToken) {
  prefs.begin("dellmonitor", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.putString("hub", hubHost);
  prefs.putUInt("hubport", hubPort);
  prefs.putString("hubtok", hubToken);
  prefs.end();
}

void wifiForget() {
  prefs.begin("dellmonitor", false);
  prefs.clear();
  prefs.end();
}

bool wifiConnect(AppState& st) {
  prefs.begin("dellmonitor", true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  prefs.end();
  if (ssid.length() == 0) return false;

  // Tela de progresso no aparelho (em vez de só o boot estático).
  DISP.clear(COL_BG);
  DISP.setTextColor(COL_TEXT, COL_BG);
  DISP.setTextSize(3);
  DISP.setCursor(16, 50);
  DISP.println(DELLMONITOR_NAME);
  DISP.setTextColor(COL_MUTED, COL_BG);
  DISP.setTextSize(1);
  DISP.setCursor(16, 90);
  DISP.println("Conectando a");
  String shortSsid = ssid;
  if (shortSsid.length() > 24) shortSsid = shortSsid.substring(0, 23) + "~";
  DISP.setTextColor(COL_ACCENT, COL_BG);
  DISP.setTextSize(2);
  DISP.setCursor(16, 104);
  DISP.println(shortSsid);
  // Barra de progresso (20 s).
  DISP.drawRect(16, 132, 200, 10, COL_MUTED);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // sem modem-sleep: WebSocket estável e sem atraso
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000UL) {
    int fw = (int)((millis() - start) * 198 / 20000UL);
    DISP.fillRect(17, 133, fw, 8, COL_ACCENT);
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    st.ipAddress = WiFi.localIP().toString();
    st.wifiSsid = ssid;
    DISP.setTextColor(COL_WORKING, COL_BG);
    DISP.setTextSize(2);
    DISP.setCursor(16, 152);
    DISP.print("Conectado! IP ");
    DISP.println(st.ipAddress);
    // Relógio via NTP (America/Sao_Paulo, sem horário de verão).
    configTime(TZ_OFFSET_SEC, 0, NTP_SERVER_1, NTP_SERVER_2);
    delay(700);
    return true;
  }
  // Erro específico, na tela, antes de voltar ao portal.
  wl_status_t wst = WiFi.status();
  DISP.setTextColor(0xF800, COL_BG);
  DISP.setTextSize(2);
  DISP.setCursor(16, 152);
  if (wst == WL_NO_SSID_AVAIL) {
    DISP.println("Rede nao encontrada");
  } else if (wst == WL_CONNECT_FAILED) {
    DISP.println("Senha errada");
  } else {
    DISP.println("Nao foi possivel conectar");
  }
  DISP.setTextColor(COL_MUTED, COL_BG);
  DISP.setTextSize(1);
  DISP.setCursor(16, 176);
  DISP.println("voltando ao portal...");
  delay(2500);
  return false;
}

void wifiStartMDNS() {
  if (!MDNS.begin(MDNS_HOSTNAME)) {
    Serial.println("[wifi] mDNS falhou");
    return;
  }
  MDNS.addService("http", "tcp", 80);
  MDNS.addService("dellmonitor", "tcp", HUB_DEFAULT_PORT);
  Serial.println("[wifi] mDNS: " MDNS_HOSTNAME ".local");
}

void wifiStartSetupPortal() {
  portalState = PORTAL_IDLE;
  portalRestartAt = 0;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(SETUP_AP_SSID);
  IPAddress apIp = WiFi.softAPIP();
  Serial.print("[wifi] AP ");
  Serial.print(SETUP_AP_SSID);
  Serial.print(" em ");
  Serial.println(apIp);

  // Portal captivo: todo DNS resolve para o próprio AP.
  dnsServer.start(53, "*", apIp);

  portalServer.on("/", HTTP_GET, []() {
    portalServer.send(200, "text/html", portalFormHtml());
  });
  portalServer.on("/save", HTTP_POST, []() {
    String ssid = portalServer.arg("ssid");
    String pass = portalServer.arg("pass");
    String hub = portalServer.arg("hub");
    String tok = portalServer.arg("tok");
    tok.trim();
    uint16_t port = (uint16_t)portalServer.arg("port").toInt();
    if (port == 0) port = HUB_DEFAULT_PORT;
    ssid.trim();
    hub.trim();
    if (ssid.length() == 0) {
      portalServer.send(400, "text/plain", "SSID vazio.");
      return;
    }
    if (hub.length() && tok.length() < 12) {
      portalServer.send(400, "text/plain",
                        "Informe o token do Hub (12+ caracteres).");
      return;
    }
    wifiSave(ssid, pass, hub, port, tok);
    portalState = PORTAL_CONNECTING;
    portalServer.send(200, "text/html", portalProgressHtml());
  });
  portalServer.on("/result", HTTP_GET, []() {
    portalServer.send(200, "text/html", portalResultHtml());
  });
  portalServer.onNotFound([]() {
    portalServer.sendHeader("Location", "/", true);
    portalServer.send(302, "text/plain", "");
  });
  portalServer.begin();
  portalRunning = true;

  wifiSetupDrawScreen();
}

void wifiPortalLoop() {
  if (!portalRunning) return;
  dnsServer.processNextRequest();
  portalServer.handleClient();
  if (portalState == PORTAL_CONNECTING) {
    if (portalTryConnect()) {
      portalState = PORTAL_OK;
      portalRestartAt = millis() + 4000;
    } else {
      portalState = PORTAL_FAIL;
    }
  }
  if (portalRestartAt != 0 && millis() > portalRestartAt) {
    delay(500);
    ESP.restart();
  }
}

uint8_t settingsLoadVolume() {
  prefs.begin("dellmonitor", true);
  uint8_t v = prefs.getUChar("vol", 100);
  prefs.end();
  return v > 100 ? 100 : v;
}

void settingsSaveVolume(uint8_t pct) {
  prefs.begin("dellmonitor", false);
  prefs.putUChar("vol", pct);
  prefs.end();
}
