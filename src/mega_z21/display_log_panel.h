/*
 * display_log_panel.h
 * ---------------------
 * Panel inferior de la pantalla: un log de texto en buffer circular con
 * las últimas líneas de eventos relevantes (sincronización con el ESP,
 * cambios de red, parada de emergencia, tramas inválidas...). Pensado
 * para poder evaluar a simple vista, sin cable USB ni monitor serie, si
 * el sistema está comunicando y qué está pasando exactamente (ver
 * petición de usuario: "facilitar evaluar si el sistema está listo y
 * comunicando").
 *
 * No usa la clase String de Arduino en ningún punto: todo son buffers
 * `char[]` de tamaño fijo, para no fragmentar el heap de un AVR con muy
 * poca RAM en un firmware pensado para correr indefinidamente.
 *
 * Historial de cambios:
 *   - v0.1 (2026-08-02): Creación inicial. Buffer circular fijo +
 *     variantes de impresión con cadena en RAM, en flash (F()) y con
 *     formato (printf-like).
 */

#ifndef DISPLAY_LOG_PANEL_H
#define DISPLAY_LOG_PANEL_H

#include "display_layout.h"
#include <Arduino.h>
#include <stdarg.h>

// Pinta el marco y título fijo del panel y prepara el buffer circular.
// Se llama una única vez desde displayInit().
void displayLogPanelInit(const DisplayLayout &layout);

// Añade una línea al buffer circular (con marca de tiempo relativa al
// arranque, en segundos) y repinta solo la zona de texto del panel de
// log (no el marco ni el resto de la pantalla).
void displayLogPanelPrintln(const char *line);

// Variante para cadenas guardadas en flash con F("..."), evita copiar
// literales de texto estático a la RAM del AVR.
void displayLogPanelPrintlnF(const __FlashStringHelper *line);

// Variante con formato tipo printf, para líneas que combinan texto fijo
// con valores en tiempo de ejecución (direcciones IP, contadores...).
// El propio formato SÍ ocupa RAM si se pasa como literal normal; si el
// formato es fijo, preferir construir la línea a mano y usar
// displayLogPanelPrintln(), o usar printf con F() a través de un buffer
// intermedio en las llamadas más frecuentes.
void displayLogPanelPrintf(const char *fmt, ...);

#endif // DISPLAY_LOG_PANEL_H
