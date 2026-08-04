/*
 * display_theme.h
 * ---------------
 * Constantes puramente visuales del módulo de pantalla: colores, tamaños
 * de texto y proporciones de layout. Aislado en su propio fichero para
 * poder retocar el aspecto (p.ej. cambiar la paleta o el reparto de
 * alturas) sin tocar ningún fichero con lógica.
 *
 * Requiere las librerías Adafruit_GFX y MCUFRIEND_kbv (Library Manager
 * del IDE de Arduino) — las constantes de color TFT_* vienen de
 * MCUFRIEND_kbv.h.
 *
 * Historial de cambios:
 *   - v0.1 (2026-08-02): Creación inicial. Paleta y proporciones para la
 *     primera versión de la pantalla (apaisado, sin encoder).
 *   - v0.2 (2026-08-04): HEADER_HEIGHT_RATIO 0.42->0.34 y LOG_MAX_LINES
 *     8->24. El buffer de 8 líneas era el cuello de botella real (con el
 *     shield de referencia sobraba hueco en pantalla para ~19 líneas
 *     visibles y solo se aprovechaban 8) — ver comentarios junto a cada
 *     constante para el cálculo completo.
 */

#ifndef DISPLAY_THEME_H
#define DISPLAY_THEME_H

#include <MCUFRIEND_kbv.h>

// ---------------------------------------------------------------------
// Orientación: apaisado (landscape) fijo, sin touch ni encoder todavía.
// En los shields MCUFRIEND, 1 y 3 son las dos rotaciones horizontales
// posibles; cuál de las dos deja el texto "del lado correcto" depende de
// cómo esté fisicamente montado el shield. Si al arrancar la imagen sale
// girada 180º, cambiar SOLO este define a 3 — no tocar layout ni drivers.
// ---------------------------------------------------------------------
#define DISPLAY_ROTATION 1

// ---------------------------------------------------------------------
// Paleta. Los TFT_* están definidos en MCUFRIEND_kbv.h (Ctrl+click sobre
// cualquiera de ellos para ver la lista completa de colores disponibles).
// ---------------------------------------------------------------------
#define COLOR_BG            TFT_BLACK
#define COLOR_TITLE         TFT_CYAN
#define COLOR_LABEL         0x7BEF // gris medio (RGB565), para las etiquetas fijas
#define COLOR_VALUE_OK      TFT_GREEN
#define COLOR_VALUE_WARN    TFT_YELLOW
#define COLOR_VALUE_BAD     TFT_RED
#define COLOR_VALUE_NEUTRAL TFT_WHITE
#define COLOR_DIVIDER       TFT_ORANGE
#define COLOR_LOG_BG        TFT_BLACK
#define COLOR_LOG_TEXT      TFT_WHITE
#define COLOR_LOG_TITLE     TFT_ORANGE

// ---------------------------------------------------------------------
// Tamaños de texto. En Adafruit_GFX cada unidad de setTextSize() equivale
// a una celda de carácter de 6x8 px, multiplicada por el tamaño (p.ej.
// tamaño 2 = 12x16 px por carácter).
// ---------------------------------------------------------------------
#define HEADER_TITLE_TEXT_SIZE 2 // fila superior: "Z21 MEGA GATEWAY vX.Y"
#define HEADER_FIELD_TEXT_SIZE 1 // filas de estado (enlace, red, frames...)
#define LOG_TEXT_SIZE          1 // panel de log inferior

// ---------------------------------------------------------------------
// Layout vertical: proporción de la pantalla (ya rotada a apaisado) que
// ocupa la cabecera de estado; el resto, hasta el borde inferior, es el
// panel de log de comunicación.
//
// 0.34 en vez del 0.42 original: la cabecera son 5 filas fijas (título +
// 4 de estado, ver STATUS_ROW_COUNT en display_status_panel.cpp) con
// texto de tamaño 1-2 (8-16 px) — con el shield de referencia del spec
// (3.5" TFT, 320 px de alto en apaisado tras rotar, ver Z21_EMULATOR_
// SPEC.md sección 9), 0.42 dejaba ~27 px por fila (mucho más margen del
// que hace falta para texto de 8-16 px) a costa de restarle esa altura
// al log. 0.34 deja ~22 px por fila —de sobra para leer cómodo— y le
// devuelve esa diferencia al panel de log, que es donde de verdad hace
// falta el espacio (ver AGENT.md, "facilitar evaluar si el sistema está
// listo y comunicando sin cable USB"). Si en el futuro se usa un shield
// de menos de ~280 px de alto, revisar aquí primero si las filas de la
// cabecera se ven apretadas.
// ---------------------------------------------------------------------
#define HEADER_HEIGHT_RATIO 0.34f

// ---------------------------------------------------------------------
// Panel de log: nº de líneas que se guardan en el buffer circular y
// longitud máxima de cada línea (se trunca lo que no quepa). El nº de
// líneas realmente VISIBLES a la vez se recalcula en tiempo de ejecución
// según el alto real del panel (ver display_log_panel.cpp: g_maxVisibleLines
// = min(lo que cabe en píxeles, LOG_MAX_LINES)), por si este shield
// concreto tiene menos resolución de la esperada.
//
// LOG_MAX_LINES subido de 8 a 24: 8 NO era una limitación de espacio en
// pantalla, era un límite del BUFFER que dejaba sin usar más de la mitad
// del panel aunque hubiera sitio de sobra. Con el shield de referencia
// (320 px de alto en apaisado) y HEADER_HEIGHT_RATIO=0.34 de arriba, el
// panel de log tiene altura para unas 19 líneas visibles a la vez
// (calculado como en displayLogPanelInit: alto_log / lineHeightPx, con
// lineHeightPx=10 px a LOG_TEXT_SIZE=1) — con LOG_MAX_LINES=8 se estaban
// desperdiciando más de 10 líneas de hueco visible en pantalla, que es
// justo el síntoma reportado ("entran más líneas, ampliar para usar todo
// el espacio disponible"). 24 deja margen incluso para un shield algo más
// alto de lo esperado, sin que el buffer vuelva a ser el cuello de
// botella. Coste en RAM: 24 * (LOG_LINE_MAXLEN+1) = 1176 bytes — sin
// problema en los 8 KB de SRAM del Mega2560 (ver Z21_EMULATOR_SPEC.md
// sección 4, "con ~50 locomotoras el consumo es del orden de pocos KB").
// ---------------------------------------------------------------------
#define LOG_MAX_LINES   24
#define LOG_LINE_MAXLEN 48

#endif // DISPLAY_THEME_H
