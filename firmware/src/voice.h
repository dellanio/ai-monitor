// AI Monitor — voz: microfone -> Hub (que ouve o "Hey Hermes" e fala com o canal de voz da
// assinatura) e fala do Hub -> alto-falante. Roda numa tarefa própria no núcleo 0 para o
// microfone nunca perder áudio enquanto o loop principal desenha a tela.
//
// No CoreS3 o microfone e o alto-falante dividem o I2S: um de cada vez. Todos os sons do
// aparelho (avisos do monitor inclusive) passam por aqui, via voiceTone().
#pragma once
#include <Arduino.h>

enum VoiceState : uint8_t {
  VOICE_OFF = 0,    // sem Hub de voz configurado/conectado
  VOICE_IDLE,       // esperando "Hey Hermes"
  VOICE_LISTENING,  // ouviu a palavra de ativação; fale
  VOICE_THINKING,   // o Hermes está pensando / fazendo
  VOICE_SPEAKING,   // tocando a resposta
  VOICE_ERROR,
};

// host/token = os mesmos do Hub do monitor; port = porta do /voice (VOICE_DEFAULT_PORT).
void voiceBegin(const String& host, uint16_t port, const String& token);
VoiceState voiceState();
bool voiceConnected();
// Avisos do monitor: entram numa fila e tocam em ordem (gap_ms de silêncio depois).
void voiceTone(uint16_t freq, uint16_t ms, uint16_t gap_ms = 10);
// Aviso com som do Hub ("needyou", "finished", "new"): arquivo escolhido no notebook.
// false = o Hub não tem esse som, está desconectado ou numa conversa -> use voiceTone().
bool voiceSound(const char* name);
// true enquanto houver tom na fila ou fala tocando (para o monitor não empilhar avisos).
bool voiceSoundBusy();
// Toque durante a fala: para de falar (manda "stop" ao Hub).
void voiceStopSpeaking();
// Contador que sobe a cada "Hey Hermes" (o main troca para o rosto ouvindo).
uint32_t voiceWakeCount();
