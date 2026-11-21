// AI Monitor v0.1 — aceno da cabeça do Stack-chan.
// Os pinos dos servos são constantes em config.h e vêm DESATIVADOS (-1)
// até que a pinagem do Stack-chan K151 seja confirmada na documentação
// oficial do M5Stack. Com -1, servoNod() não faz nada (seguro).
#pragma once

#include <Arduino.h>

// Inicializa os servos (só se os pinos estiverem configurados).
void servoBegin();
// Aceno: balança a cabeça (yaw) duas vezes. Não faz nada se desativado.
void servoNod();
// Retorna true quando os servos estão configurados e ativos.
bool servoIsEnabled();
