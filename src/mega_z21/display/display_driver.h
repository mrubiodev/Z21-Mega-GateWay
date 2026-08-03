/*
 * display_driver.h
 * ----------------
 * Envoltorio fino sobre MCUFRIEND_kbv: es el ÚNICO fichero del módulo de
 * pantalla que sabe cómo se identifica e inicializa el shield físico. El
 * resto de ficheros (layout, status panel, log panel) usan el objeto
 * `tft` ya inicializado y las dimensiones ya rotadas — no repiten
 * readID()/begin()/setRotation() en ningún otro sitio.
 *
 * Historial de cambios:
 *   - v0.1 (2026-08-02): Creación inicial.
 */

#ifndef DISPLAY_DRIVER_H
#define DISPLAY_DRIVER_H

#include <MCUFRIEND_kbv.h>

// Objeto único de la pantalla, compartido por todo el módulo. Se declara
// aquí (extern) y se define una sola vez en display_driver.cpp.
extern MCUFRIEND_kbv tft;

// Detecta el controlador (readID), inicializa el bus, fija la rotación
// en apaisado (ver DISPLAY_ROTATION en display_theme.h) y limpia la
// pantalla. Debe llamarse una única vez, antes de cualquier otra función
// del módulo de pantalla.
void displayDriverInit();

// Dimensiones de la pantalla YA con la rotación aplicada (p.ej. en
// apaisado, width() > height() salvo shields muy poco comunes).
uint16_t displayWidth();
uint16_t displayHeight();

#endif // DISPLAY_DRIVER_H
