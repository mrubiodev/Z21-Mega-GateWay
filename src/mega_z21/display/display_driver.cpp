/*
 * display_driver.cpp
 * -------------------
 * Ver display_driver.h para el contrato del módulo.
 *
 * Historial de cambios:
 *   - v0.1 (2026-08-02): Creación inicial. readID() con fallback para
 *     shields que devuelven un ID no reconocido por la librería.
 */

#include "display_driver.h"
#include "display_theme.h"

MCUFRIEND_kbv tft;

void displayDriverInit() {
  uint16_t id = tft.readID();

  // Algunos shields 3.5" (controladores tipo ILI9481/HX8357 en clones
  // baratos) devuelven un ID que MCUFRIEND_kbv no reconoce (típicamente
  // 0xD3D3 o 0x0000) aunque el panel funcione perfectamente. En ese caso
  // se fuerza un controlador común de 3.5" en vez de dejar la pantalla en
  // negro sin ninguna pista de qué ha pasado. Si el panel se ve con
  // colores/orientación incorrectos tras este fallback, es la primera
  // pista a revisar (probar 0x9486 o 0x9488 en vez de 0x9481).
  if (id == 0xD3D3 || id == 0x0000) {
    id = 0x9481;
  }

  tft.begin(id);
  tft.setRotation(DISPLAY_ROTATION);
  tft.fillScreen(COLOR_BG);
}

uint16_t displayWidth() { return tft.width(); }
uint16_t displayHeight() { return tft.height(); }
