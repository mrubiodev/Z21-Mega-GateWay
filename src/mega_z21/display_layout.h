/*
 * display_layout.h
 * ----------------
 * Calcula la geometría de las zonas de la pantalla (cabecera de estado,
 * línea divisoria, panel de log) a partir de las dimensiones reales ya
 * rotadas del shield. Ningún otro fichero del módulo debe calcular
 * coordenadas de zonas "a mano" — todos consumen un DisplayLayout ya
 * resuelto, para que cambiar el reparto de espacio (HEADER_HEIGHT_RATIO
 * en display_theme.h) no obligue a tocar el status panel ni el log.
 *
 * Historial de cambios:
 *   - v0.1 (2026-08-02): Creación inicial. Reparto vertical en dos zonas
 *     (cabecera + log), pensado para apaisado.
 */

#ifndef DISPLAY_LAYOUT_H
#define DISPLAY_LAYOUT_H

#include <Arduino.h>

// Rectángulo simple en coordenadas de pantalla (origen arriba-izquierda,
// mismo sistema que Adafruit_GFX).
struct DisplayRect {
  int16_t x, y;
  uint16_t w, h;
};

// Geometría completa resuelta para la resolución/rotación actuales.
struct DisplayLayout {
  DisplayRect header;  // zona superior: estado del sistema
  DisplayRect divider; // línea horizontal de separación (h = 1..2 px)
  DisplayRect log;     // zona inferior: log de comunicación
  uint16_t screenW, screenH;
};

// Rellena `out` a partir de displayWidth()/displayHeight() (ver
// display_driver.h) y de las proporciones definidas en display_theme.h.
// Debe llamarse después de displayDriverInit().
void displayLayoutCompute(DisplayLayout &out);

#endif // DISPLAY_LAYOUT_H
