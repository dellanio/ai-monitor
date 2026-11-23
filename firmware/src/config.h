// AI Monitor v0.2 — constantes de configuração do projeto.
// Hardware: M5Stack CoreS3 (ESP32-S3, display IPS touch 320x240 ILI9342C).
#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// Identidade do projeto (nome neutro; NÃO usar a marca "Miblo" na interface)
// ---------------------------------------------------------------------------
#define DELLMONITOR_NAME "AI Monitor"
#define DELLMONITOR_VERSION "0.2.0"
#define MDNS_HOSTNAME "aimonitor"     // -> aimonitor.local
#define SETUP_AP_SSID "AIMonitor-Setup"

// ---------------------------------------------------------------------------
// Servos da cabeça do Stack-chan
// CONFIRMAR pinagem do Stack-chan K151 antes de ligar os servos.
// Os valores abaixo são PLACEHOLDERS DESATIVADOS (-1 = servo desligado).
// Não afirme uma pinagem sem conferir na documentação oficial do M5Stack
// ou no esquemático do kit K151. Com -1, o código de aceno não faz nada.
// Para ativar: coloque os GPIOs reais e SERVO_ENABLED 1.
// ---------------------------------------------------------------------------
#define SERVO_YAW_PIN   (-1)   // CONFIRMAR pinagem do Stack-chan K151
#define SERVO_PITCH_PIN (-1)   // CONFIRMAR pinagem do Stack-chan K151
#define SERVO_ENABLED   0      // 0 = servos desligados (seguro); 1 = aceno ativo

// ---------------------------------------------------------------------------
// Comportamento
// ---------------------------------------------------------------------------
#define MOCK_ALERT_EVERY_MS 45000UL   // no modo mock, um alerta a cada ~45 s
#define HUB_DEFAULT_PORT 8790         // porta padrão do Hub (configurável)
#define HUB_RECONNECT_MS 5000UL
#define VOICE_DEFAULT_PORT 8788       // WebSocket /voice do Hub (Hey Hermes + canal de voz)
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.nist.gov"
#define TZ_OFFSET_SEC (-3 * 3600)     // America/Sao_Paulo (sem horário de verão)
#ifndef DISPLAY_ROTATION
#define DISPLAY_ROTATION 1            // 0..3; ajuste se a tela ficar de lado
#endif
#define SPEAKER_VOLUME 255            // 0..255 (maximo)
#define MAX_SESSIONS 20               // igual ao limite do produto de referência

// ---------------------------------------------------------------------------
// Tema "espaco": roxo profundo + azul cintilante (v0.2)
// ---------------------------------------------------------------------------
#define RGB565(r, g, b) \
  ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define COL_BG        RGB565(10, 8, 28)     // fundo (espaco)
#define COL_BG2       RGB565(20, 14, 48)    // fundo do degrade
#define COL_PANEL     RGB565(26, 22, 56)    // cartoes
#define COL_PANEL2    RGB565(38, 32, 80)    // cartao em destaque / botao neutro
#define COL_BORDER    RGB565(64, 56, 120)
#define COL_TEXT      RGB565(238, 236, 255)
#define COL_MUTED     RGB565(146, 140, 196)
#define COL_WORKING   RGB565(80, 232, 170)  // menta
#define COL_NEEDYOU   RGB565(255, 184, 64)  // ambar
#define COL_FINISHED  RGB565(96, 176, 255)  // azul cintilante
#define COL_IDLE      RGB565(112, 106, 152)
#define COL_ACCENT    RGB565(156, 116, 255) // violeta
#define COL_DANGER    RGB565(255, 92, 112)

// Rosto da Nous-Girl: o traco original (escuro) fica intacto e o "papel"
// (branco da arte) ganha a cor do humor. Nunca inverter: vira negativo.
#define FACE_LINE     RGB565(16, 12, 40)
#define INK_NEUTRAL   RGB565(232, 228, 255)  // papel lavanda
#define INK_WORKING   RGB565(176, 240, 255)  // ciano claro
#define INK_NEEDYOU   RGB565(255, 214, 128)  // ambar
#define INK_FINISHED  RGB565(176, 210, 255)  // azul claro
#define INK_SAD       RGB565(188, 186, 204)  // cinza
#define INK_SLEEPING  RGB565(184, 168, 236)  // violeta

#define FACE_IDLE_TIMEOUT_MS   45000UL        // sem toque -> rosto em tela cheia
#define FACE_SLEEP_AFTER_MS    (10UL * 60UL * 1000UL)  // sem atividade -> dorme
#define FACE_HAPPY_MS          30000UL        // feliz depois de uma tarefa terminar
#define FACE_GLANCE_EVERY_MS   20000UL        // olhadinha para o lado quando ociosa
