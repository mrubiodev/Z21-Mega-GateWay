/*
 * display_manager.h
 * -------------------
 * Fachada del módulo de pantalla: es el ÚNICO header que mega_z21.ino
 * necesita incluir para usar la pantalla. Por debajo orquesta
 * display_driver, display_layout, display_status_panel y
 * display_log_panel, pero mega_z21.ino no necesita conocer ni incluir
 * ninguno de esos ficheros directamente — así se puede reorganizar la
 * implementación interna del módulo sin tocar el sketch principal.
 *
 * Uso desde mega_z21.ino:
 *   setup(): displayInit();
 *   loop():  displayTick(snapshot);  // snapshot ya throttleado internamente
 *   en los puntos de interés: displayLog("..."), displayLogF(F("...")),
 *   o displayLogf("formato %d", valor) para líneas con datos variables.
 *
 * Historial de cambios:
 *   - v0.1 (2026-08-02): Creación inicial del gestor/fachada.
 */

#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include "display_types.h"
#include <Arduino.h>

// Inicializa todo el módulo de pantalla (driver, layout, cabecera y
// panel de log) y deja la primera pantalla ya pintada. Llamar una única
// vez desde setup(), después de tener DEBUG_SERIAL disponible si se
// quiere depurar en paralelo.
void displayInit();

// Actualiza la cabecera de estado con los datos de `snap`. Se puede (y
// se debe) llamar en cada vuelta de loop() sin preocuparse por el coste:
// internamente se throttlea (ver STATUS_REFRESH_INTERVAL_MS en
// display_manager.cpp) y además solo redibuja los campos que hayan
// cambiado de verdad (ver display_status_panel.cpp).
void displayTick(const DisplayStatusSnapshot &snap);

// Añade una línea al panel de log. Tres variantes según de dónde venga
// el texto:
void displayLog(const char *line);                  // cadena ya construida en RAM
void displayLogF(const __FlashStringHelper *line);   // literal estático: displayLogF(F("..."))
void displayLogf(const char *fmt, ...);              // formato tipo printf

#endif // DISPLAY_MANAGER_H
