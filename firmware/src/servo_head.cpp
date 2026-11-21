// AI Monitor v0.1 — aceno da cabeça do Stack-chan.
#include "servo_head.h"
#include "config.h"

#include <ESP32Servo.h>

static Servo yawServo;
static Servo pitchServo;
static bool enabled = false;

void servoBegin() {
#if SERVO_ENABLED
  if (SERVO_YAW_PIN < 0 && SERVO_PITCH_PIN < 0) {
    Serial.println("[servo] pinos nao configurados (CONFIRMAR K151)");
    enabled = false;
    return;
  }
  if (SERVO_YAW_PIN >= 0) {
    yawServo.setPeriodHertz(50);
    yawServo.attach(SERVO_YAW_PIN, 500, 2400);
    yawServo.write(90);
  }
  if (SERVO_PITCH_PIN >= 0) {
    pitchServo.setPeriodHertz(50);
    pitchServo.attach(SERVO_PITCH_PIN, 500, 2400);
    pitchServo.write(90);
  }
  enabled = true;
  Serial.println("[servo] servos inicializados");
#else
  // Servos desativados por padrão (config.h: SERVO_ENABLED 0).
  // Configure os pinos e ligue SERVO_ENABLED após confirmar a pinagem.
  enabled = false;
#endif
}

bool servoIsEnabled() { return enabled; }

void servoNod() {
  if (!enabled) return;
  if (SERVO_YAW_PIN >= 0) {
    yawServo.write(65);
    delay(180);
    yawServo.write(115);
    delay(180);
    yawServo.write(90);
  }
}
