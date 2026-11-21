// AI Monitor v0.2 — quadro em memoria (sem cintilacao).
// Tudo e desenhado num M5Canvas de 320x240 na PSRAM e enviado ao painel
// uma vez por quadro. Se a PSRAM faltar, cai para o desenho direto.
#pragma once
#include <M5Unified.h>

bool gfxBegin();
LovyanGFX& G();
void gfxPresent();
bool gfxBuffered();
