/*
 * display_types.h
 * ---------------
 * Tipos de datos compartidos por todos los ficheros del módulo de
 * pantalla (display_*). No contiene lógica, solo definiciones — así
 * cualquier otro fichero (incluido mega_z21.ino) puede incluir SOLO este
 * header sin arrastrar dependencias de MCUFRIEND_kbv si únicamente
 * necesita construir o leer un snapshot.
 *
 * DECISIÓN DE DISEÑO: el módulo de pantalla nunca lee variables globales
 * de mega_z21.ino directamente. mega_z21.ino construye un
 * DisplayStatusSnapshot cada vuelta de loop() con los datos que ya tiene
 * (sync, red, contadores...) y se lo pasa a displayTick(). Así la
 * pantalla es un módulo "de salida pura": no conoce nada del protocolo
 * Z21 ni del framing con el ESP, solo pinta la foto que le llega. Esto
 * facilita poder testear/depurar cada módulo por separado y evita que un
 * cambio en el core del protocolo obligue a tocar el código de la
 * pantalla.
 *
 * Historial de cambios:
 *   - v0.1 (2026-08-02): Creación inicial del snapshot de estado y del
 *     enum de modos de pantalla (solo SCREEN_MODE_DIAG implementado).
 *   - v0.2 (2026-08-03): añadido netInfoMac (puntero a NET_INFO_MAC_LEN
 *     bytes) para poder pintar la MAC efectiva junto al modo/IP en la
 *     fila de red (ver display_status_panel.cpp) -- antes solo se
 *     registraba en el log de comunicación.
 */

#ifndef DISPLAY_TYPES_H
#define DISPLAY_TYPES_H

#include <Arduino.h>

// Foto del estado del firmware en un instante dado, tal y como la necesita
// pintar la pantalla. mega_z21.ino la rellena cada vuelta de loop() a
// partir de sus propias variables (ver buildDisplaySnapshot() en
// mega_z21.ino) y se la pasa a displayTick().
struct DisplayStatusSnapshot {
  // Identidad / firmware (ver MEGA_FW_VERSION_* en z21_protocol.h)
  uint8_t fwVersionMajor;
  uint8_t fwVersionMinor;
  unsigned long uptimeMs;

  // Estado de sincronización con el ESP8266 (ver AGENT.md, sección
  // "Sincronización inicial Mega<->ESP")
  bool synced;
  bool syncDegraded;

  // Info de red tal como la reporta el ESP en FRAME_TYPE_NET_INFO.
  // netInfoMode == 0xFF significa "todavía no ha llegado ningún NET_INFO".
  uint8_t netInfoMode;
  uint32_t netInfoIp;
  const uint8_t *netInfoMac; // puntero a NET_INFO_MAC_LEN bytes en el buffer
                              // que posee mega_z21.ino; igual que netInfoSsid,
                              // el módulo de pantalla solo lo lee durante la
                              // llamada, no se queda con el puntero
  const char *netInfoSsid; // puntero al buffer que posee mega_z21.ino; el
                            // módulo de pantalla NO se queda con el puntero
                            // más allá de la llamada, solo lo lee para pintar

  // Estado global de salud del firmware (ver STATUS_* en z21_protocol.h)
  uint8_t statusCode;
  bool watchdogRecoveredAtBoot;

  // Contadores de diagnóstico del enlace Mega<->ESP (ver AGENT.md,
  // sección "Diagnóstico y watchdog")
  uint8_t framesRxOk;
  uint8_t framesRxBad;
  uint8_t framesRxChkFail;
  uint16_t freeRamBytes;
};

// Modos de pantalla. Con el hardware actual NO hay encoder ni botones
// (aparte del futuro botón dedicado de parada de emergencia, que es una
// interrupción hardware y no pasa por aquí, ver AGENT.md sección
// "Seguridad"), así que de momento solo existe SCREEN_MODE_DIAG: una
// única vista fija con el estado del sistema y el log de comunicación,
// pensada para poder evaluar a simple vista si el gateway está listo y
// qué está pasando en el enlace, sin depender del puerto serie de debug.
//
// Los modos siguientes quedan reservados para cuando se monte el encoder
// rotativo (ver docs/Z21_EMULATOR_SPEC.md sección 9) y tenga sentido
// navegar entre pantallas — no están implementados todavía y no deben
// usarse.
enum DisplayScreenMode {
  SCREEN_MODE_DIAG = 0,   // estado del sistema + log de comunicación (única activa)
  SCREEN_MODE_LOCO = 1,   // TODO futuro: control de locomotoras (requiere encoder)
  SCREEN_MODE_CONFIG = 2, // TODO futuro: configuración local (requiere encoder)
};

#endif // DISPLAY_TYPES_H
