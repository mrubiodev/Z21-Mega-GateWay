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
// ---------------------------------------------------------------------
#define HEADER_HEIGHT_RATIO 0.42f

// ---------------------------------------------------------------------
// Panel de log: nº de líneas que se guardan en el buffer circular y
// longitud máxima de cada línea (se trunca lo que no quepa). El nº de
// líneas realmente VISIBLES a la vez se recalcula en tiempo de ejecución
// según el alto real del panel (ver display_log_panel.cpp), por si este
// shield concreto tiene menos resolución de la esperada.
// ---------------------------------------------------------------------
#define LOG_MAX_LINES   8
#define LOG_LINE_MAXLEN 48

#endif // DISPLAY_THEME_H
