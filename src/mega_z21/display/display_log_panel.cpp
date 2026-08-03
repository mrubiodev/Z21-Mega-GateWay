/*
 * display_log_panel.cpp
 * ------------------------
 * Ver display_log_panel.h para el contrato del módulo.
 *
 * Historial de cambios:
 *   - v0.1 (2026-08-02): Creación inicial.
 */

#include "display_log_panel.h"
#include "display_driver.h"
#include "display_theme.h"
#include <string.h>
#include <stdio.h>

// Buffer circular en RAM estática (nada de String/heap dinámico, ver
// justificación en display_log_panel.h).
static char g_logLines[LOG_MAX_LINES][LOG_LINE_MAXLEN + 1];
static uint8_t g_logCount = 0; // líneas válidas hasta ahora (crece hasta LOG_MAX_LINES)
static uint8_t g_logHead = 0;  // índice circular de la PRÓXIMA línea a escribir

static DisplayRect g_logRect;
static uint8_t g_charsPerLine = 0;
static uint8_t g_lineHeightPx = 0;
static uint8_t g_maxVisibleLines = 0;
static int16_t g_textAreaY0 = 0;
static int16_t g_textAreaH = 0;

static void drawFrame() {
  tft.fillRect(g_logRect.x, g_logRect.y, g_logRect.w, g_logRect.h, COLOR_LOG_BG);
  tft.drawRect(g_logRect.x, g_logRect.y, g_logRect.w, g_logRect.h, COLOR_DIVIDER);

  tft.setTextSize(LOG_TEXT_SIZE);
  tft.setTextColor(COLOR_LOG_TITLE);
  tft.setCursor(g_logRect.x + 4, g_logRect.y + 2);
  tft.print(F("LOG DE COMUNICACION"));
}

// Repinta ÚNICAMENTE la zona de texto del panel (por debajo del título),
// no el marco ni el título — evita parpadeo del borde en cada línea nueva.
static void redrawLines() {
  tft.fillRect(g_logRect.x + 2, g_textAreaY0, g_logRect.w - 4, g_textAreaH, COLOR_LOG_BG);

  tft.setTextSize(LOG_TEXT_SIZE);
  tft.setTextColor(COLOR_LOG_TEXT);

  uint8_t linesToShow = (g_logCount < g_maxVisibleLines) ? g_logCount : g_maxVisibleLines;

  // Se muestran las últimas `linesToShow` líneas en orden cronológico:
  // la más antigua de las visibles arriba, la más reciente abajo (como
  // cualquier consola/log habitual).
  for (uint8_t i = 0; i < linesToShow; i++) {
    uint8_t idxFromNewest = linesToShow - 1 - i;
    uint8_t bufIdx = (uint8_t)((g_logHead + LOG_MAX_LINES - 1 - idxFromNewest) % LOG_MAX_LINES);
    tft.setCursor(g_logRect.x + 4, g_textAreaY0 + i * g_lineHeightPx);
    tft.print(g_logLines[bufIdx]);
  }
}

void displayLogPanelInit(const DisplayLayout &layout) {
  g_logRect = layout.log;

  // Caracteres por línea según el ancho REAL detectado del shield (6 px
  // por carácter y tamaño de texto, ver display_theme.h), acotado además
  // por LOG_LINE_MAXLEN.
  uint8_t charsFit = (uint8_t)((g_logRect.w - 8) / (6 * LOG_TEXT_SIZE));
  g_charsPerLine = (charsFit < LOG_LINE_MAXLEN) ? charsFit : LOG_LINE_MAXLEN;

  g_lineHeightPx = (uint8_t)((8 * LOG_TEXT_SIZE) + 2);
  g_textAreaY0 = g_logRect.y + 2 + (LOG_TEXT_SIZE * 8) + 2; // debajo del título
  g_textAreaH = g_logRect.h - (g_textAreaY0 - g_logRect.y) - 2;

  uint8_t visibleFit = (g_lineHeightPx > 0) ? (uint8_t)(g_textAreaH / g_lineHeightPx) : 0;
  g_maxVisibleLines = (visibleFit < LOG_MAX_LINES) ? visibleFit : LOG_MAX_LINES;

  g_logCount = 0;
  g_logHead = 0;

  drawFrame();
}

void displayLogPanelPrintln(const char *line) {
  char *slot = g_logLines[g_logHead];

  // Prefijo de marca de tiempo relativa al arranque (segundos), para
  // poder correlacionar lo que se ve en pantalla con DEBUG_SERIAL si hay
  // que depurar con el USB conectado (ver AGENT.md, "Depuración por USB").
  unsigned long seconds = millis() / 1000UL;
  int prefixLen = snprintf(slot, LOG_LINE_MAXLEN + 1, "[%lus] ", seconds);
  if (prefixLen < 0) prefixLen = 0;
  if ((uint8_t)prefixLen > LOG_LINE_MAXLEN) prefixLen = LOG_LINE_MAXLEN;

  if ((uint8_t)prefixLen < LOG_LINE_MAXLEN) {
    strncpy(slot + prefixLen, line, LOG_LINE_MAXLEN - prefixLen);
  }
  slot[LOG_LINE_MAXLEN] = '\0';

  // Truncado adicional al ancho real de este shield concreto (puede ser
  // menor que LOG_LINE_MAXLEN si la pantalla es más estrecha de lo
  // esperado).
  if (g_charsPerLine > 0 && g_charsPerLine < LOG_LINE_MAXLEN) {
    slot[g_charsPerLine] = '\0';
  }

  g_logHead = (uint8_t)((g_logHead + 1) % LOG_MAX_LINES);
  if (g_logCount < LOG_MAX_LINES) g_logCount++;

  redrawLines();
}

void displayLogPanelPrintlnF(const __FlashStringHelper *line) {
  char buf[LOG_LINE_MAXLEN + 1];
  strncpy_P(buf, (PGM_P)line, LOG_LINE_MAXLEN);
  buf[LOG_LINE_MAXLEN] = '\0';
  displayLogPanelPrintln(buf);
}

void displayLogPanelPrintf(const char *fmt, ...) {
  char buf[LOG_LINE_MAXLEN + 1];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  displayLogPanelPrintln(buf);
}
