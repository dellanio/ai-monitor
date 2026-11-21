// AI Monitor v0.2 — rosto da Nous-Girl (arte de homenagem, uso pessoal).
// Toca os GIFs 320x240 embutidos no firmware e pinta o traco preto e
// branco com a cor do humor sobre o fundo do tema.
#pragma once
#include <Arduino.h>
#include <M5Unified.h>

#include "config.h"

enum FaceMood {
  MOOD_NEUTRAL = 0,
  MOOD_THINKING,
  MOOD_HAPPY,
  MOOD_LISTENING,
  MOOD_SAD,
  MOOD_SLEEPING,
  MOOD_LOOK_LEFT,
  MOOD_LOOK_RIGHT,
  MOOD_COUNT
};

bool faceBegin();
// Troca humor e tinta; reinicia a animacao se algo mudou.
void faceSetMood(FaceMood mood, uint16_t ink);
// Obriga a redesenhar o quadro inteiro (ao voltar para a tela do rosto).
void faceInvalidate();
// Decodifica o proximo quadro em G() quando for a hora. Devolve true se
// a animacao completou um ciclo neste chamado.
bool faceUpdate(uint32_t nowMs);
FaceMood faceMood();

// Avatar pequeno: traco `line` sobre papel `paper` (recorte redondo).
void faceDrawAvatar(LovyanGFX& g, int x, int y, int size, uint16_t paper,
                    uint16_t line = FACE_LINE, bool round = true);
// Mistura linear entre fundo (lum=255, papel branco) e tinta (lum=0).
uint16_t faceTint(uint8_t lum, uint16_t ink, uint16_t bg);
