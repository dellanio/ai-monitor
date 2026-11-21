// AI Monitor v0.1 — Wi-Fi: portal de configuração, conexão e mDNS.
// Primeira inicialização (sem credenciais salvas): sobe um Access Point
// "AIMonitor-Setup" com portal captivo. A tela do aparelho mostra os
// passos, o nome do AP, a URL e um QR code, com status ao vivo. O portal
// lista as redes encontradas por scan (com sinal) e testa a conexão
// antes de reiniciar. Depois conecta e anuncia dellmonitor.local via mDNS.
#pragma once

#include <Arduino.h>
#include "model.h"

// Carrega credenciais salvas. Retorna true se há SSID salvo.
bool wifiLoadSaved(AppState& st);
// Salva SSID/senha/Hub nas Preferences.
void wifiSave(const String& ssid, const String& pass, const String& hubHost,
              uint16_t hubPort, const String& hubToken);
// Apaga as credenciais salvas (volta ao portal no próximo boot).
void wifiForget();
// Tenta conectar com as credenciais salvas (timeout ~20 s).
bool wifiConnect(AppState& st);
// Sobe o AP + portal captivo. Só retorna quando o usuário salva
// (então o chamador reinicia o aparelho).
void wifiStartSetupPortal();
// Redesenha só a faixa de status da tela de setup (ao vivo).
void wifiSetupSetStatus(const String& msg, uint16_t color);
// Serviço do portal (chamar em loop enquanto o portal estiver ativo).
void wifiPortalLoop();
// Inicia o mDNS (dellmonitor.local). Chamar após conectar.
void wifiStartMDNS();
