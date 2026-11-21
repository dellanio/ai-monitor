#include "gfx.h"

static M5Canvas s_canvas(&M5.Display);
static bool s_ok = false;

bool gfxBegin() {
  s_canvas.setPsram(true);
  s_canvas.setColorDepth(16);
  s_ok = s_canvas.createSprite(M5.Display.width(), M5.Display.height()) != nullptr;
  if (!s_ok) Serial.println("[gfx] sem PSRAM para o quadro: desenho direto");
  return s_ok;
}

LovyanGFX& G() {
  if (s_ok) return s_canvas;
  return M5.Display;
}

void gfxPresent() {
  if (s_ok) s_canvas.pushSprite(0, 0);
}

bool gfxBuffered() { return s_ok; }
