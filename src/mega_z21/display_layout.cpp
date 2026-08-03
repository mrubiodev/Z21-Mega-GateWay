/*
 * display_layout.cpp
 * -------------------
 * Ver display_layout.h para el contrato del módulo.
 *
 * Historial de cambios:
 *   - v0.1 (2026-08-02): Creación inicial.
 */

#include "display_layout.h"
#include "display_driver.h"
#include "display_theme.h"

void displayLayoutCompute(DisplayLayout &out) {
  out.screenW = displayWidth();
  out.screenH = displayHeight();

  uint16_t headerH = (uint16_t)(out.screenH * HEADER_HEIGHT_RATIO);

  out.header  = { 0, 0, out.screenW, headerH };
  out.divider = { 0, (int16_t)headerH, out.screenW, 2 };
  // El log ocupa todo lo que sobra por debajo de la cabecera + divisor,
  // hasta el borde inferior real de la pantalla (no un valor fijo), para
  // aprovechar shields con más resolución de la mínima esperada.
  out.log = { 0, (int16_t)(headerH + out.divider.h),
              out.screenW,
              (uint16_t)(out.screenH - headerH - out.divider.h) };
}
