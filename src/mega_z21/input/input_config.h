/*
 * input_config.h
 * ---------------
 * Configuración centralizada de las entradas físicas del Mega que
 * todavía no están montadas en el hardware actual: el botón/seta de
 * parada de emergencia (ver AGENT.md, sección "Seguridad" — no
 * negociable) y el encoder rotativo con pulsador (ver
 * docs/Z21_EMULATOR_SPEC.md sección 9).
 *
 * MISMO PATRÓN que traction_config.h (TRACTION_BACKEND_SELECTED): cada
 * pieza de hardware tiene su propio flag "¿está físicamente conectada?"
 * (EMERGENCY_STOP_HARDWARE_PRESENT / ENCODER_HARDWARE_PRESENT), en 0 por
 * defecto. El código de lectura/interrupción está completo y compila
 * siempre, pero solo se ACTIVA de verdad (pinMode + attachInterrupt) si
 * el flag está en 1 — así el firmware se puede flashear y probar hoy
 * mismo sin el botón ni el encoder conectados, sin arriesgarse a un pin
 * flotante disparando una parada de emergencia falsa en cada arranque
 * (ver el razonamiento detallado en el propio flag, más abajo).
 *
 * Cuando llegue cada pieza de hardware: revisar el pin, cablear según el
 * comentario de cada sección, cambiar el flag correspondiente a 1, y
 * recompilar. No hace falta tocar nada más.
 *
 * Historial de cambios:
 *   - v0.1 (2026-08-04): Creación inicial.
 */

#ifndef INPUT_CONFIG_H
#define INPUT_CONFIG_H

// ---------------------------------------------------------------------
// PARADA DE EMERGENCIA (seta / botón físico)
// ---------------------------------------------------------------------
//
// AGENT.md es explícito: "el botón de parada de emergencia debe seguir
// siendo una interrupción hardware que dispara LAN_X_SET_STOP de forma
// inmediata, sin pasar por el bucle de menú ni por ninguna cola de
// eventos que pueda bloquearse". La implementación (ver mega_z21.ino,
// estopISR()/estopCheckAndHandle()) cumple esto así:
//   - La ISR (estopISR) hace lo MÍNIMO posible: solo pone a true un
//     flag `volatile`. Nada de Serial, nada de dibujar en pantalla, nada
//     que pueda tardar o bloquear dentro de una interrupción.
//   - loop() comprueba ese flag como la PRIMERA cosa que hace en cada
//     vuelta (justo después de wdt_reset()), antes de tocar el enlace
//     con el ESP o la pantalla. loop() en este firmware da vueltas en
//     microsegundos, así que en la práctica la parada se dispara casi
//     tan rápido como si se llamara directamente desde la ISR, pero sin
//     los riesgos de hacer Serial3.write()/SPI de la pantalla dentro de
//     un contexto de interrupción (podría corromper una transacción SPI
//     en curso con el shield TFT, por ejemplo).
//   - Esto NO es "una cola de eventos que puede bloquearse": es un único
//     flag booleano comprobado de inmediato, no una cola FIFO de tareas
//     pendientes con posibilidad de acumularse o de esperar su turno.
//
// CABLEADO PREVISTO — contacto NC (normalmente cerrado), fail-safe:
// la mayoría de setas de emergencia reales llevan un contacto NC, no NO.
// Con NC: en reposo el circuito está cerrado (continuidad), y al pulsar
// la seta SE ABRE el circuito. Cableado así (un terminal a GND, el otro
// a EMERGENCY_STOP_PIN con INPUT_PULLUP):
//   - Reposo (seta sin pulsar, contacto cerrado): pin a GND -> LOW.
//   - Pulsada (contacto abierto): pull-up interno gana -> HIGH.
//   - Cable cortado o seta desconectada: circuito también abierto -> HIGH.
// Es decir, CUALQUIER fallo del cableado (pulsar la seta o que se
// desconecte/corte el cable) da el mismo resultado: HIGH -> parada. Por
// eso se dispara en flanco de subida (RISING). Si el modelo de seta que
// se acabe comprando es NO en vez de NC, cambiar aquí el sentido
// (RISING->FALLING en el attachInterrupt de mega_z21.ino) y documentarlo.
#define EMERGENCY_STOP_HARDWARE_PRESENT 0 // 0 = seta todavía no conectada (poner a 1 al montarla)
#define EMERGENCY_STOP_PIN 2              // INT0 en el Mega2560 — pin dedicado, no compartido con el encoder
#define EMERGENCY_STOP_DEBOUNCE_MS 250    // ignora nuevos disparos de la ISR durante este tiempo tras uno real

// ---------------------------------------------------------------------
// ENCODER ROTATIVO CON PULSADOR
// ---------------------------------------------------------------------
//
// Pensado para la librería "Encoder" de PJRC (Paul Stoffregen), la misma
// que menciona docs/Z21_EMULATOR_SPEC.md sección 9. NO se vendoriza en
// este repo (librería de terceros, igual que XpressNet) — instalar desde
// Arduino IDE -> Gestionar bibliotecas -> "Encoder" antes de compilar con
// ENCODER_HARDWARE_PRESENT en 1. Con el flag en 0, encoder_input.cpp no
// incluye <Encoder.h> en absoluto, así que el sketch compila igual sin
// tener la librería instalada (mismo patrón que traction_backend_xpressnet.h
// con <XpressNet.h>).
//
// PINES: los dos canales del encoder (A/B) van en pines con interrupción
// externa (INT en el Mega2560: 2, 3, 18, 19, 20, 21) para que la
// librería Encoder decodifique la cuadratura sin perder pasos. El pin 2
// (INT0) ya lo usa la parada de emergencia — dedicado, no se toca. Los
// pines 18/19 (INT5/INT4) están físicamente ocupados por Serial1
// (RS485/XpressNet, ver traction_backend_xpressnet.h) — la librería
// XpressNet los reserva como UART, no están libres como GPIO aunque
// tengan capacidad de interrupción. Quedan libres 3, 20 y 21: se usan A=3
// (INT1) y B=21 (INT2). El pulsador del encoder NO necesita interrupción
// (no es crítico en latencia como la parada de emergencia): pin normal
// con antirrebote por software en encoder_input.cpp.
//
// Pin 10 queda reservado a propósito para el CS de la SD del shield TFT
// (ver AGENT.md, sección "Pantalla") cuando se implemente — no
// asignarlo aquí a otra cosa aunque esté libre ahora mismo.
#define ENCODER_HARDWARE_PRESENT 0    // 0 = encoder todavía no montado (poner a 1 al montarlo)
#define ENCODER_PIN_A 3               // INT1
#define ENCODER_PIN_B 21              // INT2
#define ENCODER_BUTTON_PIN 20         // pulsador del encoder, sin interrupción, con antirrebote SW
#define ENCODER_BUTTON_DEBOUNCE_MS 30 // antirrebote típico de pulsador mecánico (no es una seta de seguridad)

#endif // INPUT_CONFIG_H
