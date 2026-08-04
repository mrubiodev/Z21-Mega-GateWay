/*
 * encoder_input.h
 * ----------------
 * Envoltorio del encoder rotativo + pulsador (ver input_config.h para
 * pines y el porqué de cada uno). API mínima, pensada para que
 * mega_z21.ino la llame cada vuelta de loop() sin preocuparse de si el
 * hardware está montado o no: si ENCODER_HARDWARE_PRESENT es 0 en
 * input_config.h, encoderInit()/encoderPoll() existen igual pero no
 * hacen nada (no dependen de <Encoder.h> ni tocan ningún pin) — así el
 * sketch principal no necesita ningún #if propio para usar el encoder.
 *
 * NO decide qué hacer con los eventos (eso es tarea de la futura
 * SCREEN_MODE_LOCO/CONFIG, ver display_types.h) — de momento
 * mega_z21.ino solo los vuelca al log de pantalla como diagnóstico, para
 * poder confirmar que el cableado funciona antes de construir un menú de
 * verdad encima.
 *
 * Historial de cambios:
 *   - v0.1 (2026-08-04): Creación inicial (sin hardware real todavía,
 *     ver ENCODER_HARDWARE_PRESENT en input_config.h).
 */

#ifndef ENCODER_INPUT_H
#define ENCODER_INPUT_H

#include <Arduino.h>

// Llamar una vez desde setup(). Si ENCODER_HARDWARE_PRESENT es 0, no
// hace nada (ni pinMode).
void encoderInit();

// Llamar cada vuelta de loop(). No bloqueante.
// Devuelve true si hay algo nuevo que reportar (giro y/o pulsación).
//   - delta: pasos girados desde la última llamada (positivo = sentido
//     horario, negativo = antihorario; 0 si no ha girado). Usa el
//     "cuarto de paso" nativo de la librería Encoder dividido entre 4,
//     para que un "clic" mecánico del encoder equivalga a delta=1/-1.
//   - buttonPressed: true si ha habido un flanco de pulsación completo
//     (con antirrebote ya aplicado) desde la última llamada.
// Si ENCODER_HARDWARE_PRESENT es 0, siempre devuelve false y deja
// delta=0 / buttonPressed=false sin leer ningún pin.
bool encoderPoll(long &delta, bool &buttonPressed);

#endif // ENCODER_INPUT_H
