// AI Monitor v0.2 — player dos GIFs da Nous-Girl com tinta por humor.
#include "face.h"

#include <AnimatedGIF.h>

#include "config.h"
#include "gfx.h"
#include "nous_avatar.h"

// GIFs embutidos (board_build.embed_files no platformio.ini).
#define NG_EMBED(name)                                                       \
  extern const uint8_t ng_##name##_start[] asm(                              \
      "_binary_assets_nousgirl_" #name "_gif_start");                        \
  extern const uint8_t ng_##name##_end[] asm("_binary_assets_nousgirl_" #name \
                                             "_gif_end");
NG_EMBED(neutral)
NG_EMBED(thinking)
NG_EMBED(happy)
NG_EMBED(listening)
NG_EMBED(sad)
NG_EMBED(sleeping)
NG_EMBED(look_left)
NG_EMBED(look_right)

struct GifAsset {
  const uint8_t* start;
  const uint8_t* end;
};
static const GifAsset ASSETS[MOOD_COUNT] = {
    {ng_neutral_start, ng_neutral_end},     {ng_thinking_start, ng_thinking_end},
    {ng_happy_start, ng_happy_end},         {ng_listening_start, ng_listening_end},
    {ng_sad_start, ng_sad_end},             {ng_sleeping_start, ng_sleeping_end},
    {ng_look_left_start, ng_look_left_end}, {ng_look_right_start, ng_look_right_end},
};

static AnimatedGIF s_gif;
static bool s_open = false;
static FaceMood s_mood = MOOD_NEUTRAL;
static uint16_t s_ink = INK_NEUTRAL;
static uint32_t s_nextFrameAt = 0;
static bool s_needOpen = true;

// LUT do quadro atual: indice da paleta do GIF -> RGB565 tingido.
static uint16_t s_lut[256];
static const uint8_t* s_lutPalette = nullptr;
static uint16_t s_lutInk = 0;
static uint16_t s_line[320];

uint16_t faceTint(uint8_t lum, uint16_t ink, uint16_t bg) {
  // lum 255 = papel (fundo), 0 = traco (tinta).
  uint32_t t = 255 - lum;
  int ir = (ink >> 11) & 0x1F, ig = (ink >> 5) & 0x3F, ib = ink & 0x1F;
  int br = (bg >> 11) & 0x1F, bgc = (bg >> 5) & 0x3F, bb = bg & 0x1F;
  int r = br + (int)((ir - br) * (int)t / 255);
  int g = bgc + (int)((ig - bgc) * (int)t / 255);
  int b = bb + (int)((ib - bb) * (int)t / 255);
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static void buildLut(const uint8_t* pal24) {
  for (int i = 0; i < 256; i++) {
    const uint8_t* p = pal24 + i * 3;
    uint8_t lum = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
    s_lut[i] = faceTint(lum, FACE_LINE, s_ink);  // traco escuro, papel colorido
  }
  s_lutPalette = pal24;
  s_lutInk = s_ink;
}

static void gifDraw(GIFDRAW* d) {
  if (!d->pPalette24) return;
  if (d->pPalette24 != s_lutPalette || s_lutInk != s_ink) buildLut(d->pPalette24);
  LovyanGFX& g = G();
  int y = d->iY + d->y;
  if (y < 0 || y >= 240) return;
  int w = d->iWidth;
  if (d->iX + w > 320) w = 320 - d->iX;
  if (w <= 0) return;
  uint8_t* s = d->pPixels;
  if (d->ucDisposalMethod == 2) {  // restaura o fundo nos transparentes
    for (int x = 0; x < w; x++)
      if (s[x] == d->ucTransparent) s[x] = d->ucBackground;
    d->ucHasTransparency = 0;
  }
  if (!d->ucHasTransparency) {
    for (int x = 0; x < w; x++) s_line[x] = s_lut[s[x]];
    g.pushImage(d->iX, y, w, 1, (const lgfx::rgb565_t*)s_line);
    return;
  }
  // Com transparencia: envia so os trechos opacos.
  int x = 0;
  while (x < w) {
    while (x < w && s[x] == d->ucTransparent) x++;
    int start = x, n = 0;
    while (x < w && s[x] != d->ucTransparent) s_line[n++] = s_lut[s[x++]];
    if (n) g.pushImage(d->iX + start, y, n, 1, (const lgfx::rgb565_t*)s_line);
  }
}

bool faceBegin() {
  s_gif.begin(GIF_PALETTE_RGB888);
  s_needOpen = true;
  return true;
}

static void openCurrent() {
  if (s_open) s_gif.close();
  const GifAsset& a = ASSETS[s_mood];
  s_open = s_gif.open((uint8_t*)a.start, (int)(a.end - a.start), gifDraw) != 0;
  if (!s_open) Serial.printf("[face] falha ao abrir GIF %d\n", (int)s_mood);
  s_lutPalette = nullptr;
  s_nextFrameAt = 0;
  s_needOpen = false;
}

void faceSetMood(FaceMood mood, uint16_t ink) {
  if (mood == s_mood && ink == s_ink && !s_needOpen) return;
  s_mood = mood;
  s_ink = ink;
  s_needOpen = true;
}

void faceInvalidate() { s_needOpen = true; }

FaceMood faceMood() { return s_mood; }

bool faceUpdate(uint32_t now) {
  if (s_needOpen) {
    // O 1o quadro e completo; limpa antes para nao sobrar outra tela.
    G().fillScreen(s_ink);
    openCurrent();
  }
  if (!s_open || now < s_nextFrameAt) return false;
  int delayMs = 100;
  int more = s_gif.playFrame(false, &delayMs);
  if (delayMs < 40) delayMs = 40;
  s_nextFrameAt = now + (uint32_t)delayMs;
  if (more == 0) {
    s_gif.reset();
    return true;
  }
  return false;
}

void faceDrawAvatar(LovyanGFX& g, int x, int y, int size, uint16_t paper,
                    uint16_t line, bool round) {
  const int n = NOUS_AVATAR_SIZE;
  float r = size / 2.0f;
  for (int yy = 0; yy < size; yy++) {
    int sy = yy * n / size;
    for (int xx = 0; xx < size; xx++) {
      if (round) {
        float dx = xx - r + 0.5f, dy = yy - r + 0.5f;
        if (dx * dx + dy * dy > r * r) continue;
      }
      int sx = xx * n / size;
      g.drawPixel(x + xx, y + yy, faceTint(NOUS_AVATAR_LUMA[sy * n + sx], line, paper));
    }
  }
  if (round) g.drawCircle(x + size / 2, y + size / 2, size / 2, paper);
}
