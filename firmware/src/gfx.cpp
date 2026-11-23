#include "gfx.h"
#include "hw.h"

static M5Canvas s_canvas(&DISP);
static bool s_ok = false;

bool gfxBegin() {
  s_canvas.setPsram(true);
  s_canvas.setColorDepth(16);
  s_ok = s_canvas.createSprite(DISP.width(), DISP.height()) != nullptr;
  if (!s_ok) Serial.println("[gfx] sem PSRAM para o quadro: desenho direto");
  return s_ok;
}

LovyanGFX& G() {
  if (s_ok) return s_canvas;
  return DISP;
}

uint32_t g_presentMaxUs = 0, g_frames = 0;
void gfxPresent() {
  uint32_t t0 = micros();
  if (s_ok) s_canvas.pushSprite(0, 0);
  uint32_t dt = micros() - t0;
  if (dt > g_presentMaxUs) g_presentMaxUs = dt;
  g_frames++;
}

bool gfxBuffered() { return s_ok; }

// Captura do quadro pela USB (bancada): cabeçalho de texto + RGB565 cru.
void gfxDumpSerial() {
  if (!s_ok) { Serial.println("SHOT 0 0"); return; }
  int w = s_canvas.width(), h = s_canvas.height();
  Serial.printf("SHOT %d %d\n", w, h);
  Serial.flush();
  const uint8_t* buf = (const uint8_t*)s_canvas.getBuffer();
  size_t n = (size_t)w * h * 2;
  for (size_t off = 0; off < n; off += 1024) Serial.write(buf + off, min((size_t)1024, n - off));
  Serial.flush();
  Serial.println("\nENDSHOT");
}
