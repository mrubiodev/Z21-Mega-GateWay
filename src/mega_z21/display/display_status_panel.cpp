/*
 * display_status_panel.cpp
 * --------------------------
 * Ver display_status_panel.h para el contrato del módulo.
 *
 * DISEÑO: cada fila de estado se trata como una única cadena de texto
 * ("Enlace ESP: SINCRONIZADO"), no como columnas separadas de etiqueta y
 * valor. Se cachea la última cadena pintada en cada fila (buffer estático
 * en RAM, sin String de Arduino) y solo se hace fillRect()+print() de la
 * fila si el contenido ha cambiado. Esto evita el parpadeo de repintar
 * toda la cabecera varias veces por segundo sin necesidad, a cambio de
 * una lógica algo más simple que llevar columnas independientes — para
 * una pantalla de diagnóstico (no de control fino) es el compromiso
 * correcto.
 *
 * Historial de cambios:
 *   - v0.1 (2026-08-02): Creación inicial.
 *   - v0.2 (2026-08-03): fila de red ampliada para mostrar también la MAC
 *     (antes solo modo+IP; el SSID y el gateway siguen sin caber en la
 *     cabecera y se quedan solo en el log). STATUS_ROW_MAXLEN 48->56 para
 *     dar margen de sobra a la línea más larga (modo+IP+MAC).
 */

#include "display_status_panel.h"
#include "display_driver.h"
#include "display_theme.h"
#include "../protocol/z21_protocol.h" // STATUS_*, NET_INFO_MODE_* — únicas constantes de
                                     // protocolo que este módulo necesita, solo para
                                     // decidir qué texto/color mostrar
#include <string.h>
#include <stdio.h>

// Nº de filas totales de la cabecera: 1 de título (estática) + 4 de
// estado (dinámicas, con redibujado selectivo).
#define STATUS_ROW_COUNT 5

static DisplayRect g_headerRect;
static int16_t g_rowH = 0;
static int16_t g_fieldTextX = 0;

// Buffers estáticos con el último texto pintado en cada fila dinámica
// (índices 0..3 = filas 1..4 de la cabecera). Tamaño generoso para no
// truncar nunca una línea de estado real.
#define STATUS_ROW_MAXLEN 56
static char g_lastRowText[4][STATUS_ROW_MAXLEN + 1];
static bool g_forceRedrawAll = true; // true en la primera llamada tras Init

static int16_t rowY(uint8_t rowIndex) {
  return g_headerRect.y + (int16_t)(rowIndex * g_rowH);
}

// Limpia y repinta el contenido de texto de una fila dinámica concreta
// (rowIndex: 0..3, correspondiente a las filas 1..4 de la cabecera).
static void drawDynamicRow(uint8_t rowIndex, const char *text, uint16_t color) {
  int16_t y = rowY(rowIndex + 1); // +1 porque la fila 0 es el título estático
  tft.fillRect(g_headerRect.x, y, g_headerRect.w, g_rowH, COLOR_BG);
  tft.setTextSize(HEADER_FIELD_TEXT_SIZE);
  tft.setTextColor(color);
  tft.setCursor(g_fieldTextX, y + (g_rowH - (8 * HEADER_FIELD_TEXT_SIZE)) / 2);
  tft.print(text);
}

// Traduce STATUS_* (ver z21_protocol.h) a una etiqueta legible en pantalla.
static const char *statusCodeLabel(uint8_t code) {
  switch (code) {
    case STATUS_OK:                 return "OK";
    case STATUS_NO_FRAMES_EVER:     return "SIN FRAMES AUN";
    case STATUS_BAD_FRAMES:         return "FRAMES CORRUPTOS";
    case STATUS_WATCHDOG_RECOVERED: return "REINICIO WATCHDOG";
    case STATUS_SYNCING:            return "SINCRONIZANDO";
    default:                        return "DESCONOCIDO";
  }
}

static uint16_t statusCodeColor(uint8_t code) {
  switch (code) {
    case STATUS_OK:                 return COLOR_VALUE_OK;
    case STATUS_NO_FRAMES_EVER:     return COLOR_VALUE_WARN;
    case STATUS_SYNCING:            return COLOR_VALUE_WARN;
    case STATUS_BAD_FRAMES:         return COLOR_VALUE_BAD;
    case STATUS_WATCHDOG_RECOVERED: return COLOR_VALUE_BAD;
    default:                        return COLOR_VALUE_NEUTRAL;
  }
}

static const char *locoDirectionLabel(bool forward) {
  return forward ? "FWD" : "REV";
}

void displayStatusPanelInit(const DisplayLayout &layout) {
  g_headerRect = layout.header;
  g_rowH = g_headerRect.h / STATUS_ROW_COUNT;
  g_fieldTextX = g_headerRect.x + 4;

  for (uint8_t i = 0; i < 4; i++) g_lastRowText[i][0] = '\0';
  g_forceRedrawAll = true;

  tft.fillRect(g_headerRect.x, g_headerRect.y, g_headerRect.w, g_headerRect.h, COLOR_BG);

  // Fila 0 (título): estática, se pinta una sola vez aquí. La versión de
  // firmware no cambia en caliente, así que no necesita redibujado
  // selectivo como el resto de filas.
  tft.setTextSize(HEADER_TITLE_TEXT_SIZE);
  tft.setTextColor(COLOR_TITLE);
  tft.setCursor(g_fieldTextX, g_headerRect.y + 2);
  tft.print(F("Z21 MEGA GATEWAY"));
}

void displayStatusPanelUpdate(const DisplayStatusSnapshot &snap) {
  char line[STATUS_ROW_MAXLEN + 1];
  uint16_t color;

  // Fila 1: enlace con el ESP (handshake de sincronización)
  if (!snap.synced) {
    snprintf(line, sizeof(line), "Enlace ESP: SINCRONIZANDO...");
    color = COLOR_VALUE_WARN;
  } else if (snap.syncDegraded) {
    snprintf(line, sizeof(line), "Enlace ESP: DEGRADADO (sin red)");
    color = COLOR_VALUE_BAD;
  } else {
    snprintf(line, sizeof(line), "Enlace ESP: SINCRONIZADO");
    color = COLOR_VALUE_OK;
  }
  if (g_forceRedrawAll || strcmp(line, g_lastRowText[0]) != 0) {
    drawDynamicRow(0, line, color);
    strncpy(g_lastRowText[0], line, STATUS_ROW_MAXLEN);
    g_lastRowText[0][STATUS_ROW_MAXLEN] = '\0';
  }

  // Fila 2: datos de red (modo/IP/MAC), tal como los reportó el ESP en
  // FRAME_TYPE_NET_INFO. El gateway y el SSID no se muestran aquí para no
  // competir por espacio con el resto de campos; quedan registrados en el
  // log al llegar el NET_INFO (ver displayLogf en handleNetInfo(),
  // mega_z21.ino) -- pero modo+IP+MAC caben de sobra en una sola fila con
  // la pantalla en apaisado, así que se muestran los tres juntos.
  if (snap.netInfoMode == 0xFF) {
    snprintf(line, sizeof(line), "Red: esperando datos...");
    color = COLOR_VALUE_WARN;
  } else {
    const char *modeStr = (snap.netInfoMode == NET_INFO_MODE_AP) ? "AP" : "STA";
    snprintf(line, sizeof(line), "Red: %s %u.%u.%u.%u  MAC %02X:%02X:%02X:%02X:%02X:%02X",
             modeStr,
             (unsigned)(snap.netInfoIp & 0xFF),
             (unsigned)((snap.netInfoIp >> 8) & 0xFF),
             (unsigned)((snap.netInfoIp >> 16) & 0xFF),
             (unsigned)((snap.netInfoIp >> 24) & 0xFF),
             (unsigned)snap.netInfoMac[0], (unsigned)snap.netInfoMac[1],
             (unsigned)snap.netInfoMac[2], (unsigned)snap.netInfoMac[3],
             (unsigned)snap.netInfoMac[4], (unsigned)snap.netInfoMac[5]);
    color = COLOR_VALUE_OK;
  }
  if (g_forceRedrawAll || strcmp(line, g_lastRowText[1]) != 0) {
    drawDynamicRow(1, line, color);
    strncpy(g_lastRowText[1], line, STATUS_ROW_MAXLEN);
    g_lastRowText[1][STATUS_ROW_MAXLEN] = '\0';
  }

  // Fila 3: contadores de frames del enlace Mega<->ESP (ver AGENT.md,
  // "Diagnóstico y watchdog"). Saturan en 255 igual que en mega_z21.ino.
  snprintf(line, sizeof(line), "Frames(1s): OK=%u BAD=%u CHK=%u",
           (unsigned)snap.framesRxOk, (unsigned)snap.framesRxBad,
           (unsigned)snap.framesRxChkFail);
  color = (snap.framesRxBad == 0 && snap.framesRxChkFail == 0) ? COLOR_VALUE_OK : COLOR_VALUE_BAD;
  if (g_forceRedrawAll || strcmp(line, g_lastRowText[2]) != 0) {
    drawDynamicRow(2, line, color);
    strncpy(g_lastRowText[2], line, STATUS_ROW_MAXLEN);
    g_lastRowText[2][STATUS_ROW_MAXLEN] = '\0';
  }

  // Fila 4: estado de la última locomotora controlada y estado global.
  if (snap.locoValid) {
    snprintf(line, sizeof(line), "Loco#%u %s V=%u F0=%u %s",
             (unsigned)snap.locoAddress,
             locoDirectionLabel(snap.locoForward),
             (unsigned)(snap.locoSpeedByte & 0x7F),
             (unsigned)snap.locoF0,
             statusCodeLabel(snap.statusCode));
  } else {
    snprintf(line, sizeof(line), "Loco: sin datos  %s",
             statusCodeLabel(snap.statusCode));
  }
  color = snap.locoValid ? COLOR_VALUE_OK : statusCodeColor(snap.statusCode);
  if (g_forceRedrawAll || strcmp(line, g_lastRowText[3]) != 0) {
    drawDynamicRow(3, line, color);
    strncpy(g_lastRowText[3], line, STATUS_ROW_MAXLEN);
    g_lastRowText[3][STATUS_ROW_MAXLEN] = '\0';
  }

  g_forceRedrawAll = false;
}
