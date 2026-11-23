// AI Monitor v0.2 — telas 320x240 paisagem, desenhadas no quadro em
// memória (gfx.h). As funções recebem o toque do quadro (touched, tx, ty)
// e devolvem a ação; o main.cpp decide a troca de tela.
#pragma once

#include <Arduino.h>
#include "model.h"
#include "face.h"

enum Screen {
  SCR_LIST = 0,
  SCR_DETAIL,
  SCR_APPROVAL,
  SCR_FACE,      // rosto da Nous-Girl em tela cheia (ocioso)
  SCR_STATS,     // tela COMPUTADOR (CPU/RAM/GPU/rede/processo/bateria do PC)
  SCR_SETTINGS   // tela CONFIGURACOES (Wi-Fi, Hub, trocar/esquecer rede)
};

// Geometria compartilhada.
static const int HDR_H = 34;
static const int LIM_Y = 36;
static const int LIST_TOP = 54;
static const int CARD_H = 46;
static const int CARD_STEP = 50;
static const int PER_PAGE = 3;
static const int SIDE_W = 38;     // barra lateral (lista / configurações)
static const int CARD_X = 42;     // início do conteúdo à direita da barra
static const int CARD_W = 272;
static const int FOOTER_Y = 205;  // rodapé com as pílulas "Hub" e "CPU/RAM"

// Humor atual e sua tinta (o rosto e o avatar acompanham o estado).
FaceMood uiMoodFor(const AppState& st, uint32_t now, uint16_t& ink);

void uiDrawBackground();
void uiDrawHeader(const AppState& st, bool blink, bool limits = true);
void uiDrawFooter(const AppState& st);
// Toque no rodapé: 1 = pílula do Hub, 2 = pílula CPU/RAM (abre Computador), 0 = nada.
int uiFooterHit(int tx, int ty);
// Lista paginada com barra lateral e paginador no cabeçalho. Retorna o índice
// tocado, -2 próxima página, -3 página anterior, -4 botão de configurações, -1 nada.
int uiDrawList(AppState& st, bool touched, int tx, int ty);
// Detalhe. 0 = nada, 1 = voltar, 2 = aprovar 1x, 4 = negar.
int uiDrawDetail(AppState& st, bool touched, int tx, int ty);
// Aprovação (âmbar). 0 = nada, 2 = aprovar 1x, 4 = negar, 5 = ver depois.
int uiDrawApproval(AppState& st, bool touched, int tx, int ty);
// Tela COMPUTADOR. 1 = voltar.
int uiDrawStats(AppState& st, bool touched, int tx, int ty);
void uiDrawPinBadge();  // selo "FIXADA" na tela Computador
// Configurações. 0 = nada, 1 = voltar, 2 = trocar rede, 3 = esquecer Wi-Fi,
// 4 = volume -, 5 = volume +.
int uiDrawSettings(AppState& st, bool touched, int tx, int ty);
// Sobreposições da tela do rosto (relógio, estado). O GIF já está em G().
void uiDrawFaceOverlay(const AppState& st, uint32_t now);
// Tela de boot (desenha e apresenta).
void uiBootScreen(const char* line);
void uiDrawToast(const String& msg);

String uiClockStr();
bool uiReadTouch(int& x, int& y);
