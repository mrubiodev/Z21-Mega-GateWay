/*
 * display_manager.cpp
 * ----------------------
 * Ver display_manager.h para el contrato del módulo.
 *
 * Historial de cambios:
 *   - v0.1 (2026-08-02): Creación inicial.
 */

#include "display_manager.h"
#include "display_driver.h"
#include "display_layout.h"
#include "display_status_panel.h"
#include "display_log_panel.h"
#include "display_theme.h"
#include <stdarg.h>
#include <stdio.h>

// La cabecera de estado no necesita refrescarse en cada vuelta de loop()
// (los datos que muestra —contadores, uptime, RAM— no cambian tan rápido
// como para justificarlo, y cada refresco cuesta ciclos de SPI/bus
// paralelo). 250 ms es suficientemente responsivo para un panel de
// diagnóstico sin competir por tiempo de CPU con el resto del firmware
// (en particular, con el futuro polling XpressNet — ver AGENT.md).
#define STATUS_REFRESH_INTERVAL_MS 250

static DisplayLayout g_layout;
static unsigned long g_lastStatusRefreshMs = 0;

void displayInit() {
  displayDriverInit();
  displayLayoutCompute(g_layout);

  // Línea divisoria entre cabecera y log. Se pinta aquí (y no dentro de
  // ningún submódulo) porque es la única pieza visual que no pertenece
  // ni a la cabecera ni al log, sino a la unión de ambos.
  tft.drawFastHLine(g_layout.divider.x, g_layout.divider.y, g_layout.divider.w, COLOR_DIVIDER);

  displayStatusPanelInit(g_layout);
  displayLogPanelInit(g_layout);

  g_lastStatusRefreshMs = 0;

  displayLog("Pantalla inicializada");
}

void displayTick(const DisplayStatusSnapshot &snap) {
  unsigned long now = millis();
  if (now - g_lastStatusRefreshMs < STATUS_REFRESH_INTERVAL_MS) return;
  g_lastStatusRefreshMs = now;

  displayStatusPanelUpdate(snap);
}

void displayLog(const char *line) {
  displayLogPanelPrintln(line);
}

void displayLogF(const __FlashStringHelper *line) {
  displayLogPanelPrintlnF(line);
}

void displayLogf(const char *fmt, ...) {
  char buf[LOG_LINE_MAXLEN + 1];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  displayLogPanelPrintln(buf);
}
