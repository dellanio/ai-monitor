// Camada de hardware: o resto do firmware usa DISP (tela), M5.Touch, M5.Speaker e M5.Mic.
//  - M5Stack CoreS3 / StackChan (padrão): tudo vem do M5Unified (M5.begin detecta a placa).
//  - LCDWiki ES3C28P (-DBOARD_ES3C28P): ESP32-S3 2.8" ILI9341V IPS + toque FT6336G +
//    codec ES8311 com amplificador FM8002E. O M5Unified não conhece a placa, então a tela é
//    um LGFX próprio e o som usa as classes Speaker/Mic do M5Unified com os pinos daqui.
#pragma once
#include <M5Unified.h>

#if defined(BOARD_ES3C28P)
lgfx::LGFX_Device& hwDisplay();
#define DISP (hwDisplay())
#define BOARD_NAME "ES3C28P"
#else
#define DISP (M5.Display)
#define BOARD_NAME "CoreS3"
#endif

void hwBegin();   // no lugar de M5.begin()
void hwUpdate();  // no lugar de M5.update()
const char* hwInfo();  // placa + periféricos detectados (vai no log [diag])
// Toque. Na ES3C28P uma tarefa lê o FT6336G a cada 8 ms e guarda cada toque
// novo, então nenhum toque se perde mesmo com o loop lento (lista cheia, áudio).
bool hwTouchTap(int& x, int& y);   // toque novo desde a última chamada
bool hwTouchHeld(int& x, int& y);  // dedo na tela agora
