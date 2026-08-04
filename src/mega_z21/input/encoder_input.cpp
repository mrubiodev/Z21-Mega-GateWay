/*
 * encoder_input.cpp
 * -------------------
 * Ver encoder_input.h para el contrato del módulo y input_config.h para
 * pines/flags.
 *
 * Historial de cambios:
 *   - v0.1 (2026-08-04): Creación inicial.
 */

#include "encoder_input.h"
#include "input_config.h"

#if ENCODER_HARDWARE_PRESENT

// <Encoder.h> (PJRC) SOLO se incluye cuando el hardware está marcado
// como presente — igual que <XpressNet.h> en traction_backend_xpressnet.h,
// para no forzar la dependencia de esta librería externa cuando no hace
// falta. Instalar desde el Gestor de Bibliotecas antes de compilar con
// ENCODER_HARDWARE_PRESENT en 1 (ver input_config.h).
#include <Encoder.h>

static Encoder encoder(ENCODER_PIN_A, ENCODER_PIN_B);
static long lastEncoderRaw = 0;

// Antirrebote por software del pulsador (no crítico en latencia, a
// diferencia de la parada de emergencia — por eso no lleva interrupción
// ni flag volatile, un simple sondeo con millis() en encoderPoll() es
// suficiente).
static bool lastButtonRawState = HIGH; // HIGH = suelto, con INPUT_PULLUP
static bool debouncedButtonState = HIGH;
static unsigned long lastButtonEdgeMs = 0;

void encoderInit() {
  pinMode(ENCODER_BUTTON_PIN, INPUT_PULLUP);
  lastEncoderRaw = encoder.read();
  lastButtonRawState = digitalRead(ENCODER_BUTTON_PIN);
  debouncedButtonState = lastButtonRawState;
}

bool encoderPoll(long &delta, bool &buttonPressed) {
  delta = 0;
  buttonPressed = false;
  bool changed = false;

  // La librería Encoder cuenta en "cuartos de paso" (4 pulsos por clic
  // mecánico en la mayoría de encoders baratos tipo EC11) — se divide
  // entre 4 para que delta=1 sea "un clic", que es lo que espera
  // cualquier futuro menú (SCREEN_MODE_LOCO/CONFIG).
  long raw = encoder.read();
  long rawDelta = raw - lastEncoderRaw;
  if (rawDelta >= 4 || rawDelta <= -4) {
    delta = rawDelta / 4;
    lastEncoderRaw += delta * 4;
    changed = true;
  }

  // Antirrebote simple por tiempo: un cambio de estado solo se acepta
  // como "de verdad" si el pin lleva estable ENCODER_BUTTON_DEBOUNCE_MS
  // sin volver a cambiar. buttonPressed se reporta en el flanco de
  // bajada ya debounced (pulsador a GND con INPUT_PULLUP: LOW = pulsado).
  bool rawState = digitalRead(ENCODER_BUTTON_PIN);
  unsigned long now = millis();
  if (rawState != lastButtonRawState) {
    lastButtonRawState = rawState;
    lastButtonEdgeMs = now;
  } else if ((now - lastButtonEdgeMs) >= ENCODER_BUTTON_DEBOUNCE_MS &&
             debouncedButtonState != rawState) {
    debouncedButtonState = rawState;
    if (debouncedButtonState == LOW) { // flanco de pulsación completo
      buttonPressed = true;
      changed = true;
    }
  }

  return changed;
}

#else // !ENCODER_HARDWARE_PRESENT

// Encoder todavía no montado: versión "vacía" que no toca ningún pin ni
// depende de <Encoder.h>, para que el sketch compile igual sin la
// librería instalada (ver comentario de cabecera en encoder_input.h).
void encoderInit() {
  // Nada que hacer todavía.
}

bool encoderPoll(long &delta, bool &buttonPressed) {
  delta = 0;
  buttonPressed = false;
  return false;
}

#endif // ENCODER_HARDWARE_PRESENT
