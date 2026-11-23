// AI Monitor — voz (ver voice.h).
#include "voice.h"

#include <ArduinoJson.h>
#include <M5Unified.h>
#include <WebSocketsClient.h>
#include <esp_heap_caps.h>
#include <freertos/queue.h>

#include "config.h"

namespace {

WebSocketsClient vws;
bool wsEnabled = false;
volatile VoiceState g_state = VOICE_OFF;
volatile bool g_idlePending = false;  // "idle" chegou durante a fala
volatile bool g_conn = false;
volatile uint32_t g_wakes = 0;
volatile bool g_stopReq = false;

// ---- microfone: PCM16 16 kHz, quadros de 60 ms -------------------------
constexpr uint32_t MIC_RATE = 16000;
constexpr size_t MIC_SAMPLES = 960;
constexpr uint32_t MIC_BUFS = 8;
int16_t* micBuf[MIC_BUFS];
uint32_t micQueued = 0, micSent = 0;

// ---- fala: PCM16 24 kHz numa fila circular na PSRAM -----------------------
constexpr uint32_t OUT_RATE = 24000;
constexpr size_t RING = OUT_RATE * 40;      // 40 s
constexpr size_t PREBUF = OUT_RATE / 4;     // 250 ms antes de começar a tocar
constexpr size_t CHUNK = 2400;              // 100 ms por bloco entregue ao alto-falante
constexpr uint8_t VOICE_CH = 0;
constexpr uint8_t TONE_CH = 1;
int16_t* ring = nullptr;
size_t rHead = 0, rTail = 0;                // só a tarefa de voz mexe (eventos WS vêm dela)
int16_t* pool[3];
uint8_t poolIdx = 0;
bool outActive = false, outStarted = false, endReceived = false;
uint32_t lastBinMs = 0;
// Sons do Hub (arquivos escolhidos no notebook) para os avisos do monitor.
// Bit i = o Hub tem o som SOUND_NAMES[i]; soundMode = a fila guarda um aviso, não fala.
const char* const SOUND_NAMES[] = {"needyou", "finished", "new"};
constexpr uint8_t N_SOUNDS = 3;
volatile uint8_t soundAvail = 0;
char soundSrc[12] = "";           // ferramenta do aviso pedido
volatile int8_t soundReq = -1;     // pedido do loop principal; a tarefa de voz envia
volatile bool soundWaiting = false; // pediu e o áudio ainda não terminou
uint32_t soundReqMs = 0;
bool soundMode = false;
uint8_t oddByte = 0;
bool hasOdd = false;

inline size_t ringCount() { return (rHead + RING - rTail) % RING; }

void ringPush(const uint8_t* data, size_t len) {
  if (hasOdd && len) {
    uint8_t two[2] = {oddByte, data[0]};
    int16_t s;
    memcpy(&s, two, 2);
    ring[rHead] = s;
    rHead = (rHead + 1) % RING;
    data++;
    len--;
    hasOdd = false;
  }
  size_t n = len / 2;
  for (size_t i = 0; i < n; i++) {
    if (ringCount() == RING - 1) rTail = (rTail + 1) % RING;  // cheio: descarta o mais antigo
    int16_t s;
    memcpy(&s, data + 2 * i, 2);
    ring[rHead] = s;
    rHead = (rHead + 1) % RING;
  }
  if (len & 1) {
    oddByte = data[len - 1];
    hasOdd = true;
  }
}

// ---- avisos do monitor ----------------------------------------------------
struct ToneReq {
  uint16_t freq, ms, gap;
};
QueueHandle_t toneQ = nullptr;
uint32_t toneUntil = 0;
uint32_t lastSpkUse = 0;

uint8_t g_spkVolume = SPEAKER_VOLUME;  // 0..255, ajustável nas Configurações

enum Mode : uint8_t { M_NONE, M_MIC, M_SPK };
Mode mode = M_NONE;

void toSpeaker() {
  if (mode == M_SPK) return;
  if (mode == M_MIC) M5.Mic.end();
  M5.Speaker.begin();
  M5.Speaker.setVolume(g_spkVolume);
  mode = M_SPK;
}

void toMic() {
  if (mode == M_MIC) return;
  if (mode == M_SPK) {
    M5.Speaker.stop();
    M5.Speaker.end();
  }
  M5.Mic.begin();
  micQueued = micSent = 0;
  mode = M_MIC;
}

void sendJson(const char* json) {
  if (g_conn) vws.sendTXT(json);
}

uint32_t soundBytes = 0;

void finishSpeaking(bool notify) {
  bool wasStarted = outStarted;
  outActive = outStarted = endReceived = false;
  rHead = rTail = 0;
  hasOdd = false;
  if (soundMode) {  // fim de um aviso: a conversa (se houver) não muda
    Serial.printf("[voz] aviso: fim (%u bytes, tocou %s)\n", (unsigned)soundBytes, wasStarted ? "sim" : "nao");
    soundMode = false;
    soundWaiting = false;
    return;
  }
  if (notify) sendJson("{\"type\":\"played\"}");
  // O "idle" do Hub pode chegar enquanto a resposta ainda toca: aplica agora.
  // (Antes ele era descartado e o robô ficava preso em "ouvindo", recusando
  // os avisos falados do monitor.)
  if (g_idlePending) {
    g_idlePending = false;
    g_state = VOICE_IDLE;
  } else if (g_state == VOICE_SPEAKING) {
    g_state = VOICE_LISTENING;  // o Hub manda "idle" ao fim da conversa
  }
}

void onEvent(WStype_t type, uint8_t* payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED:
      g_conn = true;
      g_state = VOICE_IDLE;
      Serial.println("[voz] conectado ao Hub");
      break;
    case WStype_DISCONNECTED:
      if (g_conn) Serial.println("[voz] desconectado");
      g_conn = false;
      g_state = VOICE_OFF;
      g_idlePending = false;
      soundAvail = 0;
      soundWaiting = false;
      break;
    case WStype_BIN:
      if (!ring) break;
      ringPush(payload, len);
      if (soundMode) {
        if (soundBytes == 0) Serial.printf("[voz] aviso: 1o bloco %u bytes\n", (unsigned)len);
        soundBytes += len;
      }
      outActive = true;
      lastBinMs = millis();
      if (!soundMode) g_state = VOICE_SPEAKING;
      break;
    case WStype_TEXT: {
      JsonDocument doc;
      if (deserializeJson(doc, payload, len)) break;
      const char* mt = doc["type"] | "";
      if (!strcmp(mt, "sounds")) {
        uint8_t m = 0;
        for (JsonVariant n : doc["names"].as<JsonArray>())
          for (uint8_t i = 0; i < N_SOUNDS; i++)
            if (!strcmp(n | "", SOUND_NAMES[i])) m |= (1 << i);
        soundAvail = m;
        Serial.printf("[voz] sons do Hub: 0x%02x\n", m);
        break;
      }
      if (!strcmp(mt, "sound")) {
        const char* ss = doc["state"] | "";
        if (!strcmp(ss, "start")) {
          Serial.println("[voz] aviso: inicio");
          soundBytes = 0;
          soundMode = true;
          rHead = rTail = 0;
          hasOdd = false;
          outActive = true;
          outStarted = endReceived = false;
          lastBinMs = millis();
        } else if (!strcmp(ss, "end")) {
          if (soundMode) {
            endReceived = true;
            if (ringCount() == 0) finishSpeaking(false);
          } else {
            soundWaiting = false;  // o Hub recusou (ocupado / sem o arquivo)
          }
        }
        break;
      }
      if (strcmp(mt, "voice") != 0) break;
      const char* st = doc["state"] | "";
      if (!strcmp(st, "idle")) {
        if (!outActive) g_state = VOICE_IDLE;
        else g_idlePending = true;
      } else if (!strcmp(st, "listening")) {
        g_idlePending = false;
        g_state = VOICE_LISTENING;
        g_wakes = g_wakes + 1;  // sem bipe: trocar para o alto-falante cortaria o começo da sua frase
      } else if (!strcmp(st, "thinking")) {
        if (!outActive) g_state = VOICE_THINKING;
      } else if (!strcmp(st, "speaking")) {
        g_state = VOICE_SPEAKING;
        outActive = true;
        lastBinMs = millis();
      } else if (!strcmp(st, "end")) {
        endReceived = true;
        if (!outActive) finishSpeaking(true);
      } else if (!strcmp(st, "error")) {
        g_state = VOICE_ERROR;
        finishSpeaking(false);
      }
      break;
    }
    default:
      if (soundMode) Serial.printf("[voz] aviso: evento WS %d (%u bytes)\n", (int)type, (unsigned)len);
      break;
  }
}

void serviceMic() {
  while (M5.Mic.isRecording() < 2 && micQueued - micSent < MIC_BUFS) {
    if (!M5.Mic.record(micBuf[micQueued % MIC_BUFS], MIC_SAMPLES, MIC_RATE)) break;
    micQueued++;
  }
  uint32_t done = micQueued - M5.Mic.isRecording();
  while (micSent < done) {
    if (g_conn) vws.sendBIN((uint8_t*)micBuf[micSent % MIC_BUFS], MIC_SAMPLES * 2);
    micSent++;
  }
}

void servicePlayback(uint32_t now) {
  size_t cnt = ringCount();
  if (!outStarted && (cnt >= PREBUF || (endReceived && cnt > 0))) outStarted = true;
  while (outStarted && cnt > 0 && M5.Speaker.isPlaying(VOICE_CH) < 2) {
    size_t n = cnt < CHUNK ? cnt : CHUNK;
    int16_t* dst = pool[poolIdx];
    for (size_t i = 0; i < n; i++) {
      dst[i] = ring[rTail];
      rTail = (rTail + 1) % RING;
    }
    M5.Speaker.playRaw(dst, n, OUT_RATE, false, 1, VOICE_CH, false);
    poolIdx = (poolIdx + 1) % 3;
    cnt = ringCount();
  }
  bool playing = M5.Speaker.isPlaying(VOICE_CH) > 0;
  if (cnt == 0 && !playing) {
    if (endReceived) {
      finishSpeaking(true);
    } else if (outStarted) {
      outStarted = false;  // faltou áudio: espera juntar de novo
    } else if ((int32_t)(millis() - lastBinMs) > 8000) {  // "now" é de antes do vws.loop()
      finishSpeaking(true);  // o "end" se perdeu
    }
  }
  if (playing) lastSpkUse = now;
}

void voiceTask(void*) {
  for (;;) {
    uint32_t now = millis();
    if (wsEnabled) vws.loop();

    if (g_stopReq) {
      g_stopReq = false;
      if (outActive) {
        if (mode == M_SPK) M5.Speaker.stop(VOICE_CH);
        finishSpeaking(false);
        sendJson("{\"type\":\"stop\"}");
      }
    }

    if (soundReq >= 0) {
      int8_t i = soundReq;
      soundReq = -1;
      char js[80];
      snprintf(js, sizeof js, "{\"type\":\"sound\",\"name\":\"%s\",\"source\":\"%s\"}", SOUND_NAMES[i],
               soundSrc);
      sendJson(js);
    }
    if (soundWaiting && !soundMode && now - soundReqMs > 3000) soundWaiting = false;  // sem resposta

    bool toneWaiting = uxQueueMessagesWaiting(toneQ) > 0;
    bool toneOn = (int32_t)(toneUntil - now) > 0;
    bool needSpk = outActive || toneWaiting || toneOn || (now - lastSpkUse < 200);
    if (needSpk || !g_conn) {
      toSpeaker();
    } else {
      toMic();
    }

    if (mode == M_SPK) {
      if (outActive) servicePlayback(now);
      if (!toneOn && !outActive) {
        ToneReq t;
        if (xQueueReceive(toneQ, &t, 0) == pdTRUE) {
          M5.Speaker.tone(t.freq, t.ms, TONE_CH);
          toneUntil = now + t.ms + t.gap;
          lastSpkUse = now + t.ms;
        }
      }
    } else if (mode == M_MIC) {
      serviceMic();
    }
    vTaskDelay(1);
  }
}

}  // namespace

void voiceBegin(const String& host, uint16_t port, const String& token) {
  toneQ = xQueueCreate(16, sizeof(ToneReq));
  for (auto& b : micBuf) b = (int16_t*)heap_caps_malloc(MIC_SAMPLES * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  for (auto& p : pool) p = (int16_t*)heap_caps_malloc(CHUNK * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  ring = (int16_t*)heap_caps_malloc(RING * 2, MALLOC_CAP_SPIRAM);
  if (!ring) Serial.println("[voz] sem PSRAM para a fala");
  if (host.length() && token.length() && ring) {
    String path = "/voice?token=" + token;
    vws.onEvent(onEvent);
    vws.setReconnectInterval(3000);
    vws.begin(host.c_str(), port, path.c_str());
    wsEnabled = true;
    Serial.printf("[voz] Hub de voz em %s:%u\n", host.c_str(), port);
  }
  xTaskCreatePinnedToCore(voiceTask, "voz", 8192, nullptr, 3, nullptr, 0);
}

VoiceState voiceState() { return g_state; }
bool voiceConnected() { return g_conn; }

void voiceTone(uint16_t freq, uint16_t ms, uint16_t gap_ms) {
  if (!toneQ) return;
  ToneReq t{freq, ms, gap_ms};
  xQueueSend(toneQ, &t, 0);
}

bool voiceSoundBusy() {
  if (!toneQ) return false;
  return uxQueueMessagesWaiting(toneQ) > 0 || (int32_t)(toneUntil - millis()) > 0 || outActive ||
         soundWaiting;
}

bool voiceSound(const char* name, const char* source) {
  if (!g_conn || g_state != VOICE_IDLE || outActive || soundWaiting) {
    Serial.printf("[voz] aviso %s sem Hub: conn %d estado %d fala %d pendente %d sons 0x%02x\n", name, g_conn,
                  (int)g_state, outActive, soundWaiting, soundAvail);
    return false;
  }
  for (uint8_t i = 0; i < N_SOUNDS; i++) {
    if (strcmp(name, SOUND_NAMES[i]) || !(soundAvail & (1 << i))) continue;
    soundReqMs = millis();
    soundWaiting = true;
    strlcpy(soundSrc, source ? source : "", sizeof soundSrc);
    soundReq = i;
    return true;
  }
  return false;
}

void voiceStopSpeaking() { g_stopReq = true; }
uint32_t voiceWakeCount() { return g_wakes; }

void voiceSetVolume(uint8_t pct) {
  if (pct > 100) pct = 100;
  g_spkVolume = (uint8_t)((uint16_t)SPEAKER_VOLUME * pct / 100);
  M5.Speaker.setVolume(g_spkVolume);
}
